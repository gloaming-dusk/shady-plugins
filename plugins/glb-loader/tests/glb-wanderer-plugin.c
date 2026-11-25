/* Drive glb-wanderer against a fake host: it waits for its model, then hands
 * the host a posed scene that stands on the floor and moves as it ticks;
 * a settings change reloads it, a bad model clears its scene, and unloading
 * clears the scene too. The model is a two-bone "character" built here:
 * a box on a bone that an animation named Walk swings. */
#define _POSIX_C_SOURCE 200809L
/* The checks call the plugin inside assert(). */
#undef NDEBUG

#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <shady/environment.h>
#include <shady/plugin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int host_object;
#define HOST ((shady_host)&host_object)

static struct { const char *key; char value[512]; bool set; } values[8];
static int scenes_set, scenes_cleared, renders;
static bool scene_owned;
static float last_min_y, last_max_y, last_center[3];
static size_t last_triangles;

static const char *published_value(shady_host h, const char *key) {
    assert(h == HOST);
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i)
        if (values[i].set && !strcmp(values[i].key, key)) return values[i].value;
    return NULL;
}
static void publish(const char *key, const char *value) {
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i)
        if (!values[i].key || !strcmp(values[i].key, key)) {
            values[i].key = key;
            values[i].set = value != NULL;
            if (value) snprintf(values[i].value, sizeof(values[i].value), "%s", value);
            return;
        }
    abort();
}

static bool set_scene(shady_host h, const struct shady_environment_scene *s) {
    assert(h == HOST && s->struct_size >= sizeof(*s));
    assert(s->indices && s->index_count % 3 == 0 && s->index_count);
    last_min_y = INFINITY, last_max_y = -INFINITY;
    double c[3] = { 0, 0, 0 };
    for (size_t i = 0; i < s->index_count; ++i) assert(s->indices[i] < s->vertex_count);
    for (size_t i = 0; i < s->vertex_count; ++i) {
        const float *p = s->vertices[i].position;
        for (int k = 0; k < 3; ++k) {
            assert(isfinite(p[k]));
            c[k] += p[k];
        }
        if (p[1] < last_min_y) last_min_y = p[1];
        if (p[1] > last_max_y) last_max_y = p[1];
    }
    for (int k = 0; k < 3; ++k) last_center[k] = (float)(c[k] / s->vertex_count);
    last_triangles = s->index_count / 3;
    scenes_set++;
    scene_owned = true;
    return true;
}
static bool clear_scene(shady_host h) {
    assert(h == HOST && scene_owned);
    scene_owned = false;
    scenes_cleared++;
    return true;
}
static const struct shady_environment_api_v1 feature = {
    .struct_size = sizeof(feature), .set_scene = set_scene, .clear_scene = clear_scene,
};
static const void *query_api(shady_host h, const char *name, uint32_t version) {
    assert(h == HOST);
    return !strcmp(name, SHADY_ENVIRONMENT_API) && version == 1 ? &feature : NULL;
}
static char logs[8192];
static void host_log(enum shady_plugin_log_level level, const char *message) {
    (void)level;
    fprintf(stderr, "[plugin] %s\n", message);
    strncat(logs, message, sizeof(logs) - strlen(logs) - 2);
    strcat(logs, "\n");
}
static void schedule_render(shady_host h) { assert(h == HOST); renders++; }
static shady_render_callback frame_hook;
static shady_render_hook_id hook_add(shady_host h, uint32_t stage, shady_render_callback cb, void *u) {
    (void)u;
    assert(h == HOST && stage == SHADY_RENDER_STAGE_OVERLAY && !frame_hook);
    frame_hook = cb;
    return 3;
}
static bool hook_remove(shady_host h, shady_render_hook_id id) {
    assert(h == HOST && id == 3 && frame_hook);
    frame_hook = NULL;
    return true;
}
static struct shady_plugin_api_v1 api = {
    .abi_version = 1, .struct_size = sizeof(api), .query_api = query_api, .log = host_log,
    .published_value = published_value, .schedule_render = schedule_render,
    .render_hook_add = hook_add, .render_hook_remove = hook_remove,
};

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = { v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24 };
    fwrite(b, 1, 4, f);
}

/* A unit box (0..1 in y) on node "Leg" under "Hips", and a decoration node
 * "Hat" above it; Walk turns Leg about X back and forth, Idle holds it. */
static void write_character(const char *path) {
    static const float box[] = {
        -0.1f, 0, -0.1f, 0.1f, 0, -0.1f, 0.1f, 0, 0.1f, -0.1f, 0, 0.1f,
        -0.1f, 1, -0.1f, 0.1f, 1, -0.1f, 0.1f, 1, 0.1f, -0.1f, 1, 0.1f,
    };
    static const uint16_t idx[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    static const float times[] = { 0, 0.5f, 1 };
    static const float swing[] = { 0, 0, 0, 1, 0.38268343f, 0, 0, 0.92387953f, 0, 0, 0, 1 };
    static const float idle_t[] = { 0, 2 };
    static const float hold[] = { 0, 0, 0, 1, 0, 0, 0, 1 };
    uint8_t bin[512];
    size_t n = 0;
#define ADD(a) (memcpy(bin + n, a, sizeof(a)), n += (sizeof(a) + 3) & ~(size_t)3, n - ((sizeof(a) + 3) & ~(size_t)3))
    size_t o_box = ADD(box), o_idx = ADD(idx), o_t = ADD(times), o_s = ADD(swing),
        o_it = ADD(idle_t), o_h = ADD(hold);
#undef ADD
    char json[4096];
    snprintf(json, sizeof(json),
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":%zu}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":96},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":72},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":32}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"nodes\":[{\"name\":\"Hips\",\"children\":[1,2]},{\"name\":\"Leg\",\"mesh\":0},"
        "{\"name\":\"Hat\",\"mesh\":0,\"translation\":[0,5,0]}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"animations\":["
        "{\"name\":\"Walk\",\"samplers\":[{\"input\":2,\"output\":3}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}]},"
        "{\"name\":\"Idle\",\"samplers\":[{\"input\":4,\"output\":5,\"interpolation\":\"STEP\"}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}}]}]}",
        n, o_box, o_idx, o_t, o_s, o_it, o_h);
    size_t jl = strlen(json), jp = (jl + 3) & ~(size_t)3;
    FILE *f = fopen(path, "wb");
    assert(f);
    put32(f, 0x46546C67u);
    put32(f, 2);
    put32(f, (uint32_t)(12 + 8 + jp + 8 + n));
    put32(f, (uint32_t)jp);
    put32(f, 0x4E4F534Au);
    fwrite(json, 1, jl, f);
    for (size_t i = jl; i < jp; ++i) fputc(' ', f);
    put32(f, (uint32_t)n);
    put32(f, 0x004E4942u);
    fwrite(bin, 1, n, f);
    fclose(f);
}

static const struct shady_module *module;

/* A frame: the render hook, then the module tick, as the host runs them. */
static void run(double seconds) {
    for (double t = 0; t < seconds; t += 1.0 / 60.0) {
        frame_hook(HOST, NULL, NULL);
        module->tick((struct shady_server *)HOST, 1.f / 60.f, 1920.f, 1080.f);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    shady_plugin_entry_v1_fn entry =
        (shady_plugin_entry_v1_fn)dlsym(handle, SHADY_PLUGIN_ENTRY_V1);
    assert(entry);
    assert(!entry(2, &api, HOST));
    module = entry(1, &api, HOST);
    assert(module && !strcmp(module->name, "glb-wanderer"));

    char dir[256] = "/tmp/glb-wanderer-test-XXXXXX", path[300];
    if (getenv("TMPDIR")) snprintf(dir, sizeof(dir), "%s/glb-wanderer-test-XXXXXX", getenv("TMPDIR"));
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/character.glb", dir);
    write_character(path);

    /* No model yet: it loads, waits and keeps frames coming to see one. */
    assert(module->init((struct shady_server *)HOST));
    assert(strstr(logs, "waiting for"));
    run(0.5);
    assert(scenes_set == 0 && renders > 0);

    /* The model appears: 1 world unit tall, standing on the floor. The hat
     * sits 5 above, so hiding it changes the height measured. */
    publish("glb-wanderer.hide", "Hat");
    publish("glb-wanderer.height", "1");
    publish("glb-wanderer.speed", "2");
    publish("glb-wanderer.area", "-1,-2,1,0");
    publish("glb-wanderer.model", path);
    run(0.1);
    assert(scenes_set >= 1 && scene_owned && last_triangles == 12);
    assert(fabsf(last_min_y - -0.62f) < 1e-4f && fabsf(last_max_y - 0.38f) < 1e-4f);
    assert(fabsf(last_center[0]) < 1e-3f && fabsf(last_center[2] - -1.0f) < 1e-3f);

    /* Over a while she walks: the scene keeps coming, stays near the floor
     * (the swung box's corner dips 0.07 below it) and within the area. */
    int before = scenes_set;
    float moved = 0;
    for (int s = 0; s < 40; ++s) {
        float x = last_center[0], z = last_center[2];
        run(0.25);
        moved += hypotf(last_center[0] - x, last_center[2] - z);
        assert(last_min_y >= -0.62f - 0.08f);
        assert(last_center[0] > -1.6f && last_center[0] < 1.6f);
        assert(last_center[2] > -2.6f && last_center[2] < 0.6f);
    }
    assert(scenes_set > before + 200);
    assert(moved > 0.5f);

    /* Changing a setting reloads: with the hat shown she is measured
     * 6 model units tall, and the leg alone is a sixth of the height. */
    publish("glb-wanderer.hide", NULL);
    run(0.6);
    assert(last_triangles == 24);
    assert(fabsf(last_max_y - last_min_y - 1.0f) < 0.05f);

    /* A broken model clears the scene; a good one brings it back. */
    publish("glb-wanderer.model", "/nonexistent/character.glb");
    run(0.6);
    assert(!scene_owned && scenes_cleared == 1 && strstr(logs, "cannot open"));
    publish("glb-wanderer.model", path);
    run(0.6);
    assert(scene_owned);

    module->destroy((struct shady_server *)HOST);
    assert(!scene_owned && scenes_cleared == 2 && !frame_hook);
    unlink(path);
    rmdir(dir);
    dlclose(handle);
    fprintf(stderr, "glb-wanderer-plugin: PASS\n");
    return 0;
}
