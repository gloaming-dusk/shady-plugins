/* Drive the glb-loader plugin through its public contract against a fake
 * host: dlopen it, capture the loader it registers, and load .glb files built
 * here byte by byte. Covers the scene graph, every accessor form, primitive
 * modes, morph targets, skins, instancing, collision and hidden nodes, the
 * three buffer sources, and malformed input (also mutated at random). */
#define _POSIX_C_SOURCE 200809L
/* The checks call the loader inside assert(). */
#undef NDEBUG

#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <shady/environment.h>
#include <shady/plugin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static int host_object;
#define HOST ((shady_host)&host_object)

static const struct shady_environment_loader *registered;
static char last_log[2048];

static shady_environment_loader_id register_loader(shady_host h,
        const struct shady_environment_loader *loader) {
    assert(h == HOST && !registered);
    registered = loader;
    return 7;
}
static bool unregister_loader(shady_host h, shady_environment_loader_id id) {
    assert(h == HOST && id == 7 && registered);
    registered = NULL;
    return true;
}
static const struct shady_environment_api_v1 feature = {
    .struct_size = sizeof(feature),
    .register_loader = register_loader,
    .unregister_loader = unregister_loader,
};
static const void *query_api(shady_host h, const char *name, uint32_t version) {
    assert(h == HOST);
    return !strcmp(name, SHADY_ENVIRONMENT_API) && version == 1 ? &feature : NULL;
}
static bool quiet;
static void host_log(enum shady_plugin_log_level level, const char *message) {
    (void)level;
    snprintf(last_log, sizeof(last_log), "%s", message);
    if (!quiet) fprintf(stderr, "[plugin] %s\n", message);
}
static struct shady_plugin_api_v1 api = {
    .abi_version = 1, .struct_size = sizeof(api), .query_api = query_api, .log = host_log,
};

/* ---- building .glb files ---------------------------------------------- */

struct doc {
    uint8_t bin[1 << 16];
    size_t bin_length;
    char views[1 << 14];
    int view_count;
    char accessors[1 << 14];
    int accessor_count;
};

static void doc_init(struct doc *d) { memset(d, 0, sizeof(*d)); }

static void append(char *buf, size_t size, const char *fmt, ...) {
    size_t n = strlen(buf);
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + n, size - n, fmt, args);
    va_end(args);
}

static int add_view(struct doc *d, const void *data, size_t length, size_t stride) {
    d->bin_length = (d->bin_length + 3) & ~(size_t)3;
    assert(d->bin_length + length <= sizeof(d->bin));
    memcpy(d->bin + d->bin_length, data, length);
    append(d->views, sizeof(d->views), "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu",
        d->view_count ? "," : "", d->bin_length, length);
    if (stride) append(d->views, sizeof(d->views), ",\"byteStride\":%zu", stride);
    append(d->views, sizeof(d->views), "}");
    d->bin_length += length;
    return d->view_count++;
}

/* view < 0: no bufferView. extra: more members, e.g. a sparse object. */
static int add_accessor(struct doc *d, int view, size_t offset, int component_type,
        size_t count, const char *type, bool normalized, const char *extra) {
    append(d->accessors, sizeof(d->accessors), "%s{", d->accessor_count ? "," : "");
    if (view >= 0)
        append(d->accessors, sizeof(d->accessors), "\"bufferView\":%d,\"byteOffset\":%zu,",
            view, offset);
    append(d->accessors, sizeof(d->accessors),
        "\"componentType\":%d,\"count\":%zu,\"type\":\"%s\"%s%s%s}", component_type, count,
        type, normalized ? ",\"normalized\":true" : "", extra ? "," : "", extra ? extra : "");
    return d->accessor_count++;
}

static int floats(struct doc *d, const float *v, size_t count, const char *type) {
    size_t n = !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 :
        !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4 : 16;
    int view = add_view(d, v, count * n * sizeof(float), 0);
    return add_accessor(d, view, 0, 5126, count, type, false, NULL);
}

static int indices16(struct doc *d, const uint16_t *v, size_t count) {
    int view = add_view(d, v, count * sizeof(*v), 0);
    return add_accessor(d, view, 0, 5123, count, "SCALAR", false, NULL);
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = { v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 };
    fwrite(b, 1, 4, f);
}

static char dir[256];

static const char *write_raw_glb(const char *name, const char *json, const void *bin,
        size_t bin_length) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    size_t json_length = strlen(json), json_padded = (json_length + 3) & ~(size_t)3;
    size_t bin_padded = (bin_length + 3) & ~(size_t)3;
    size_t total = 12 + 8 + json_padded + (bin ? 8 + bin_padded : 0);
    FILE *f = fopen(path, "wb");
    assert(f);
    put32(f, 0x46546C67u);
    put32(f, 2);
    put32(f, (uint32_t)total);
    put32(f, (uint32_t)json_padded);
    put32(f, 0x4E4F534Au);
    fwrite(json, 1, json_length, f);
    for (size_t i = json_length; i < json_padded; ++i) fputc(' ', f);
    if (bin) {
        put32(f, (uint32_t)bin_padded);
        put32(f, 0x004E4942u);
        fwrite(bin, 1, bin_length, f);
        for (size_t i = bin_length; i < bin_padded; ++i) fputc(0, f);
    }
    fclose(f);
    return path;
}

/* rest: the document's other top-level members, without braces. */
static const char *write_glb(struct doc *d, const char *name, const char *rest) {
    static char json[1 << 16];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"glb-loader test\"},"
        "\"buffers\":[{\"byteLength\":%zu}],\"bufferViews\":[%s],\"accessors\":[%s]%s%s}",
        d->bin_length, d->views, d->accessors, rest[0] ? "," : "", rest);
    return write_raw_glb(name, json, d->bin, d->bin_length);
}

/* ---- loading ----------------------------------------------------------- */

static struct shady_environment_scene scene;

static bool load(const char *path) {
    scene = (struct shady_environment_scene){ .struct_size = sizeof(scene) };
    last_log[0] = '\0';
    return registered->load(HOST, path, &scene, registered->user_data);
}

static void release(void) {
    registered->release(HOST, &scene, registered->user_data);
}

static void expect_failure(const char *path, const char *message) {
    assert(!load(path));
    if (!strstr(last_log, message)) {
        fprintf(stderr, "expected '%s' in '%s'\n", message, last_log);
        abort();
    }
}

/* Scene as the host draws it: every index in range, a triangle list. */
static size_t triangles(void) {
    assert(scene.indices && scene.index_count % 3 == 0);
    for (size_t i = 0; i < scene.index_count; ++i) assert(scene.indices[i] < scene.vertex_count);
    return scene.index_count / 3;
}

static const struct shady_environment_vertex *corner(size_t triangle, int k) {
    return &scene.vertices[scene.indices[triangle * 3 + k]];
}

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static bool near3(const float *v, float x, float y, float z) {
    return near(v[0], x) && near(v[1], y) && near(v[2], z);
}

/* Normal of the triangle from its winding. */
static void winding_normal(size_t t, float n[3]) {
    const float *a = corner(t, 0)->position, *b = corner(t, 1)->position, *c = corner(t, 2)->position;
    float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    n[0] /= l; n[1] /= l; n[2] /= l;
}

/* A unit quad in the XY plane facing +Z: 4 vertices, 2 triangles. */
static const float quad_pos[] = { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 };
static const float quad_nrm[] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
static const float quad_uv[] = { 0, 1, 1, 1, 1, 0, 0, 0 };
static const uint16_t quad_idx[] = { 0, 1, 2, 0, 2, 3 };

/* ---- cases ------------------------------------------------------------- */

/* Material base colours times COLOR_0, converted from linear to sRGB; and
 * a host whose scene struct predates colors gets none written. */
static void check_colors(void) {
#ifdef SHADY_ENVIRONMENT_SCENE_HAS
    struct doc d;
    doc_init(&d);
    const float tri[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const float vc[] = { 1, 1, 1, 0.5f, 0.5f, 0.5f, 0, 0, 0 };
    int pos = floats(&d, tri, 3, "VEC3");
    int col = floats(&d, vc, 3, "VEC3");
    char rest[1024];
    snprintf(rest, sizeof(rest),
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.2140411,0,0.5]}}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":%d,\"COLOR_0\":%d},\"material\":0},"
        "{\"attributes\":{\"POSITION\":%d}}]}],"
        "\"nodes\":[{\"mesh\":0}]", pos, col, pos);
    const char *path = write_glb(&d, "colors.glb", rest);
    assert(load(path));
    assert(triangles() == 2 && scene.colors);
    /* Linear 0.2140411 is sRGB 0.5. Alpha is how shaded a vertex is: fully,
     * whatever the material's alpha. */
    const float *c = scene.colors[scene.indices[0]].rgba;
    assert(near(c[0], 1) && near(c[1], 0.5f) && near(c[2], 0) && near(c[3], 1));
    c = scene.colors[scene.indices[1]].rgba; /* times COLOR_0 0.5 */
    assert(near(c[0], 0.7353569f) && near(c[3], 1));
    c = scene.colors[scene.indices[3]].rgba; /* no material: white */
    assert(near(c[0], 1) && near(c[1], 1) && near(c[2], 1) && near(c[3], 1));
    release();

    /* An old host's struct is shorter: nothing may land past it. */
    struct { struct shady_environment_scene scene; } old;
    memset(&old, 0xAB, sizeof(old));
    old.scene.struct_size = SHADY_ENVIRONMENT_SCENE_V1_SIZE;
    assert(registered->load(HOST, path, &old.scene, registered->user_data));
    const unsigned char *tail = (const unsigned char *)&old.scene + SHADY_ENVIRONMENT_SCENE_V1_SIZE;
    for (size_t i = 0; i < sizeof(old.scene) - SHADY_ENVIRONMENT_SCENE_V1_SIZE; ++i)
        assert(tail[i] == 0xAB);
    assert(old.scene.vertex_count == 6);
    registered->release(HOST, &old.scene, registered->user_data);
    for (size_t i = 0; i < sizeof(old.scene) - SHADY_ENVIRONMENT_SCENE_V1_SIZE; ++i)
        assert(tail[i] == 0xAB);
#endif /* older Shady headers have no colours */
}

#ifdef SHADY_ENVIRONMENT_SCENE_TOON
/* A PNG of w x h RGBA8 pixels (rows top first), built with zlib. */
static size_t make_png(const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out) {
    uint8_t raw[1024], z[1024];
    size_t r = 0;
    for (uint32_t y = 0; y < h; ++y) {
        raw[r++] = 0; /* filter: none */
        memcpy(raw + r, rgba + (size_t)y * w * 4, (size_t)w * 4);
        r += (size_t)w * 4;
    }
    uLongf zl = sizeof(z);
    assert(compress(z, &zl, raw, (uLong)r) == Z_OK);
    size_t n = 0;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    n = 8;
#define BE32(v) (out[n++] = (uint8_t)((v) >> 24), out[n++] = (uint8_t)((v) >> 16), \
        out[n++] = (uint8_t)((v) >> 8), out[n++] = (uint8_t)(v))
#define CHUNK(type, data, len) do { \
        BE32((uint32_t)(len)); size_t at = n; memcpy(out + n, type, 4); n += 4; \
        memcpy(out + n, data, len); n += (len); \
        BE32((uint32_t)crc32(0, out + at, (uInt)((len) + 4))); } while (0)
    uint8_t ihdr[13] = { 0, 0, 0, (uint8_t)w, 0, 0, 0, (uint8_t)h, 8, 6, 0, 0, 0 };
    CHUNK("IHDR", ihdr, 13);
    CHUNK("IDAT", z, zl);
    CHUNK("IEND", "", 0);
#undef CHUNK
#undef BE32
    return n;
}
#endif

/* A base colour texture reaches the scene through the atlas: each corner of
 * a textured quad samples its own texel, an untextured triangle white. */
static void check_textures(void) {
#ifdef SHADY_ENVIRONMENT_SCENE_TOON
    struct doc d;
    doc_init(&d);
    static const uint8_t texels[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,   /* top: red, green */
        0, 0, 255, 255,   255, 255, 0, 255, /* bottom: blue, yellow */
    };
    uint8_t png[2048];
    size_t png_size = make_png(texels, 2, 2, png);
    int image_view = add_view(&d, png, png_size, 0);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    /* quad_pos: (0,0) (1,0) (1,1) (0,1) in x,y; uv top-left origin. */
    static const float uv[] = { 0, 1, 1, 1, 1, 0, 0, 0 };
    int tex = floats(&d, uv, 4, "VEC2");
    int idx = indices16(&d, quad_idx, 6);
    char rest[1024];
    snprintf(rest, sizeof(rest),
        "\"images\":[{\"bufferView\":%d,\"mimeType\":\"image/png\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d},"
        "\"indices\":%d,\"material\":0},{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]}],"
        "\"nodes\":[{\"mesh\":0}]", image_view, pos, tex, idx, pos, idx);
    assert(load(write_glb(&d, "texture.glb", rest)));
    assert(scene.texture && scene.texture_width >= 2 && scene.texture_height >= 2);
    const uint8_t expect[4][3] = { { 0, 0, 255 }, { 255, 255, 0 }, { 0, 255, 0 }, { 255, 0, 0 } };
    for (size_t i = 0; i < scene.index_count; ++i) {
        const struct shady_environment_vertex *v = &scene.vertices[scene.indices[i]];
        size_t x = (size_t)(v->uv[0] * scene.texture_width);
        size_t y = (size_t)((1.0f - v->uv[1]) * scene.texture_height);
        const uint8_t *t = scene.texture + (y * scene.texture_width + x) * 4;
        if (i < 6) {
            /* Which corner: quad_pos (x, y) -> uv row/column. */
            int corner = v->position[0] > 0.5f ? (v->position[1] > 0.5f ? 2 : 1) : (v->position[1] > 0.5f ? 3 : 0);
            assert(t[0] == expect[corner][0] && t[1] == expect[corner][1] && t[2] == expect[corner][2]);
        } else {
            assert(t[0] == 255 && t[1] == 255 && t[2] == 255 && t[3] == 255);
        }
    }
    release();
#endif
}

static void check_minimal(void) {
    struct doc d;
    doc_init(&d);
    /* No indices and no normals: a flat normal from the winding. */
    const float tri[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    int pos = floats(&d, tri, 3, "VEC3");
    char rest[512];
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d}}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0", pos);
    assert(load(write_glb(&d, "minimal.glb", rest)));
    assert(triangles() == 1 && scene.box_count == 0 && scene.triangle_count == 0);
    for (int k = 0; k < 3; ++k) assert(near3(corner(0, k)->normal, 0, 0, 1));
    assert(near3(corner(0, 1)->position, 1, 0, 0));
    release();
}

static void check_transforms(void) {
    struct doc d;
    doc_init(&d);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    int nrm = floats(&d, quad_nrm, 4, "VEC3");
    int uv = floats(&d, quad_uv, 4, "VEC2");
    int idx = indices16(&d, quad_idx, 6);
    /* Parent: rotate 90 degrees about Y (+Z turns to +X), then translate.
     * Child: scale 2 and translate (matrix form). Two scenes; the second is
     * the default. */
    char rest[2048];
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,"
        "\"TEXCOORD_0\":%d},\"indices\":%d,\"mode\":4}]}],"
        "\"nodes\":["
        "{\"name\":\"parent\",\"rotation\":[0,0.70710678,0,0.70710678],\"translation\":[10,0,0],"
        "\"children\":[1]},"
        "{\"mesh\":0,\"matrix\":[2,0,0,0, 0,2,0,0, 0,0,2,0, 0,0,-1,1]},"
        "{\"mesh\":0,\"translation\":[100,0,0]}],"
        "\"scenes\":[{\"nodes\":[2]},{\"nodes\":[0]}],\"scene\":1",
        pos, nrm, uv, idx);
    assert(load(write_glb(&d, "transforms.glb", rest)));
    assert(triangles() == 2 && scene.vertex_count == 4);
    /* Local (1,1,0) -> scaled (2,2,0) -> moved (2,2,-1) -> rotated (-1,2,-2)
     * -> translated (9,2,-2). The normal +Z turns to +X. */
    const struct shady_environment_vertex *v = corner(0, 2);
    assert(near3(v->position, 9, 2, -2));
    assert(near3(v->normal, 1, 0, 0));
    /* glTF's v (top-left origin) is flipped: (1,0) -> (1,1). */
    assert(near(v->uv[0], 1) && near(v->uv[1], 1));
    float n[3];
    winding_normal(0, n);
    assert(near3(n, 1, 0, 0));
    release();

    /* A mirrored node: the winding is reversed so faces still point along
     * their (inverse-transpose transformed) normals. */
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},"
        "\"indices\":%d}]}],\"nodes\":[{\"mesh\":0,\"scale\":[1,1,-3]}]", pos, nrm, idx);
    assert(load(write_glb(&d, "mirror.glb", rest)));
    assert(triangles() == 2);
    for (size_t t = 0; t < 2; ++t) {
        winding_normal(t, n);
        assert(near3(n, 0, 0, -1) && near3(corner(t, 0)->normal, 0, 0, -1));
    }
    release();
}

static void check_modes(void) {
    struct doc d;
    doc_init(&d);
    /* A strip and a fan over the same quad, with 8-bit indices; a line and a
     * points primitive are skipped. */
    int pos = floats(&d, quad_pos, 4, "VEC3");
    const uint8_t strip[] = { 0, 1, 3, 2 }, fan[] = { 0, 1, 2, 3 };
    int sv = add_view(&d, strip, 4, 0), fv = add_view(&d, fan, 4, 0);
    int si = add_accessor(&d, sv, 0, 5121, 4, "SCALAR", false, NULL);
    int fi = add_accessor(&d, fv, 0, 5121, 4, "SCALAR", false, NULL);
    char rest[1024];
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":%d},\"indices\":%d,\"mode\":5},"
        "{\"attributes\":{\"POSITION\":%d},\"indices\":%d,\"mode\":6},"
        "{\"attributes\":{\"POSITION\":%d},\"mode\":1},"
        "{\"attributes\":{\"POSITION\":%d},\"mode\":0}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
        pos, si, pos, fi, pos, pos);
    assert(load(write_glb(&d, "modes.glb", rest)));
    assert(triangles() == 4);
    assert(strstr(last_log, "skipped 2 point, line"));
    /* Every triangle of both keeps the authored front face (+Z). */
    for (size_t t = 0; t < 4; ++t) {
        float n[3];
        winding_normal(t, n);
        assert(near3(n, 0, 0, 1));
    }
    /* Strip 0 1 3 2: triangle 1 is (s1, s3, s2) = vertices (1, 2, 3). */
    assert(near3(corner(1, 0)->position, 1, 0, 0) && near3(corner(1, 1)->position, 1, 1, 0));
    /* Fan triangle 0 is (1, 2, 0). */
    assert(near3(corner(2, 2)->position, 0, 0, 0));
    release();
}

static void check_accessor_forms(void) {
    struct doc d;
    doc_init(&d);
    /* Interleaved position+normal (stride 24), positions starting at an
     * accessor byteOffset; 32-bit indices. */
    float inter[4 * 6];
    for (int i = 0; i < 4; ++i) {
        memcpy(inter + i * 6, quad_pos + i * 3, 12);
        memcpy(inter + i * 6 + 3, quad_nrm + i * 3, 12);
    }
    int iv = add_view(&d, inter, sizeof(inter), 24);
    int pos = add_accessor(&d, iv, 0, 5126, 4, "VEC3", false, NULL);
    int nrm = add_accessor(&d, iv, 12, 5126, 4, "VEC3", false, NULL);
    const uint32_t idx32[] = { 0, 1, 2, 0, 2, 3 };
    int xv = add_view(&d, idx32, sizeof(idx32), 0);
    int idx = add_accessor(&d, xv, 0, 5125, 6, "SCALAR", false, NULL);

    /* Sparse: vertex 2 moves to z=5. */
    const uint8_t sparse_index[] = { 2 };
    const float sparse_value[] = { 1, 1, 5 };
    int spi = add_view(&d, sparse_index, 1, 0), spv = add_view(&d, sparse_value, 12, 0);
    char sparse[256];
    snprintf(sparse, sizeof(sparse),
        "\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":%d,\"componentType\":5121},"
        "\"values\":{\"bufferView\":%d}}", spi, spv);
    int sparse_pos = add_accessor(&d, iv, 0, 5126, 4, "VEC3", false, sparse);
    /* Sparse over zeros (no bufferView): only vertex 2 is non-zero. */
    int zero_pos = add_accessor(&d, -1, 0, 5126, 4, "VEC3", false, sparse);

    /* KHR_mesh_quantization: normalized int16 positions, scaled by the node. */
    /* Elements are padded to 8 bytes, the stride the spec requires. */
    const int16_t qpos[] = { 0, 0, 0, 32767, 0, 0, 32767, 32767, 0, 0, 32767, 0 };
    int16_t qpad[16] = {0};
    for (int i = 0; i < 4; ++i) memcpy(qpad + i * 4, qpos + i * 3, 6);
    int qpv = add_view(&d, qpad, sizeof(qpad), 8);
    int qp = add_accessor(&d, qpv, 0, 5122, 4, "VEC3", true, NULL);
    /* Normalized unsigned byte UVs. */
    const uint8_t quv[] = { 0, 255, 255, 255, 255, 0, 0, 0 };
    int quvv = add_view(&d, quv, sizeof(quv), 0);
    int qu = add_accessor(&d, quvv, 0, 5121, 4, "VEC2", true, NULL);

    char rest[2048];
    snprintf(rest, sizeof(rest),
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\",\"KHR_materials_unlit\"],"
        "\"meshes\":["
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d},\"indices\":%d}]}],"
        "\"nodes\":[{\"mesh\":0},{\"mesh\":1},{\"mesh\":2},{\"mesh\":3,\"scale\":[4,4,4]}],"
        "\"scenes\":[{\"nodes\":[0,1,2,3]}]",
        pos, nrm, idx, sparse_pos, nrm, idx, zero_pos, idx, qp, qu, idx);
    assert(load(write_glb(&d, "accessors.glb", rest)));
    assert(triangles() == 8);
    assert(near3(corner(0, 2)->position, 1, 1, 0) && near3(corner(0, 2)->normal, 0, 0, 1));
    assert(near3(corner(2, 2)->position, 1, 1, 5));
    assert(near3(corner(4, 0)->position, 0, 0, 0) && near3(corner(4, 2)->position, 1, 1, 5));
    assert(near3(corner(6, 2)->position, 4, 4, 0));
    assert(near(corner(6, 2)->uv[0], 1) && near(corner(6, 2)->uv[1], 1));
    release();
}

static void check_morph_skin_instancing(void) {
    struct doc d;
    doc_init(&d);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    int nrm = floats(&d, quad_nrm, 4, "VEC3");
    int idx = indices16(&d, quad_idx, 6);
    /* Morph target: everything up by 2, at weight 0.5 (node overrides mesh). */
    const float up[] = { 0, 2, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0 };
    int target = floats(&d, up, 4, "VEC3");

    /* Skin: vertices 0,3 on joint 0, vertices 1,2 on joint 1. Joint 1 sits at
     * x=1 with inverse bind -1 (bind pose), then moves to x=3 in the rest
     * pose, dragging its vertices +2 in x. */
    const uint8_t joints[] = { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
    const float weights[] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    int jv = add_view(&d, joints, sizeof(joints), 0);
    int ja = add_accessor(&d, jv, 0, 5121, 4, "VEC4", false, NULL);
    int wa = floats(&d, weights, 4, "VEC4");
    const float ibm[32] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 1 };
    int ibma = floats(&d, ibm, 2, "MAT4");

    /* Instancing: two copies at z=-10 and z=-20. */
    const float inst_t[] = { 0, 0, -10, 0, 0, -20 };
    int it = floats(&d, inst_t, 2, "VEC3");

    char rest[2048];
    snprintf(rest, sizeof(rest),
        "\"extensionsRequired\":[\"EXT_mesh_gpu_instancing\"],"
        "\"meshes\":["
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d,"
        "\"targets\":[{\"POSITION\":%d}]}],\"weights\":[1]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"JOINTS_0\":%d,"
        "\"WEIGHTS_0\":%d},\"indices\":%d}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]}],"
        "\"skins\":[{\"joints\":[2,3],\"inverseBindMatrices\":%d}],"
        "\"nodes\":["
        "{\"mesh\":0,\"weights\":[0.5]},"
        /* The skinned node's own transform is ignored, per the spec. */
        "{\"mesh\":1,\"skin\":0,\"translation\":[50,50,50]},"
        "{\"name\":\"root joint\",\"children\":[3]},"
        "{\"name\":\"tip joint\",\"translation\":[3,0,0]},"
        "{\"mesh\":2,\"extensions\":{\"EXT_mesh_gpu_instancing\":{\"attributes\":"
        "{\"TRANSLATION\":%d}}}}],"
        "\"scenes\":[{\"nodes\":[0,1,2,4]}]",
        pos, nrm, idx, target, pos, nrm, ja, wa, idx, pos, idx, ibma, it);
    assert(load(write_glb(&d, "deform.glb", rest)));
    assert(triangles() == 8);
    assert(near3(corner(0, 2)->position, 1, 2, 0));   /* morphed: y + 0.5 * 2 */
    assert(near3(corner(2, 0)->position, 0, 0, 0));   /* skinned, joint 0 */
    assert(near3(corner(2, 2)->position, 3, 1, 0));   /* skinned, joint 1 */
    assert(near3(corner(4, 2)->position, 1, 1, -10)); /* instance 0 */
    assert(near3(corner(6, 2)->position, 1, 1, -20)); /* instance 1 */
    release();
}

static void check_collision(void) {
    struct doc d;
    doc_init(&d);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    int idx = indices16(&d, quad_idx, 6);
    /* A drawn collision node with a child, a hidden collision proxy under a
     * hidden group (KHR_node_visibility), a collision node without geometry,
     * and a node hidden through extras. */
    char rest[2048];
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]}],"
        "\"nodes\":["
        "{\"name\":\"collision_floor\",\"mesh\":0,\"children\":[1]},"
        "{\"mesh\":0,\"translation\":[0,0,-1]},"
        "{\"name\":\"proxies\",\"extensions\":{\"KHR_node_visibility\":{\"visible\":false}},"
        "\"children\":[3]},"
        "{\"name\":\"collision_wall\",\"mesh\":0,\"translation\":[5,0,0]},"
        "{\"name\":\"collision_empty\"},"
        "{\"name\":\"decoration\",\"mesh\":0,\"extras\":{\"shady_hidden\":1}}],"
        "\"scenes\":[{\"nodes\":[0,2,4,5]}]", pos, idx);
    assert(load(write_glb(&d, "collision.glb", rest)));
    assert(triangles() == 4); /* collision_floor and its child */
    assert(scene.box_count == 2 && scene.triangle_count == 6);
    const struct shady_environment_box *b = scene.boxes;
    assert(near3(b[0].min, 0, 0, -1) && near3(b[0].max, 1, 1, 0));
    assert(near3(b[1].min, 5, 0, 0) && near3(b[1].max, 6, 1, 0));
    release();

    /* Only invisible collision: still a scene. */
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]}],"
        "\"nodes\":[{\"name\":\"collision_x\",\"mesh\":0,\"extras\":{\"shady_hidden\":true}}]",
        pos, idx);
    assert(load(write_glb(&d, "proxy.glb", rest)));
    assert(scene.index_count == 0 && scene.triangle_count == 2 && scene.box_count == 1);
    release();
}

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) |
            (i + 2 < n ? in[i + 2] : 0);
        out[o++] = base64_chars[v >> 18 & 63];
        out[o++] = base64_chars[v >> 12 & 63];
        out[o++] = i + 1 < n ? base64_chars[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? base64_chars[v & 63] : '=';
    }
    out[o] = '\0';
}

static void check_buffer_sources(void) {
    /* Buffer 0 is an external file with a space in its name, buffer 1 a
     * data: URI; no BIN chunk. */
    const float tri[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    char path[512];
    snprintf(path, sizeof(path), "%s/my tri.bin", dir);
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(tri, 1, sizeof(tri), f);
    fclose(f);
    const uint16_t idx[] = { 0, 2, 1 };
    char encoded[64], json[2048];
    base64((const uint8_t *)idx, sizeof(idx), encoded);
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.1\"},"
        "\"buffers\":[{\"uri\":\"my%%20tri.bin\",\"byteLength\":36},"
        "{\"uri\":\"data:application/octet-stream;base64,%s\",\"byteLength\":6}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":1,\"byteLength\":6}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"meshes\":[{\"name\":\"\\u00e9\\ud83d\\ude00\",\"primitives\":"
        "[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}", encoded);
    assert(load(write_raw_glb("external.glb", json, NULL, 0)));
    assert(triangles() == 1 && near3(corner(0, 1)->position, 0, 1, 0));
    assert(near3(corner(0, 0)->normal, 0, 0, -1));
    release();
    unlink(path);
    expect_failure(write_raw_glb("external.glb", json, NULL, 0), "cannot open");
}

static void check_errors(void) {
    expect_failure("/nonexistent/shady.glb", "cannot open");

    char path[512];
    snprintf(path, sizeof(path), "%s/text.glb", dir);
    FILE *f = fopen(path, "w");
    fputs("this is not a glb file at all", f);
    fclose(f);
    expect_failure(path, "not a binary glTF file");

    struct doc d;
    doc_init(&d);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    int idx = indices16(&d, quad_idx, 6);
    char rest[1024];
#define MESH "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d},\"indices\":%d}]}]"

    snprintf(rest, sizeof(rest), "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]");
    expect_failure(write_glb(&d, "draco.glb", rest), "without mesh compression");
    snprintf(rest, sizeof(rest), "\"extensionsRequired\":[\"VENDOR_magic\"]");
    expect_failure(write_glb(&d, "vendor.glb", rest), "unsupported extension VENDOR_magic");
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"children\":[1]},{\"children\":[0],\"mesh\":0}]",
        pos, idx);
    expect_failure(write_glb(&d, "cycle.glb", rest), "no triangles");
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"children\":[1]},{\"children\":[0],\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}]", pos, idx);
    expect_failure(write_glb(&d, "cycle-root.glb", rest), "is not a root");
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"children\":[2]},{\"children\":[2]},{\"mesh\":0}]",
        pos, idx);
    expect_failure(write_glb(&d, "two-parents.glb", rest), "more than one parent");
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"mesh\":0},{\"mesh\":3}]", pos, idx);
    expect_failure(write_glb(&d, "bad-mesh.glb", rest), "missing or invalid mesh");
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"mesh\":0}]", pos, pos);
    expect_failure(write_glb(&d, "float-indices.glb", rest), "expected 1 components");
    snprintf(rest, sizeof(rest), "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d}}]}],"
        "\"nodes\":[{\"mesh\":0}]", idx);
    expect_failure(write_glb(&d, "scalar-positions.glb", rest), "expected 3 components");

    /* An index past the vertices. */
    const uint16_t far_idx[] = { 0, 1, 9 };
    int fi = indices16(&d, far_idx, 3);
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"mesh\":0}]", pos, fi);
    expect_failure(write_glb(&d, "far-index.glb", rest), "out of range");
    /* An accessor running past its bufferView. */
    int view = add_view(&d, quad_pos, sizeof(quad_pos), 0);
    int big = add_accessor(&d, view, 4, 5126, 4, "VEC3", false, NULL);
    snprintf(rest, sizeof(rest), MESH ",\"nodes\":[{\"mesh\":0}]", big, idx);
    expect_failure(write_glb(&d, "overrun.glb", rest), "lies outside bufferView");
    snprintf(rest, sizeof(rest), "\"meshes\":[],\"nodes\":[]");
    expect_failure(write_glb(&d, "empty.glb", rest), "no triangles");

    expect_failure(write_raw_glb("version.glb", "{\"asset\":{\"version\":\"1.0\"}}", NULL, 0),
        "version 1.0");
    expect_failure(write_raw_glb("json.glb", "{\"asset\":{\"version\":\"2.0\"},}", NULL, 0),
        "invalid JSON");
    expect_failure(write_raw_glb("nan.glb", "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"mesh\":0,"
        "\"translation\":[1,2]}],\"meshes\":[{\"primitives\":[]}]}", NULL, 0), "translation");
#undef MESH
}

/* Truncate and corrupt a valid file at random: every load must fail cleanly
 * or succeed with an in-range scene, never crash (run under ASan). */
static void check_mutations(void) {
    struct doc d;
    doc_init(&d);
    int pos = floats(&d, quad_pos, 4, "VEC3");
    int nrm = floats(&d, quad_nrm, 4, "VEC3");
    int idx = indices16(&d, quad_idx, 6);
    char rest[1024];
    snprintf(rest, sizeof(rest),
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},"
        "\"indices\":%d,\"mode\":5}]}],\"nodes\":[{\"name\":\"collision_a\",\"mesh\":0,"
        "\"children\":[1]},{\"mesh\":0,\"scale\":[1,-1,1]}],\"scenes\":[{\"nodes\":[0]}]",
        pos, nrm, idx);
    const char *path = write_glb(&d, "mutate.glb", rest);
    FILE *f = fopen(path, "rb");
    static uint8_t original[1 << 17], mutated[1 << 17];
    size_t n = fread(original, 1, sizeof(original), f);
    fclose(f);
    assert(load(path));
    release();

    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/mutated.glb", dir);
    srand(1234);
    quiet = true;
    for (int round = 0; round < 3000; ++round) {
        memcpy(mutated, original, n);
        size_t length = n;
        if (round % 4 == 0) length = (size_t)rand() % n;
        int flips = 1 + rand() % 4;
        for (int k = 0; k < flips && length; ++k) {
            size_t at = (size_t)rand() % length;
            mutated[at] = round % 3 ? (uint8_t)rand() : (uint8_t)"0123456789[]{},\":-e."[rand() % 20];
        }
        f = fopen(mpath, "wb");
        fwrite(mutated, 1, length, f);
        fclose(f);
        if (load(mpath)) {
            if (scene.index_count) (void)triangles();
            release();
        }
    }
    quiet = false;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    shady_plugin_entry_v1_fn entry =
        (shady_plugin_entry_v1_fn)dlsym(handle, SHADY_PLUGIN_ENTRY_V1);
    assert(entry);
    assert(!entry(2, &api, HOST));
    const struct shady_module *module = entry(1, &api, HOST);
    assert(module && !strcmp(module->name, "glb-loader"));
    assert(!strcmp(module->requires[0], "spatial"));

    assert(module->init((struct shady_server *)HOST) && registered);
    assert(!strcmp(registered->name, "glb"));
    assert(!strcmp(registered->extensions[0], "glb") && !registered->extensions[1]);

    snprintf(dir, sizeof(dir), "/tmp/glb-loader-test-XXXXXX");
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir) snprintf(dir, sizeof(dir), "%s/glb-loader-test-XXXXXX", tmpdir);
    assert(mkdtemp(dir));

    check_minimal();
    check_colors();
    check_textures();
    check_transforms();
    check_modes();
    check_accessor_forms();
    check_morph_skin_instancing();
    check_collision();
    check_buffer_sources();
    check_errors();
    check_mutations();

    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);

    module->destroy((struct shady_server *)HOST);
    assert(!registered);
    dlclose(handle);
    fprintf(stderr, "glb-loader-plugin: PASS\n");
    return 0;
}
