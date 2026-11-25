/*
 * glTF 2.0 binary (.glb) reader: see gltf.h.
 *
 * A model is flattened into world space:
 *  - node hierarchies with matrix or TRS transforms; a negative determinant
 *    reverses the winding, as the spec asks;
 *  - triangles, triangle strips and fans, indexed (8/16/32-bit) or not;
 *    points and lines are skipped;
 *  - every accessor form: byteStride, normalized integers, sparse storage,
 *    accessors without a bufferView, KHR_mesh_quantization;
 *  - morph targets at their default weights, skins in the given pose,
 *    EXT_mesh_gpu_instancing, KHR_node_visibility;
 *  - buffers in the BIN chunk, in data: URIs or in files next to the .glb.
 * Primitives without normals get flat normals. Animations are sampled on
 * request (LINEAR, STEP and CUBICSPLINE). Materials, textures, cameras and
 * lights do not change the geometry and are ignored.
 *
 * Collision is authored like the OBJ loader's: every node whose name starts
 * with collision_ contributes its subtree's triangles as exact collision and
 * their bounds as one box. A node with extras {"shady_hidden": true} (a Blender
 * custom property) or KHR_node_visibility "visible": false is not drawn, so a
 * collision proxy can be invisible.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gltf.h"

#define COLLISION_PREFIX "collision_"

#define GLB_MAGIC 0x46546C67u   /* "glTF" */
#define GLB_CHUNK_JSON 0x4E4F534Au
#define GLB_CHUNK_BIN 0x004E4942u

/* Generous bounds that keep every size computation far from overflow. */
#define MAX_FILE_SIZE ((size_t)1 << 30)
#define MAX_JSON_DEPTH 128
/* What the host draws in one buffer (see the environment module). */
#define MAX_DRAW_VERTICES ((size_t)16 * 1024 * 1024)
#define MAX_JOINT_SETS 4

/* ------------------------------------------------------------------------ */
/* Growable arrays                                                          */

struct array {
	void *data;
	size_t count;
	size_t capacity;
};

static bool array_reserve(struct array *a, size_t extra, size_t size) {
	if (extra > SIZE_MAX / size - a->count) return false;
	size_t need = a->count + extra;
	if (need <= a->capacity) return true;
	size_t next = a->capacity ? a->capacity : 256;
	while (next < need) next = next > SIZE_MAX / 2 ? need : next * 2;
	if (next > SIZE_MAX / size) return false;
	void *p = realloc(a->data, next * size);
	if (!p) return false;
	a->data = p;
	a->capacity = next;
	return true;
}

static bool array_push(struct array *a, const void *item, size_t size) {
	if (!array_reserve(a, 1, size)) return false;
	memcpy((char *)a->data + a->count * size, item, size);
	a->count++;
	return true;
}

/* ------------------------------------------------------------------------ */
/* JSON (RFC 8259)                                                          */

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json_member;

struct json {
	enum json_type type;
	size_t count; /* string length, array items or object members */
	union {
		bool boolean;
		double number;
		char *string;
		struct json *items;
		struct json_member *members;
	} u;
};

struct json_member {
	char *key;
	struct json value;
};

struct json_parser {
	const char *p, *end;
	int depth;
	const char *error;
};

static void json_free(struct json *v) {
	switch (v->type) {
	case JSON_STRING:
		free(v->u.string);
		break;
	case JSON_ARRAY:
		for (size_t i = 0; i < v->count; ++i) json_free(&v->u.items[i]);
		free(v->u.items);
		break;
	case JSON_OBJECT:
		for (size_t i = 0; i < v->count; ++i) {
			free(v->u.members[i].key);
			json_free(&v->u.members[i].value);
		}
		free(v->u.members);
		break;
	default:
		break;
	}
	*v = (struct json){0};
}

static void json_ws(struct json_parser *jp) {
	while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' ||
			*jp->p == '\n' || *jp->p == '\r'))
		jp->p++;
}

static bool json_fail(struct json_parser *jp, const char *error) {
	if (!jp->error) jp->error = error;
	return false;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool json_hex4(const char *p, const char *end, unsigned *out) {
	if (end - p < 4) return false;
	unsigned v = 0;
	for (int i = 0; i < 4; ++i) {
		int d = hex_digit(p[i]);
		if (d < 0) return false;
		v = v << 4 | (unsigned)d;
	}
	*out = v;
	return true;
}

static size_t utf8_put(char *out, unsigned cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Decoded text is never longer than its escaped form, so the raw length
 * bounds the allocation. */
static bool json_string(struct json_parser *jp, char **out, size_t *length) {
	if (jp->p >= jp->end || *jp->p != '"') return json_fail(jp, "expected a string");
	const char *start = ++jp->p, *q = start;
	while (q < jp->end && *q != '"') {
		if (*q == '\\') q++;
		q++;
	}
	if (q >= jp->end) return json_fail(jp, "unterminated string");

	char *s = malloc((size_t)(q - start) + 1);
	if (!s) return json_fail(jp, "out of memory");
	size_t n = 0;
	for (const char *p = start; p < q;) {
		unsigned char c = (unsigned char)*p;
		if (c < 0x20) {
			free(s);
			return json_fail(jp, "control character in string");
		}
		if (c != '\\') {
			s[n++] = (char)c;
			p++;
			continue;
		}
		char e = p[1];
		p += 2;
		switch (e) {
		case '"': s[n++] = '"'; break;
		case '\\': s[n++] = '\\'; break;
		case '/': s[n++] = '/'; break;
		case 'b': s[n++] = '\b'; break;
		case 'f': s[n++] = '\f'; break;
		case 'n': s[n++] = '\n'; break;
		case 'r': s[n++] = '\r'; break;
		case 't': s[n++] = '\t'; break;
		case 'u': {
			unsigned cp;
			if (!json_hex4(p, q, &cp)) goto bad_escape;
			p += 4;
			if (cp >= 0xD800 && cp < 0xDC00) {
				unsigned lo;
				if (q - p < 6 || p[0] != '\\' || p[1] != 'u' ||
						!json_hex4(p + 2, q, &lo) || lo < 0xDC00 || lo > 0xDFFF)
					goto bad_escape;
				p += 6;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				goto bad_escape;
			}
			n += utf8_put(s + n, cp);
			break;
		}
		default:
			goto bad_escape;
		}
	}
	s[n] = '\0';
	jp->p = q + 1;
	*out = s;
	*length = n;
	return true;

bad_escape:
	free(s);
	return json_fail(jp, "invalid escape in string");
}

static bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

/* Locale-independent: strtod would follow LC_NUMERIC. */
static bool json_number(struct json_parser *jp, double *out) {
	const char *p = jp->p, *end = jp->end;
	bool negative = false;
	if (p < end && *p == '-') {
		negative = true;
		p++;
	}
	if (p >= end || !is_digit(*p)) return json_fail(jp, "invalid number");

	uint64_t mantissa = 0;
	int digits = 0;
	long exponent = 0;
	if (*p == '0') {
		p++;
	} else {
		for (; p < end && is_digit(*p); ++p) {
			if (digits < 19) {
				mantissa = mantissa * 10 + (uint64_t)(*p - '0');
				if (mantissa) digits++;
			} else {
				exponent++;
			}
		}
	}
	if (p < end && *p == '.') {
		p++;
		if (p >= end || !is_digit(*p)) return json_fail(jp, "invalid number");
		for (; p < end && is_digit(*p); ++p) {
			if (digits < 19) {
				mantissa = mantissa * 10 + (uint64_t)(*p - '0');
				if (mantissa) digits++;
				exponent--;
			}
		}
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++;
		bool exp_negative = false;
		if (p < end && (*p == '+' || *p == '-')) exp_negative = *p++ == '-';
		if (p >= end || !is_digit(*p)) return json_fail(jp, "invalid number");
		long e = 0;
		for (; p < end && is_digit(*p); ++p)
			if (e < 100000) e = e * 10 + (*p - '0');
		exponent += exp_negative ? -e : e;
	}
	double v = (double)mantissa;
	if (mantissa && exponent) {
		if (exponent < -400) v = 0.0;
		else if (exponent > 400) v = HUGE_VAL;
		/* Two steps keep 10^exponent representable for tiny values. */
		else if (exponent < -300) v = v * pow(10.0, (double)(exponent + 100)) * 1e-100;
		else v *= pow(10.0, (double)exponent);
	}
	*out = negative ? -v : v;
	jp->p = p;
	return true;
}

static bool json_literal(struct json_parser *jp, const char *word) {
	size_t n = strlen(word);
	if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, word, n))
		return json_fail(jp, "invalid literal");
	jp->p += n;
	return true;
}

static bool json_value(struct json_parser *jp, struct json *out);

static bool json_array(struct json_parser *jp, struct json *out) {
	struct array items = {0};
	jp->p++;
	json_ws(jp);
	if (jp->p < jp->end && *jp->p == ']') {
		jp->p++;
		*out = (struct json){ .type = JSON_ARRAY };
		return true;
	}
	for (;;) {
		struct json item;
		if (!json_value(jp, &item)) goto fail;
		if (!array_push(&items, &item, sizeof(item))) {
			json_free(&item);
			json_fail(jp, "out of memory");
			goto fail;
		}
		json_ws(jp);
		if (jp->p < jp->end && *jp->p == ',') {
			jp->p++;
			continue;
		}
		if (jp->p < jp->end && *jp->p == ']') {
			jp->p++;
			break;
		}
		json_fail(jp, "expected ',' or ']'");
		goto fail;
	}
	*out = (struct json){ .type = JSON_ARRAY, .count = items.count, .u.items = items.data };
	return true;

fail:
	for (size_t i = 0; i < items.count; ++i) json_free(&((struct json *)items.data)[i]);
	free(items.data);
	return false;
}

static bool json_object(struct json_parser *jp, struct json *out) {
	struct array members = {0};
	jp->p++;
	json_ws(jp);
	if (jp->p < jp->end && *jp->p == '}') {
		jp->p++;
		*out = (struct json){ .type = JSON_OBJECT };
		return true;
	}
	for (;;) {
		struct json_member m = {0};
		size_t key_length;
		json_ws(jp);
		if (!json_string(jp, &m.key, &key_length)) goto fail;
		json_ws(jp);
		if (jp->p >= jp->end || *jp->p != ':') {
			free(m.key);
			json_fail(jp, "expected ':'");
			goto fail;
		}
		jp->p++;
		if (!json_value(jp, &m.value)) {
			free(m.key);
			goto fail;
		}
		if (!array_push(&members, &m, sizeof(m))) {
			free(m.key);
			json_free(&m.value);
			json_fail(jp, "out of memory");
			goto fail;
		}
		json_ws(jp);
		if (jp->p < jp->end && *jp->p == ',') {
			jp->p++;
			continue;
		}
		if (jp->p < jp->end && *jp->p == '}') {
			jp->p++;
			break;
		}
		json_fail(jp, "expected ',' or '}'");
		goto fail;
	}
	*out = (struct json){ .type = JSON_OBJECT, .count = members.count,
		.u.members = members.data };
	return true;

fail:
	for (size_t i = 0; i < members.count; ++i) {
		struct json_member *m = &((struct json_member *)members.data)[i];
		free(m->key);
		json_free(&m->value);
	}
	free(members.data);
	return false;
}

static bool json_value(struct json_parser *jp, struct json *out) {
	*out = (struct json){0};
	json_ws(jp);
	if (jp->p >= jp->end) return json_fail(jp, "unexpected end of JSON");
	bool ok;
	switch (*jp->p) {
	case '{':
	case '[':
		if (++jp->depth > MAX_JSON_DEPTH) return json_fail(jp, "JSON nested too deeply");
		ok = *jp->p == '{' ? json_object(jp, out) : json_array(jp, out);
		jp->depth--;
		return ok;
	case '"':
		out->type = JSON_STRING;
		return json_string(jp, &out->u.string, &out->count);
	case 't':
		*out = (struct json){ .type = JSON_BOOL, .u.boolean = true };
		return json_literal(jp, "true");
	case 'f':
		*out = (struct json){ .type = JSON_BOOL, .u.boolean = false };
		return json_literal(jp, "false");
	case 'n':
		return json_literal(jp, "null");
	default:
		out->type = JSON_NUMBER;
		return json_number(jp, &out->u.number);
	}
}

static bool json_parse(const char *text, size_t length, struct json *out, const char **error) {
	struct json_parser jp = { .p = text, .end = text + length };
	/* Tolerate a byte order mark although the spec forbids one. */
	if (length >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) jp.p += 3;
	if (!json_value(&jp, out)) {
		*error = jp.error ? jp.error : "invalid JSON";
		return false;
	}
	json_ws(&jp);
	/* The JSON chunk may be padded with spaces or, from some writers, NULs. */
	while (jp.p < jp.end && *jp.p == '\0') jp.p++;
	if (jp.p != jp.end) {
		json_free(out);
		*error = "trailing data after JSON";
		return false;
	}
	return true;
}

static const struct json *json_get(const struct json *object, const char *key) {
	if (!object || object->type != JSON_OBJECT) return NULL;
	for (size_t i = 0; i < object->count; ++i)
		if (!strcmp(object->u.members[i].key, key)) return &object->u.members[i].value;
	return NULL;
}

static const struct json *json_at(const struct json *array, size_t index) {
	if (!array || array->type != JSON_ARRAY || index >= array->count) return NULL;
	return &array->u.items[index];
}

static size_t json_length(const struct json *array) {
	return array && array->type == JSON_ARRAY ? array->count : 0;
}

static const char *json_str(const struct json *v) {
	return v && v->type == JSON_STRING ? v->u.string : NULL;
}

/* ------------------------------------------------------------------------ */
/* Loader state                                                             */

struct buffer {
	const uint8_t *data; /* NULL: the buffer has no data (e.g. a meshopt fallback) */
	size_t length;
	uint8_t *owned;
};

struct view {
	size_t buffer;
	size_t offset;
	size_t length;
	size_t stride; /* 0: tightly packed */
};

enum { NODE_PENDING, NODE_VISITING, NODE_DONE };

struct gltf {
	char *path;
	char error[512];

	uint8_t *file;
	size_t file_size;
	const uint8_t *bin;
	size_t bin_length;
	bool has_bin;

	struct json root;
	const struct json *accessors, *meshes, *nodes, *skins;
	size_t accessor_count, mesh_count, node_count, skin_count;

	struct buffer *buffers;
	size_t buffer_count;
	struct view *views;
	size_t view_count;

	ptrdiff_t *parent;
	size_t *chain;
	double (*world)[16];
	unsigned char *world_state;

	struct array vertices;  /* struct shady_environment_vertex */
	struct array indices;   /* uint32_t */
	struct array boxes;     /* struct shady_environment_box */
	struct array triangles; /* struct shady_environment_triangle */

	size_t skipped_primitives;
	size_t ignored_instancing;

	/* The build in progress. */
	const struct gltf_build_options *options;

	struct animation *animations;
	size_t animation_count;
};

static bool fail(struct gltf *g, const char *fmt, ...) {
	if (g->error[0]) return false;
	va_list args;
	va_start(args, fmt);
	vsnprintf(g->error, sizeof(g->error), fmt, args);
	va_end(args);
	return false;
}

/* A non-negative integer, at most max. */
static bool json_uint(const struct json *v, size_t max, size_t *out) {
	if (!v || v->type != JSON_NUMBER) return false;
	double n = v->u.number;
	if (!(n >= 0.0) || n != floor(n) || n > (double)max) return false;
	*out = (size_t)n;
	return true;
}

/* Optional integer member: absent gives def; present must be valid. */
static bool get_uint(struct gltf *g, const struct json *object, const char *key,
		size_t max, size_t def, size_t *out, const char *what) {
	const struct json *v = json_get(object, key);
	if (!v) {
		*out = def;
		return true;
	}
	if (!json_uint(v, max, out)) return fail(g, "%s: invalid %s", what, key);
	return true;
}

static bool get_index(struct gltf *g, const struct json *object, const char *key,
		size_t count, size_t *out, const char *what) {
	const struct json *v = json_get(object, key);
	if (!v || !json_uint(v, SIZE_MAX, out) || *out >= count)
		return fail(g, "%s: missing or invalid %s", what, key);
	return true;
}

static bool get_numbers(struct gltf *g, const struct json *object, const char *key,
		size_t n, double *out, const char *what) {
	const struct json *v = json_get(object, key);
	if (!v) return true;
	if (v->type != JSON_ARRAY || v->count != n)
		return fail(g, "%s: %s must be an array of %zu numbers", what, key, n);
	for (size_t i = 0; i < n; ++i) {
		if (v->u.items[i].type != JSON_NUMBER || !isfinite(v->u.items[i].u.number))
			return fail(g, "%s: %s must be an array of %zu numbers", what, key, n);
		out[i] = v->u.items[i].u.number;
	}
	return true;
}

static bool truthy(const struct json *v) {
	if (!v) return false;
	if (v->type == JSON_BOOL) return v->u.boolean;
	if (v->type == JSON_NUMBER) return v->u.number != 0.0;
	return false;
}

/* ------------------------------------------------------------------------ */
/* Files and buffers                                                        */

static bool read_file(const char *path, size_t min_size, uint8_t **out, size_t *size,
		char *error, size_t error_size) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
		return false;
	}
	struct stat st;
	if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) {
		snprintf(error, error_size, "%s is not a regular file", path);
		fclose(f);
		return false;
	}
	if ((uintmax_t)st.st_size > MAX_FILE_SIZE) {
		snprintf(error, error_size, "%s is larger than %zu MiB", path, MAX_FILE_SIZE >> 20);
		fclose(f);
		return false;
	}
	size_t n = (size_t)st.st_size;
	if (n < min_size) {
		snprintf(error, error_size, "%s is truncated", path);
		fclose(f);
		return false;
	}
	uint8_t *data = malloc(n ? n : 1);
	if (!data) {
		snprintf(error, error_size, "out of memory reading %s", path);
		fclose(f);
		return false;
	}
	if (n && fread(data, 1, n, f) != n) {
		snprintf(error, error_size, "cannot read %s", path);
		free(data);
		fclose(f);
		return false;
	}
	fclose(f);
	*out = data;
	*size = n;
	return true;
}

static uint16_t le16(const uint8_t *p) {
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float lef32(const uint8_t *p) {
	uint32_t bits = le32(p);
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static bool parse_glb(struct gltf *g, const char **json, size_t *json_length) {
	const uint8_t *d = g->file;
	if (g->file_size < 20 || le32(d) != GLB_MAGIC)
		return fail(g, "not a binary glTF file (no glTF header)");
	uint32_t version = le32(d + 4);
	if (version != 2) return fail(g, "glTF container version %u is not supported", version);
	size_t length = le32(d + 8);
	if (length > g->file_size || length < 20)
		return fail(g, "header length %zu does not match the file size %zu",
			length, g->file_size);

	size_t pos = 12;
	size_t chunk_length = le32(d + pos);
	if (le32(d + pos + 4) != GLB_CHUNK_JSON) return fail(g, "the first chunk is not JSON");
	if (chunk_length > length - pos - 8) return fail(g, "the JSON chunk is truncated");
	*json = (const char *)d + pos + 8;
	*json_length = chunk_length;
	pos += 8 + chunk_length;

	bool first = true;
	while (length - pos >= 8) {
		chunk_length = le32(d + pos);
		uint32_t type = le32(d + pos + 4);
		if (chunk_length > length - pos - 8) return fail(g, "a chunk is truncated");
		if (type == GLB_CHUNK_BIN) {
			if (!first) return fail(g, "the BIN chunk must follow the JSON chunk");
			g->bin = d + pos + 8;
			g->bin_length = chunk_length;
			g->has_bin = true;
		}
		/* Chunks of unknown type are skipped, as the spec asks. */
		first = false;
		pos += 8 + chunk_length;
	}
	return true;
}

static int base64_value(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

static bool base64_decode(const char *s, uint8_t **out, size_t *size) {
	size_t n = strlen(s);
	uint8_t *data = malloc(n / 4 * 3 + 3);
	if (!data) return false;
	size_t length = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c == '=') break;
		int v = base64_value(c);
		if (v < 0) {
			free(data);
			return false;
		}
		acc = acc << 6 | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			data[length++] = (uint8_t)(acc >> bits);
		}
	}
	*out = data;
	*size = length;
	return true;
}

static bool percent_decode(const char *s, char *out, size_t out_size) {
	size_t n = 0;
	for (; *s; ++s) {
		if (n + 1 >= out_size) return false;
		if (*s == '%') {
			int hi = hex_digit(s[1]), lo = hi >= 0 ? hex_digit(s[2]) : -1;
			if (lo < 0) return false;
			char c = (char)(hi << 4 | lo);
			if (!c) return false;
			out[n++] = c;
			s += 2;
		} else {
			out[n++] = *s;
		}
	}
	out[n] = '\0';
	return true;
}

static bool has_uri_scheme(const char *uri) {
	const char *p = uri;
	if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return false;
	while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			(*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')
		p++;
	return *p == ':';
}

static bool load_buffer_uri(struct gltf *g, size_t index, const char *uri, struct buffer *b) {
	size_t size = 0;
	if (!strncmp(uri, "data:", 5)) {
		const char *comma = strchr(uri, ',');
		if (!comma || comma - uri < 12 || memcmp(comma - 7, ";base64", 7))
			return fail(g, "buffer %zu: only base64 data URIs are supported", index);
		if (!base64_decode(comma + 1, &b->owned, &size))
			return fail(g, "buffer %zu: invalid base64 data", index);
	} else {
		if (has_uri_scheme(uri) || uri[0] == '/')
			return fail(g, "buffer %zu: only relative file URIs are supported", index);
		char relative[PATH_MAX], full[PATH_MAX];
		if (!percent_decode(uri, relative, sizeof(relative)))
			return fail(g, "buffer %zu: invalid URI", index);
		const char *slash = strrchr(g->path, '/');
		int n = slash ?
			snprintf(full, sizeof(full), "%.*s/%s", (int)(slash - g->path), g->path, relative) :
			snprintf(full, sizeof(full), "%s", relative);
		if (n < 0 || (size_t)n >= sizeof(full))
			return fail(g, "buffer %zu: path too long", index);
		char error[sizeof(g->error) - 32];
		if (!read_file(full, 0, &b->owned, &size, error, sizeof(error)))
			return fail(g, "buffer %zu: %s", index, error);
	}
	if (size < b->length) {
		return fail(g, "buffer %zu: holds %zu bytes, byteLength is %zu",
			index, size, b->length);
	}
	b->data = b->owned;
	return true;
}

static bool load_buffers(struct gltf *g) {
	const struct json *buffers = json_get(&g->root, "buffers");
	g->buffer_count = json_length(buffers);
	if (g->buffer_count) {
		g->buffers = calloc(g->buffer_count, sizeof(*g->buffers));
		if (!g->buffers) return fail(g, "out of memory");
	}
	for (size_t i = 0; i < g->buffer_count; ++i) {
		const struct json *b = json_at(buffers, i);
		struct buffer *out = &g->buffers[i];
		if (!get_uint(g, b, "byteLength", MAX_FILE_SIZE, SIZE_MAX, &out->length, "buffer") ||
				out->length == SIZE_MAX)
			return fail(g, "buffer %zu: missing or invalid byteLength", i);
		const char *uri = json_str(json_get(b, "uri"));
		if (uri) {
			if (!load_buffer_uri(g, i, uri, out)) return false;
		} else if (i == 0 && g->has_bin) {
			if (g->bin_length < out->length)
				return fail(g, "the BIN chunk holds %zu bytes, buffer 0 needs %zu",
					g->bin_length, out->length);
			out->data = g->bin;
		}
	}

	const struct json *views = json_get(&g->root, "bufferViews");
	g->view_count = json_length(views);
	if (g->view_count) {
		g->views = calloc(g->view_count, sizeof(*g->views));
		if (!g->views) return fail(g, "out of memory");
	}
	for (size_t i = 0; i < g->view_count; ++i) {
		const struct json *v = json_at(views, i);
		struct view *out = &g->views[i];
		if (!get_index(g, v, "buffer", g->buffer_count, &out->buffer, "bufferView") ||
				!get_uint(g, v, "byteOffset", MAX_FILE_SIZE, 0, &out->offset, "bufferView") ||
				!get_uint(g, v, "byteLength", MAX_FILE_SIZE, 0, &out->length, "bufferView") ||
				!get_uint(g, v, "byteStride", 252, 0, &out->stride, "bufferView"))
			return false;
		if (out->stride && (out->stride < 4 || out->stride % 4))
			return fail(g, "bufferView %zu: byteStride %zu is not allowed", i, out->stride);
		const struct buffer *b = &g->buffers[out->buffer];
		if (out->offset > b->length || out->length > b->length - out->offset)
			return fail(g, "bufferView %zu lies outside buffer %zu", i, out->buffer);
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/* Accessors                                                                */

enum {
	COMPONENT_BYTE = 5120,
	COMPONENT_UNSIGNED_BYTE = 5121,
	COMPONENT_SHORT = 5122,
	COMPONENT_UNSIGNED_SHORT = 5123,
	COMPONENT_UNSIGNED_INT = 5125,
	COMPONENT_FLOAT = 5126,
};

static size_t component_size(size_t type) {
	switch (type) {
	case COMPONENT_BYTE:
	case COMPONENT_UNSIGNED_BYTE: return 1;
	case COMPONENT_SHORT:
	case COMPONENT_UNSIGNED_SHORT: return 2;
	case COMPONENT_UNSIGNED_INT:
	case COMPONENT_FLOAT: return 4;
	default: return 0;
	}
}

static double read_component(const uint8_t *p, size_t type, bool normalized) {
	switch (type) {
	case COMPONENT_BYTE: {
		int8_t v = (int8_t)p[0];
		return normalized ? fmax(v / 127.0, -1.0) : v;
	}
	case COMPONENT_UNSIGNED_BYTE:
		return normalized ? p[0] / 255.0 : p[0];
	case COMPONENT_SHORT: {
		int16_t v = (int16_t)le16(p);
		return normalized ? fmax(v / 32767.0, -1.0) : v;
	}
	case COMPONENT_UNSIGNED_SHORT:
		return normalized ? le16(p) / 65535.0 : le16(p);
	case COMPONENT_UNSIGNED_INT:
		return le32(p);
	default:
		return lef32(p);
	}
}

static unsigned type_components(const char *type) {
	static const struct { const char *name; unsigned n; } types[] = {
		{ "SCALAR", 1 }, { "VEC2", 2 }, { "VEC3", 3 }, { "VEC4", 4 },
		{ "MAT2", 4 }, { "MAT3", 9 }, { "MAT4", 16 },
	};
	for (size_t i = 0; type && i < sizeof(types) / sizeof(types[0]); ++i)
		if (!strcmp(type, types[i].name)) return types[i].n;
	return 0;
}

/* Bytes `count` elements of `element` bytes, `stride` apart, need from offset
 * in a range of `length` bytes. */
static bool range_fits(size_t offset, size_t count, size_t stride, size_t element,
		size_t length) {
	if (!count) return offset <= length;
	if (offset > length || element > length - offset) return false;
	return (count - 1) <= (length - offset - element) / stride;
}

static const uint8_t *view_data(struct gltf *g, size_t index, const char *what) {
	const struct view *v = &g->views[index];
	const struct buffer *b = &g->buffers[v->buffer];
	if (!b->data) {
		fail(g, "%s uses buffer %zu, which has no data (compressed with an "
			"unsupported extension?)", what, v->buffer);
		return NULL;
	}
	return b->data + v->offset;
}

/* Read an accessor of `components` components per element as floats (out_f)
 * or as exact unsigned integers (out_u). The result is malloc'd. */
static bool read_accessor(struct gltf *g, size_t index, unsigned components,
		float **out_f, uint32_t **out_u, size_t *out_count, const char *what) {
	char label[96];
	snprintf(label, sizeof(label), "%s (accessor %zu)", what, index);
	const struct json *a = json_at(g->accessors, index);
	if (!a) return fail(g, "%s: no such accessor", label);

	size_t type, count;
	if (!get_index(g, a, "componentType", COMPONENT_FLOAT + 1, &type, label) ||
			!get_uint(g, a, "count", MAX_DRAW_VERTICES * 3, SIZE_MAX, &count, label))
		return false;
	size_t csize = component_size(type);
	if (!csize) return fail(g, "%s: invalid componentType %zu", label, type);
	if (count == SIZE_MAX || !count) return fail(g, "%s: missing or invalid count", label);
	/* Callers ask for SCALAR, VEC2, VEC3, VEC4 or MAT4; MAT2 and MAT3, whose
	 * columns may be padded, are never wanted. */
	const char *type_name = json_str(json_get(a, "type"));
	unsigned n = type_components(type_name);
	if (n != components || !strncmp(type_name, "MAT", 3) != (components == 16))
		return fail(g, "%s: expected %u components, accessor type is %s", label,
			components, type_name ? type_name : "missing");
	bool normalized = truthy(json_get(a, "normalized"));
	if (out_u && (type == COMPONENT_FLOAT || normalized))
		return fail(g, "%s: must hold unsigned integers", label);
	if (out_u && (type == COMPONENT_BYTE || type == COMPONENT_SHORT))
		return fail(g, "%s: must hold unsigned integers", label);
	size_t element = csize * n;

	float *f = NULL;
	uint32_t *u = NULL;
	if (out_f) f = calloc(count * n, sizeof(*f));
	else u = calloc(count * n, sizeof(*u));
	if (!f && !u) return fail(g, "%s: out of memory", label);

	const struct json *view_json = json_get(a, "bufferView");
	if (view_json) {
		size_t view, offset;
		if (!get_index(g, a, "bufferView", g->view_count, &view, label) ||
				!get_uint(g, a, "byteOffset", MAX_FILE_SIZE, 0, &offset, label))
			goto fail;
		const struct view *v = &g->views[view];
		size_t stride = v->stride ? v->stride : element;
		if (stride < element) {
			fail(g, "%s: byteStride %zu is smaller than an element", label, stride);
			goto fail;
		}
		if (!range_fits(offset, count, stride, element, v->length)) {
			fail(g, "%s lies outside bufferView %zu", label, view);
			goto fail;
		}
		const uint8_t *base = view_data(g, view, label);
		if (!base) goto fail;
		base += offset;
		for (size_t i = 0; i < count; ++i) {
			const uint8_t *e = base + i * stride;
			for (unsigned c = 0; c < n; ++c) {
				double value = read_component(e + c * csize, type, normalized);
				if (f) f[i * n + c] = (float)value;
				else u[i * n + c] = (uint32_t)value;
			}
		}
	}

	const struct json *sparse = json_get(a, "sparse");
	if (sparse) {
		const struct json *si = json_get(sparse, "indices");
		const struct json *sv = json_get(sparse, "values");
		size_t sparse_count, iview, ioffset, itype, vview, voffset;
		if (!get_uint(g, sparse, "count", count, SIZE_MAX, &sparse_count, label) ||
				sparse_count == SIZE_MAX || !sparse_count || !si || !sv) {
			fail(g, "%s: invalid sparse storage", label);
			goto fail;
		}
		if (!get_index(g, si, "bufferView", g->view_count, &iview, label) ||
				!get_uint(g, si, "byteOffset", MAX_FILE_SIZE, 0, &ioffset, label) ||
				!get_index(g, si, "componentType", COMPONENT_UNSIGNED_INT + 1, &itype, label) ||
				!get_index(g, sv, "bufferView", g->view_count, &vview, label) ||
				!get_uint(g, sv, "byteOffset", MAX_FILE_SIZE, 0, &voffset, label))
			goto fail;
		size_t isize = component_size(itype);
		if (itype != COMPONENT_UNSIGNED_BYTE && itype != COMPONENT_UNSIGNED_SHORT &&
				itype != COMPONENT_UNSIGNED_INT) {
			fail(g, "%s: invalid sparse index type", label);
			goto fail;
		}
		if (!range_fits(ioffset, sparse_count, isize, isize, g->views[iview].length) ||
				!range_fits(voffset, sparse_count, element, element, g->views[vview].length)) {
			fail(g, "%s: sparse storage lies outside its bufferViews", label);
			goto fail;
		}
		const uint8_t *ibase = view_data(g, iview, label);
		const uint8_t *vbase = ibase ? view_data(g, vview, label) : NULL;
		if (!vbase) goto fail;
		ibase += ioffset;
		vbase += voffset;
		for (size_t i = 0; i < sparse_count; ++i) {
			size_t target = (size_t)read_component(ibase + i * isize, itype, false);
			if (target >= count) {
				fail(g, "%s: sparse index %zu out of range", label, target);
				goto fail;
			}
			for (unsigned c = 0; c < n; ++c) {
				double value = read_component(vbase + i * element + c * csize, type, normalized);
				if (f) f[target * n + c] = (float)value;
				else u[target * n + c] = (uint32_t)value;
			}
		}
	}
	/* Without bufferView or sparse storage, every element is zero. */

	if (out_f) *out_f = f;
	else *out_u = u;
	*out_count = count;
	return true;

fail:
	free(f);
	free(u);
	return false;
}

/* Optional attribute: absent leaves *out NULL. */
static bool read_attribute(struct gltf *g, const struct json *attributes, const char *name,
		unsigned components, size_t expected, float **out, const char *what) {
	*out = NULL;
	const struct json *v = json_get(attributes, name);
	if (!v) return true;
	size_t index, count;
	if (!json_uint(v, SIZE_MAX, &index)) return fail(g, "%s: invalid %s", what, name);
	char label[64];
	snprintf(label, sizeof(label), "%s %s", what, name);
	if (!read_accessor(g, index, components, out, NULL, &count, label)) return false;
	if (count != expected) {
		free(*out);
		*out = NULL;
		return fail(g, "%s has %zu elements, POSITION has %zu", label, count, expected);
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/* Matrices (column-major, like glTF)                                       */

static void mat_identity(double m[16]) {
	memset(m, 0, 16 * sizeof(*m));
	m[0] = m[5] = m[10] = m[15] = 1.0;
}

static void mat_mul(double out[16], const double a[16], const double b[16]) {
	double r[16];
	for (int c = 0; c < 4; ++c)
		for (int row = 0; row < 4; ++row) {
			double s = 0.0;
			for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
			r[c * 4 + row] = s;
		}
	memcpy(out, r, sizeof(r));
}

static void mat_trs(double m[16], const double t[3], const double q_in[4], const double s[3]) {
	double x = q_in[0], y = q_in[1], z = q_in[2], w = q_in[3];
	double len = sqrt(x * x + y * y + z * z + w * w);
	if (len > 0.0) {
		x /= len;
		y /= len;
		z /= len;
		w /= len;
	} else {
		w = 1.0;
	}
	m[0] = (1 - 2 * (y * y + z * z)) * s[0];
	m[1] = (2 * (x * y + z * w)) * s[0];
	m[2] = (2 * (x * z - y * w)) * s[0];
	m[3] = 0;
	m[4] = (2 * (x * y - z * w)) * s[1];
	m[5] = (1 - 2 * (x * x + z * z)) * s[1];
	m[6] = (2 * (y * z + x * w)) * s[1];
	m[7] = 0;
	m[8] = (2 * (x * z + y * w)) * s[2];
	m[9] = (2 * (y * z - x * w)) * s[2];
	m[10] = (1 - 2 * (x * x + y * y)) * s[2];
	m[11] = 0;
	m[12] = t[0];
	m[13] = t[1];
	m[14] = t[2];
	m[15] = 1;
}

static double mat_det3(const double m[16]) {
	return m[0] * (m[5] * m[10] - m[9] * m[6]) -
		m[4] * (m[1] * m[10] - m[9] * m[2]) +
		m[8] * (m[1] * m[6] - m[5] * m[2]);
}

/* Normal matrix up to scale: the cofactor matrix of the upper 3x3 equals
 * det * inverse-transpose; multiplying by sign(det) keeps the orientation.
 * Row-major 3x3 output, to be applied as n' = N n. */
static void mat_normal(double n[9], const double m[16]) {
	double a = m[0], b = m[4], c = m[8];
	double d = m[1], e = m[5], f = m[9];
	double g = m[2], h = m[6], i = m[10];
	double sign = mat_det3(m) < 0.0 ? -1.0 : 1.0;
	n[0] = sign * (e * i - f * h);
	n[1] = sign * (f * g - d * i);
	n[2] = sign * (d * h - e * g);
	n[3] = sign * (c * h - b * i);
	n[4] = sign * (a * i - c * g);
	n[5] = sign * (b * g - a * h);
	n[6] = sign * (b * f - c * e);
	n[7] = sign * (c * d - a * f);
	n[8] = sign * (a * e - b * d);
}

static void transform_point(const double m[16], const float p[3], float out[3]) {
	for (int r = 0; r < 3; ++r)
		out[r] = (float)(m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r]);
}

static void transform_normal(const double n[9], const float v[3], float out[3]) {
	double x = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
	double y = n[3] * v[0] + n[4] * v[1] + n[5] * v[2];
	double z = n[6] * v[0] + n[7] * v[1] + n[8] * v[2];
	double len = sqrt(x * x + y * y + z * z);
	if (len > 1e-12) {
		x /= len;
		y /= len;
		z /= len;
	}
	out[0] = (float)x;
	out[1] = (float)y;
	out[2] = (float)z;
}

/* ------------------------------------------------------------------------ */
/* Nodes                                                                    */

static bool node_authored(struct gltf *g, size_t index, struct gltf_trs *out) {
	const struct json *node = json_at(g->nodes, index);
	char what[48];
	snprintf(what, sizeof(what), "node %zu", index);
	*out = (struct gltf_trs){ .r = { 0, 0, 0, 1 }, .s = { 1, 1, 1 } };
	if (json_get(node, "matrix")) {
		if (!get_numbers(g, node, "matrix", 16, out->m, what)) return false;
		/* glTF matrices are affine; ignore a stray projective row. */
		out->m[3] = out->m[7] = out->m[11] = 0.0;
		out->m[15] = 1.0;
		out->has_matrix = true;
		return true;
	}
	return get_numbers(g, node, "translation", 3, out->t, what) &&
		get_numbers(g, node, "rotation", 4, out->r, what) &&
		get_numbers(g, node, "scale", 3, out->s, what);
}

static bool node_local(struct gltf *g, size_t index, double m[16]) {
	struct gltf_trs authored;
	const struct gltf_trs *trs = g->options && g->options->pose ? &g->options->pose[index] : NULL;
	if (!trs) {
		if (!node_authored(g, index, &authored)) return false;
		trs = &authored;
	}
	if (trs->has_matrix) memcpy(m, trs->m, 16 * sizeof(*m));
	else mat_trs(m, trs->t, trs->r, trs->s);
	return true;
}

/* Parents from the children lists: a node may have one parent and the graph
 * must be acyclic (checked while resolving world matrices). */
static bool build_hierarchy(struct gltf *g) {
	if (!g->node_count) return true;
	g->parent = malloc(g->node_count * sizeof(*g->parent));
	g->world = malloc(g->node_count * sizeof(*g->world));
	g->world_state = calloc(g->node_count, 1);
	g->chain = malloc(g->node_count * sizeof(*g->chain));
	if (!g->parent || !g->world || !g->world_state || !g->chain)
		return fail(g, "out of memory");
	for (size_t i = 0; i < g->node_count; ++i) g->parent[i] = -1;
	for (size_t i = 0; i < g->node_count; ++i) {
		const struct json *children = json_get(json_at(g->nodes, i), "children");
		if (children && children->type != JSON_ARRAY)
			return fail(g, "node %zu: children must be an array", i);
		for (size_t k = 0; k < json_length(children); ++k) {
			size_t child;
			if (!json_uint(json_at(children, k), g->node_count - 1, &child))
				return fail(g, "node %zu: invalid child", i);
			if (child == i || g->parent[child] >= 0)
				return fail(g, "node %zu has more than one parent", child);
			g->parent[child] = (ptrdiff_t)i;
		}
	}
	return true;
}

static bool node_world(struct gltf *g, size_t index, const double **out) {
	if (g->world_state[index] != NODE_DONE) {
		/* Walk up to a resolved ancestor or a root, then resolve downwards. */
		size_t chain_length = 0, *chain = g->chain;
		ptrdiff_t n = (ptrdiff_t)index;
		while (n >= 0 && g->world_state[n] != NODE_DONE) {
			if (g->world_state[n] == NODE_VISITING)
				return fail(g, "node %zd is its own ancestor", n);
			g->world_state[n] = NODE_VISITING;
			chain[chain_length++] = (size_t)n;
			n = g->parent[n];
		}
		while (chain_length) {
			size_t c = chain[--chain_length];
			double local[16];
			if (!node_local(g, c, local)) return false;
			if (g->parent[c] >= 0) mat_mul(g->world[c], g->world[g->parent[c]], local);
			else if (g->options && g->options->root) mat_mul(g->world[c], g->options->root, local);
			else memcpy(g->world[c], local, sizeof(local));
			g->world_state[c] = NODE_DONE;
		}
	}
	*out = g->world[index];
	return true;
}

static bool node_hidden(const struct json *node) {
	if (truthy(json_get(json_get(node, "extras"), "shady_hidden"))) return true;
	const struct json *visible = json_get(json_get(json_get(node, "extensions"),
		"KHR_node_visibility"), "visible");
	return visible && visible->type == JSON_BOOL && !visible->u.boolean;
}

static bool node_is_collision(const struct json *node) {
	const char *name = json_str(json_get(node, "name"));
	return name && !strncmp(name, COLLISION_PREFIX, strlen(COLLISION_PREFIX));
}

/* ------------------------------------------------------------------------ */
/* Primitives                                                               */

struct skin {
	double (*joints)[16]; /* world(joint) * inverseBind */
	size_t joint_count;
};

struct emit {
	const double *world;
	const struct skin *skin; /* NULL: rigid */
	bool visible;
	ptrdiff_t box;          /* collision box index, or -1 */
};

static void grow_box(struct gltf *g, ptrdiff_t box, const float p[3]) {
	struct shady_environment_box *b = &((struct shady_environment_box *)g->boxes.data)[box];
	for (int i = 0; i < 3; ++i) {
		if (p[i] < b->min[i]) b->min[i] = p[i];
		if (p[i] > b->max[i]) b->max[i] = p[i];
	}
}

static void flat_normal(const float a[3], const float b[3], const float c[3], float n[3]) {
	float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	n[0] = u[1] * v[2] - u[2] * v[1];
	n[1] = u[2] * v[0] - u[0] * v[2];
	n[2] = u[0] * v[1] - u[1] * v[0];
	float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (length > 1e-12f) {
		n[0] /= length;
		n[1] /= length;
		n[2] /= length;
	}
}

/* Triangle list (indices into the primitive's vertices) for one mode. */
static bool triangulate(struct gltf *g, size_t mode, const uint32_t *idx, size_t n,
		uint32_t **out, size_t *out_count) {
	size_t tris = 0;
	if (mode == 4) tris = n / 3;
	else if (n >= 3) tris = n - 2;
	*out = NULL;
	*out_count = 0;
	if (!tris) return true;
	uint32_t *t = malloc(tris * 3 * sizeof(*t));
	if (!t) return fail(g, "out of memory");
#define AT(i) (idx ? idx[i] : (uint32_t)(i))
	for (size_t i = 0; i < tris; ++i) {
		uint32_t *o = t + i * 3;
		if (mode == 4) {
			o[0] = AT(i * 3);
			o[1] = AT(i * 3 + 1);
			o[2] = AT(i * 3 + 2);
		} else if (mode == 5) {
			o[0] = AT(i);
			o[1] = AT(i + 1 + i % 2);
			o[2] = AT(i + 2 - i % 2);
		} else {
			o[0] = AT(i + 1);
			o[1] = AT(i + 2);
			o[2] = AT(0);
		}
	}
#undef AT
	*out = t;
	*out_count = tris * 3;
	return true;
}

/* Default morph weights apply to POSITION and NORMAL. */
static bool apply_morph_targets(struct gltf *g, const struct json *prim, const struct json *mesh,
		const struct json *node, size_t count, float *pos, float *nrm, const char *what) {
	const struct json *targets = json_get(prim, "targets");
	const struct json *weights = json_get(node, "weights");
	if (!weights) weights = json_get(mesh, "weights");
	for (size_t t = 0; t < json_length(targets); ++t) {
		const struct json *w = json_at(weights, t);
		double weight = w && w->type == JSON_NUMBER ? w->u.number : 0.0;
		if (weight == 0.0 || !isfinite(weight)) continue;
		const struct json *target = json_at(targets, t);
		float *delta;
		if (!read_attribute(g, target, "POSITION", 3, count, &delta, what)) return false;
		for (size_t i = 0; delta && i < count * 3; ++i) pos[i] += (float)(weight * delta[i]);
		free(delta);
		if (!nrm) continue;
		if (!read_attribute(g, target, "NORMAL", 3, count, &delta, what)) return false;
		for (size_t i = 0; delta && i < count * 3; ++i) nrm[i] += (float)(weight * delta[i]);
		free(delta);
	}
	return true;
}

struct skin_weights {
	uint32_t *joints[MAX_JOINT_SETS];
	float *weights[MAX_JOINT_SETS];
	int sets;
};

static void skin_weights_free(struct skin_weights *sw) {
	for (int s = 0; s < MAX_JOINT_SETS; ++s) {
		free(sw->joints[s]);
		free(sw->weights[s]);
	}
}

static bool read_skin_weights(struct gltf *g, const struct json *attributes, size_t count,
		const struct skin *skin, struct skin_weights *sw, const char *what) {
	for (int s = 0; s < MAX_JOINT_SETS; ++s) {
		char jname[16], wname[16];
		snprintf(jname, sizeof(jname), "JOINTS_%d", s);
		snprintf(wname, sizeof(wname), "WEIGHTS_%d", s);
		const struct json *jv = json_get(attributes, jname);
		if (!jv || !json_get(attributes, wname)) break;
		size_t index, n;
		if (!json_uint(jv, SIZE_MAX, &index)) return fail(g, "%s: invalid %s", what, jname);
		if (!read_accessor(g, index, 4, NULL, &sw->joints[s], &n, jname)) return false;
		if (n != count) return fail(g, "%s: %s has the wrong count", what, jname);
		if (!read_attribute(g, attributes, wname, 4, count, &sw->weights[s], what))
			return false;
		for (size_t i = 0; i < count * 4; ++i)
			if (sw->joints[s][i] >= skin->joint_count && sw->weights[s][i] != 0.0f)
				return fail(g, "%s: %s references joint %u of %zu", what, jname,
					sw->joints[s][i], skin->joint_count);
		sw->sets = s + 1;
	}
	return true;
}

static void skin_matrix(const struct skin *skin, const struct skin_weights *sw, size_t v,
		const double *fallback, double out[16]) {
	double total = 0.0;
	memset(out, 0, 16 * sizeof(*out));
	for (int s = 0; s < sw->sets; ++s)
		for (int k = 0; k < 4; ++k) {
			double w = sw->weights[s][v * 4 + k];
			if (w == 0.0) continue;
			const double *j = skin->joints[sw->joints[s][v * 4 + k]];
			for (int i = 0; i < 16; ++i) out[i] += w * j[i];
			total += w;
		}
	if (!(total > 1e-8)) {
		memcpy(out, fallback, 16 * sizeof(*out));
		return;
	}
	for (int i = 0; i < 16; ++i) out[i] /= total;
}

static bool emit_primitive(struct gltf *g, const struct json *prim, const struct json *mesh,
		const struct json *node, const struct emit *e, const char *what) {
	size_t mode;
	if (!get_uint(g, prim, "mode", 6, 4, &mode, what)) return false;
	if (mode < 4) {
		g->skipped_primitives++;
		return true;
	}
	const struct json *attributes = json_get(prim, "attributes");
	const struct json *position = json_get(attributes, "POSITION");
	if (!position) {
		g->skipped_primitives++;
		return true;
	}
	size_t position_index;
	if (!json_uint(position, SIZE_MAX, &position_index))
		return fail(g, "%s: invalid POSITION", what);
	const struct json *ext = json_get(prim, "extensions");
	if (json_get(ext, "KHR_draco_mesh_compression") &&
			!json_get(json_at(g->accessors, position_index), "bufferView"))
		return fail(g, "%s is Draco-compressed, which glb-loader does not decode", what);

	bool ok = false;
	float *pos = NULL, *nrm = NULL, *uv = NULL, *wpos = NULL, *wnrm = NULL;
	uint32_t *idx = NULL, *tris = NULL, *remap = NULL;
	struct skin_weights sw = {0};
	size_t count, index_count = 0, tri_count;

	if (!read_accessor(g, position_index, 3, &pos, NULL, &count, "POSITION") ||
			!read_attribute(g, attributes, "NORMAL", 3, count, &nrm, what) ||
			!read_attribute(g, attributes, "TEXCOORD_0", 2, count, &uv, what) ||
			!apply_morph_targets(g, prim, mesh, node, count, pos, nrm, what))
		goto done;

	const struct json *indices = json_get(prim, "indices");
	if (indices) {
		size_t index;
		if (!json_uint(indices, SIZE_MAX, &index)) {
			fail(g, "%s: invalid indices", what);
			goto done;
		}
		if (!read_accessor(g, index, 1, NULL, &idx, &index_count, "indices")) goto done;
		for (size_t i = 0; i < index_count; ++i)
			if (idx[i] >= count) {
				fail(g, "%s: index %u is out of range (%zu vertices)", what, idx[i], count);
				goto done;
			}
	} else {
		index_count = count;
	}
	if (!triangulate(g, mode, idx, index_count, &tris, &tri_count)) goto done;
	if (!tri_count) {
		ok = true;
		goto done;
	}

	bool skinned = false;
	if (e->skin) {
		if (!read_skin_weights(g, attributes, count, e->skin, &sw, what)) goto done;
		skinned = sw.sets > 0;
	}

	/* Transform only the vertices the triangles use: primitives often share
	 * one large vertex buffer. */
	remap = malloc(count * sizeof(*remap));
	wpos = malloc(count * 3 * sizeof(*wpos));
	if (nrm) wnrm = malloc(count * 3 * sizeof(*wnrm));
	if (!remap || !wpos || (nrm && !wnrm)) {
		fail(g, "out of memory");
		goto done;
	}
	memset(remap, 0xFF, count * sizeof(*remap));
	double normal_matrix[9];
	mat_normal(normal_matrix, e->world);
	size_t used = 0;
	for (size_t t = 0; t < tri_count; ++t) {
		uint32_t v = tris[t];
		if (remap[v] != UINT32_MAX) continue;
		remap[v] = (uint32_t)used++;
		if (skinned) {
			double m[16], nm[9];
			skin_matrix(e->skin, &sw, v, e->world, m);
			transform_point(m, pos + v * 3, wpos + v * 3);
			if (nrm) {
				mat_normal(nm, m);
				transform_normal(nm, nrm + v * 3, wnrm + v * 3);
			}
		} else {
			transform_point(e->world, pos + v * 3, wpos + v * 3);
			if (nrm) transform_normal(normal_matrix, nrm + v * 3, wnrm + v * 3);
		}
	}
	if (!skinned && mat_det3(e->world) < 0.0) {
		for (size_t t = 0; t < tri_count; t += 3) {
			uint32_t tmp = tris[t + 1];
			tris[t + 1] = tris[t + 2];
			tris[t + 2] = tmp;
		}
	}

	if (e->visible) {
		size_t new_vertices = nrm ? used : tri_count;
		if (new_vertices > MAX_DRAW_VERTICES - g->vertices.count ||
				tri_count > MAX_DRAW_VERTICES - g->indices.count) {
			fail(g, "the scene has more than %zu vertices, more than Shady draws",
				MAX_DRAW_VERTICES);
			goto done;
		}
		if (!array_reserve(&g->vertices, new_vertices, sizeof(struct shady_environment_vertex)) ||
				!array_reserve(&g->indices, tri_count, sizeof(uint32_t))) {
			fail(g, "out of memory");
			goto done;
		}
		struct shady_environment_vertex *out_v =
			(struct shady_environment_vertex *)g->vertices.data;
		uint32_t *out_i = g->indices.data;
		uint32_t base = (uint32_t)g->vertices.count;
		if (nrm) {
			/* Shared vertices, in first-use order. */
			for (size_t v = 0; v < count; ++v) {
				if (remap[v] == UINT32_MAX) continue;
				struct shady_environment_vertex *o = &out_v[base + remap[v]];
				memcpy(o->position, wpos + v * 3, sizeof(o->position));
				memcpy(o->normal, wnrm + v * 3, sizeof(o->normal));
				/* glTF puts the UV origin top-left; the scene, like OBJ, bottom-left. */
				o->uv[0] = uv ? uv[v * 2] : 0.0f;
				o->uv[1] = uv ? 1.0f - uv[v * 2 + 1] : 0.0f;
			}
			for (size_t t = 0; t < tri_count; ++t) out_i[g->indices.count + t] = base + remap[tris[t]];
			g->vertices.count += used;
		} else {
			/* Flat shading needs a vertex per triangle corner. */
			for (size_t t = 0; t < tri_count; t += 3) {
				float n[3];
				flat_normal(wpos + tris[t] * 3, wpos + tris[t + 1] * 3, wpos + tris[t + 2] * 3, n);
				for (int k = 0; k < 3; ++k) {
					uint32_t v = tris[t + k];
					struct shady_environment_vertex *o = &out_v[base + t + k];
					memcpy(o->position, wpos + v * 3, sizeof(o->position));
					memcpy(o->normal, n, sizeof(o->normal));
					o->uv[0] = uv ? uv[v * 2] : 0.0f;
					o->uv[1] = uv ? 1.0f - uv[v * 2 + 1] : 0.0f;
					out_i[g->indices.count + t + k] = base + (uint32_t)(t + k);
				}
			}
			g->vertices.count += tri_count;
		}
		g->indices.count += tri_count;
	}

	if (e->box >= 0) {
		if (!array_reserve(&g->triangles, tri_count / 3, sizeof(struct shady_environment_triangle))) {
			fail(g, "out of memory");
			goto done;
		}
		for (size_t t = 0; t < tri_count; t += 3) {
			struct shady_environment_triangle tri;
			for (int k = 0; k < 3; ++k) {
				memcpy(tri.v[k], wpos + tris[t + k] * 3, sizeof(tri.v[k]));
				grow_box(g, e->box, tri.v[k]);
			}
			((struct shady_environment_triangle *)g->triangles.data)[g->triangles.count++] = tri;
		}
	}
	ok = true;

done:
	free(pos);
	free(nrm);
	free(uv);
	free(wpos);
	free(wnrm);
	free(idx);
	free(tris);
	free(remap);
	skin_weights_free(&sw);
	return ok;
}

static bool load_skin(struct gltf *g, size_t index, struct skin *skin) {
	const struct json *s = json_at(g->skins, index);
	const struct json *joints = json_get(s, "joints");
	char what[32];
	snprintf(what, sizeof(what), "skin %zu", index);
	if (!joints || joints->type != JSON_ARRAY || !joints->count)
		return fail(g, "%s: missing joints", what);
	skin->joint_count = joints->count;
	skin->joints = malloc(joints->count * sizeof(*skin->joints));
	if (!skin->joints) return fail(g, "out of memory");

	float *ibm = NULL;
	const struct json *ibm_json = json_get(s, "inverseBindMatrices");
	if (ibm_json) {
		size_t accessor, n;
		if (!json_uint(ibm_json, SIZE_MAX, &accessor))
			return fail(g, "%s: invalid inverseBindMatrices", what);
		if (!read_accessor(g, accessor, 16, &ibm, NULL, &n, "inverseBindMatrices"))
			return false;
		if (n < joints->count) {
			free(ibm);
			return fail(g, "%s: fewer inverse bind matrices than joints", what);
		}
	}
	for (size_t j = 0; j < joints->count; ++j) {
		size_t node;
		const double *world;
		if (!json_uint(json_at(joints, j), g->node_count ? g->node_count - 1 : 0, &node) ||
				node >= g->node_count) {
			free(ibm);
			return fail(g, "%s: invalid joint %zu", what, j);
		}
		if (!node_world(g, node, &world)) {
			free(ibm);
			return false;
		}
		double inverse_bind[16];
		if (ibm) for (int i = 0; i < 16; ++i) inverse_bind[i] = ibm[j * 16 + i];
		else mat_identity(inverse_bind);
		mat_mul(skin->joints[j], world, inverse_bind);
	}
	free(ibm);
	return true;
}

/* EXT_mesh_gpu_instancing: one transform per instance, or none. */
static bool load_instances(struct gltf *g, const struct json *node, double (**out)[16],
		size_t *out_count, const char *what) {
	*out = NULL;
	*out_count = 0;
	const struct json *attributes = json_get(json_get(json_get(node, "extensions"),
		"EXT_mesh_gpu_instancing"), "attributes");
	if (!attributes) return true;

	static const struct { const char *name; unsigned components; } kinds[] = {
		{ "TRANSLATION", 3 }, { "ROTATION", 4 }, { "SCALE", 3 },
	};
	float *data[3] = { NULL, NULL, NULL };
	size_t count = 0;
	bool ok = false;
	for (int k = 0; k < 3; ++k) {
		const struct json *v = json_get(attributes, kinds[k].name);
		if (!v) continue;
		size_t index, n;
		if (!json_uint(v, SIZE_MAX, &index)) {
			fail(g, "%s: invalid instance %s", what, kinds[k].name);
			goto done;
		}
		if (!read_accessor(g, index, kinds[k].components, &data[k], NULL, &n, kinds[k].name))
			goto done;
		if (count && n != count) {
			fail(g, "%s: instance attributes have different counts", what);
			goto done;
		}
		count = n;
	}
	if (!count) {
		ok = true;
		goto done;
	}
	*out = malloc(count * sizeof(**out));
	if (!*out) {
		fail(g, "out of memory");
		goto done;
	}
	for (size_t i = 0; i < count; ++i) {
		double t[3] = { 0, 0, 0 }, r[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
		for (int c = 0; c < 3 && data[0]; ++c) t[c] = data[0][i * 3 + c];
		for (int c = 0; c < 4 && data[1]; ++c) r[c] = data[1][i * 4 + c];
		for (int c = 0; c < 3 && data[2]; ++c) s[c] = data[2][i * 3 + c];
		mat_trs((*out)[i], t, r, s);
	}
	*out_count = count;
	ok = true;

done:
	for (int k = 0; k < 3; ++k) free(data[k]);
	return ok;
}

static bool emit_node(struct gltf *g, size_t index, bool visible, ptrdiff_t box) {
	const struct json *node = json_at(g->nodes, index);
	size_t mesh_index;
	if (!json_get(node, "mesh")) return true;
	char what[64];
	snprintf(what, sizeof(what), "node %zu", index);
	if (!get_index(g, node, "mesh", g->mesh_count, &mesh_index, what)) return false;
	const struct json *mesh = json_at(g->meshes, mesh_index);
	const struct json *primitives = json_get(mesh, "primitives");
	if (!primitives || primitives->type != JSON_ARRAY)
		return fail(g, "mesh %zu: missing primitives", mesh_index);

	const double *world;
	if (!node_world(g, index, &world)) return false;

	struct skin skin = {0};
	bool has_skin = json_get(node, "skin") != NULL;
	if (has_skin) {
		size_t skin_index;
		if (!get_index(g, node, "skin", g->skin_count, &skin_index, what) ||
				!load_skin(g, skin_index, &skin)) {
			free(skin.joints);
			return false;
		}
	}

	double (*instances)[16] = NULL;
	size_t instance_count = 0;
	if (!load_instances(g, node, &instances, &instance_count, what)) {
		free(skin.joints);
		return false;
	}
	if (has_skin && instance_count) {
		/* The extension does not define instancing of skinned meshes. */
		g->ignored_instancing++;
		instance_count = 0;
	}

	bool ok = true;
	for (size_t i = 0; ok && i < (instance_count ? instance_count : 1); ++i) {
		double m[16];
		if (instance_count) mat_mul(m, world, instances[i]);
		else memcpy(m, world, sizeof(m));
		struct emit e = {
			.world = m,
			.skin = has_skin ? &skin : NULL,
			.visible = visible,
			.box = box,
		};
		for (size_t p = 0; ok && p < primitives->count; ++p) {
			char label[96];
			snprintf(label, sizeof(label), "mesh %zu primitive %zu", mesh_index, p);
			ok = emit_primitive(g, &primitives->u.items[p], mesh, node, &e, label);
		}
	}
	free(instances);
	free(skin.joints);
	return ok;
}

struct visit {
	size_t node;
	bool visible;
	ptrdiff_t box;
};

static bool emit_scene(struct gltf *g) {
	const struct json *scenes = json_get(&g->root, "scenes");
	const struct json *roots = NULL;
	size_t scene_index = 0;
	if (json_length(scenes)) {
		if (!get_uint(g, &g->root, "scene", json_length(scenes) - 1, 0, &scene_index, "glTF"))
			return false;
		roots = json_get(json_at(scenes, scene_index), "nodes");
		if (roots && roots->type != JSON_ARRAY)
			return fail(g, "scene %zu: nodes must be an array", scene_index);
	}

	struct array stack = {0};
	bool ok = true;
	/* No scenes: draw every root node, as viewers do. */
	size_t root_count = roots ? roots->count : (scenes ? 0 : g->node_count);
	for (size_t r = root_count; ok && r-- > 0;) {
		size_t node;
		if (roots) {
			if (!json_uint(json_at(roots, r), g->node_count ? g->node_count - 1 : 0, &node) ||
					node >= g->node_count) {
				ok = fail(g, "scene %zu: invalid node", scene_index);
				break;
			}
			if (g->parent[node] >= 0) {
				ok = fail(g, "scene %zu: node %zu is not a root", scene_index, node);
				break;
			}
		} else {
			node = r;
			if (g->parent[node] >= 0) continue;
		}
		struct visit v = { node, true, -1 };
		if (!array_push(&stack, &v, sizeof(v))) ok = fail(g, "out of memory");
	}

	/* Depth-first; the hierarchy is a forest, so every node is visited once. */
	size_t visited = 0;
	while (ok && stack.count) {
		struct visit v = ((struct visit *)stack.data)[--stack.count];
		if (++visited > g->node_count) {
			ok = fail(g, "the node hierarchy has a cycle");
			break;
		}
		const struct json *node = json_at(g->nodes, v.node);
		if (node_hidden(node) || (g->options->hidden && g->options->hidden[v.node]))
			v.visible = false;
		if (v.box < 0 && g->options->collision && node_is_collision(node)) {
			struct shady_environment_box box = {
				{ INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY },
			};
			if (!array_push(&g->boxes, &box, sizeof(box))) {
				ok = fail(g, "out of memory");
				break;
			}
			v.box = (ptrdiff_t)g->boxes.count - 1;
		}
		/* A hidden node still collides, and may have collision_ children. */
		if (v.visible || v.box >= 0) ok = emit_node(g, v.node, v.visible, v.box);
		const struct json *children = json_get(node, "children");
		for (size_t c = json_length(children); ok && c-- > 0;) {
			struct visit child = { (size_t)json_at(children, c)->u.number, v.visible, v.box };
			if (!array_push(&stack, &child, sizeof(child))) ok = fail(g, "out of memory");
		}
	}
	free(stack.data);
	if (!ok) return false;

	/* Collision nodes without triangles add no box. */
	struct shady_environment_box *boxes = g->boxes.data;
	size_t kept = 0;
	for (size_t i = 0; i < g->boxes.count; ++i)
		if (boxes[i].min[0] <= boxes[i].max[0]) boxes[kept++] = boxes[i];
	g->boxes.count = kept;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Document                                                                 */

static bool extension_supported(const char *name) {
	/* Implemented here. */
	static const char *const geometry[] = {
		"KHR_mesh_quantization", "EXT_mesh_gpu_instancing", "KHR_node_visibility",
	};
	/* Change only appearance or animation, which Shady does not use. */
	static const char *const appearance_prefixes[] = {
		"KHR_materials_", "KHR_texture_", "EXT_texture_", "KHR_lights_", "EXT_lights_",
		"KHR_xmp", "KHR_animation_pointer",
	};
	for (size_t i = 0; i < sizeof(geometry) / sizeof(*geometry); ++i)
		if (!strcmp(name, geometry[i])) return true;
	for (size_t i = 0; i < sizeof(appearance_prefixes) / sizeof(*appearance_prefixes); ++i)
		if (!strncmp(name, appearance_prefixes[i], strlen(appearance_prefixes[i]))) return true;
	return false;
}

static bool check_document(struct gltf *g) {
	if (g->root.type != JSON_OBJECT) return fail(g, "the JSON chunk is not an object");
	const struct json *asset = json_get(&g->root, "asset");
	const char *version = json_str(json_get(asset, "version"));
	if (!version) return fail(g, "asset.version is missing");
	if (strncmp(version, "2.", 2)) return fail(g, "glTF version %s is not supported", version);
	const char *min_version = json_str(json_get(asset, "minVersion"));
	if (min_version && strcmp(min_version, "2.0"))
		return fail(g, "glTF minVersion %s is not supported", min_version);

	const struct json *required = json_get(&g->root, "extensionsRequired");
	for (size_t i = 0; i < json_length(required); ++i) {
		const char *name = json_str(json_at(required, i));
		if (!name) return fail(g, "extensionsRequired must hold strings");
		if (extension_supported(name)) continue;
		if (!strcmp(name, "KHR_draco_mesh_compression") ||
				!strcmp(name, "EXT_meshopt_compression") ||
				!strcmp(name, "KHR_meshopt_compression"))
			return fail(g, "the file requires %s; re-export it without mesh compression", name);
		return fail(g, "the file requires the unsupported extension %s", name);
	}

	g->accessors = json_get(&g->root, "accessors");
	g->meshes = json_get(&g->root, "meshes");
	g->nodes = json_get(&g->root, "nodes");
	g->skins = json_get(&g->root, "skins");
	g->accessor_count = json_length(g->accessors);
	g->mesh_count = json_length(g->meshes);
	g->node_count = json_length(g->nodes);
	g->skin_count = json_length(g->skins);
	return true;
}

/* ------------------------------------------------------------------------ */
/* Animations                                                               */

enum { PATH_TRANSLATION, PATH_ROTATION, PATH_SCALE, PATH_OTHER };
enum { INTERP_LINEAR, INTERP_STEP, INTERP_CUBIC };

struct sampler {
	float *input;   /* key times, ascending */
	float *output;  /* components per key; three times that for CUBICSPLINE */
	size_t count;
	unsigned components;
	int interpolation;
};

struct channel {
	size_t node;
	int path;
	size_t sampler;
};

struct animation {
	bool loaded, valid;
	struct sampler *samplers;
	size_t sampler_count;
	struct channel *channels;
	size_t channel_count;
	double duration;
};

static void animation_free(struct animation *a) {
	for (size_t i = 0; i < a->sampler_count; ++i) {
		free(a->samplers[i].input);
		free(a->samplers[i].output);
	}
	free(a->samplers);
	free(a->channels);
	*a = (struct animation){0};
}

/* Read samplers and channels once, on first use. Channels that target
 * morph weights, pointer paths or missing nodes are dropped. */
static bool animation_load(struct gltf *g, size_t index) {
	struct animation *a = &g->animations[index];
	if (a->loaded) return a->valid;
	a->loaded = true;
	const struct json *json = json_at(json_get(&g->root, "animations"), index);
	const struct json *samplers = json_get(json, "samplers");
	const struct json *channels = json_get(json, "channels");
	char what[48];
	snprintf(what, sizeof(what), "animation %zu", index);
	a->sampler_count = json_length(samplers);
	a->samplers = calloc(a->sampler_count ? a->sampler_count : 1, sizeof(*a->samplers));
	a->channels = calloc(json_length(channels) ? json_length(channels) : 1, sizeof(*a->channels));
	if (!a->samplers || !a->channels) return fail(g, "out of memory");

	for (size_t c = 0; c < json_length(channels); ++c) {
		const struct json *ch = json_at(channels, c);
		const struct json *target = json_get(ch, "target");
		const char *path = json_str(json_get(target, "path"));
		size_t node, sampler;
		if (!json_get(target, "node") || !path) continue;
		if (!get_index(g, target, "node", g->node_count, &node, what) ||
				!get_index(g, ch, "sampler", a->sampler_count, &sampler, what))
			goto fail;
		int kind = !strcmp(path, "translation") ? PATH_TRANSLATION :
			!strcmp(path, "rotation") ? PATH_ROTATION :
			!strcmp(path, "scale") ? PATH_SCALE : PATH_OTHER;
		if (kind == PATH_OTHER) continue;

		struct sampler *s = &a->samplers[sampler];
		unsigned components = kind == PATH_ROTATION ? 4 : 3;
		if (!s->input) {
			const struct json *sj = json_at(samplers, sampler);
			const char *interp = json_str(json_get(sj, "interpolation"));
			s->interpolation = !interp || !strcmp(interp, "LINEAR") ? INTERP_LINEAR :
				!strcmp(interp, "STEP") ? INTERP_STEP :
				!strcmp(interp, "CUBICSPLINE") ? INTERP_CUBIC : -1;
			if (s->interpolation < 0) {
				fail(g, "%s: unknown interpolation %s", what, interp);
				goto fail;
			}
			size_t in_index, out_index, in_count, out_count;
			if (!get_index(g, sj, "input", g->accessor_count, &in_index, what) ||
					!get_index(g, sj, "output", g->accessor_count, &out_index, what) ||
					!read_accessor(g, in_index, 1, &s->input, NULL, &in_count, "animation input") ||
					!read_accessor(g, out_index, components, &s->output, NULL, &out_count,
						"animation output"))
				goto fail;
			size_t per_key = s->interpolation == INTERP_CUBIC ? 3 : 1;
			if (out_count != in_count * per_key) {
				fail(g, "%s: sampler %zu has %zu outputs for %zu keys", what, sampler,
					out_count, in_count);
				goto fail;
			}
			for (size_t k = 0; k < in_count; ++k)
				if (!isfinite(s->input[k]) || (k && s->input[k] < s->input[k - 1])) {
					fail(g, "%s: sampler %zu key times are not ascending", what, sampler);
					goto fail;
				}
			s->count = in_count;
			s->components = components;
			if (s->input[in_count - 1] > a->duration) a->duration = s->input[in_count - 1];
		} else if (s->components != components) {
			fail(g, "%s: sampler %zu animates different kinds of values", what, sampler);
			goto fail;
		}
		a->channels[a->channel_count++] = (struct channel){ node, kind, sampler };
	}
	a->valid = true;
	return true;

fail:
	animation_free(a);
	a->loaded = true;
	return false;
}

static void quat_normalize(double q[4]) {
	double l = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 0.0) for (int i = 0; i < 4; ++i) q[i] /= l;
	else q[0] = q[1] = q[2] = 0.0, q[3] = 1.0;
}

static void quat_slerp(double out[4], const double a[4], const double b_in[4], double t) {
	double b[4] = { b_in[0], b_in[1], b_in[2], b_in[3] };
	double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (d < 0.0) {
		d = -d;
		for (int i = 0; i < 4; ++i) b[i] = -b[i];
	}
	double wa = 1.0 - t, wb = t;
	if (d < 0.9995) {
		double theta = acos(d), s = sin(theta);
		wa = sin((1.0 - t) * theta) / s;
		wb = sin(t * theta) / s;
	}
	for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * b[i];
	quat_normalize(out);
}

static void sampler_value(const struct sampler *s, double time, double out[4]) {
	unsigned n = s->components;
	size_t stride = s->interpolation == INTERP_CUBIC ? 3 * n : n;
	size_t value_at = s->interpolation == INTERP_CUBIC ? n : 0; /* skip the in-tangent */
	if (s->count == 1 || time <= s->input[0]) {
		for (unsigned c = 0; c < n; ++c) out[c] = s->output[value_at + c];
		return;
	}
	if (time >= s->input[s->count - 1]) {
		for (unsigned c = 0; c < n; ++c) out[c] = s->output[(s->count - 1) * stride + value_at + c];
		return;
	}
	size_t lo = 0, hi = s->count - 1; /* input[lo] <= time < input[hi] */
	while (hi - lo > 1) {
		size_t mid = lo + (hi - lo) / 2;
		if (s->input[mid] <= time) lo = mid;
		else hi = mid;
	}
	double t0 = s->input[lo], dt = s->input[hi] - t0;
	double u = dt > 0.0 ? (time - t0) / dt : 0.0;
	const float *k0 = s->output + lo * stride, *k1 = s->output + hi * stride;

	if (s->interpolation == INTERP_STEP) {
		for (unsigned c = 0; c < n; ++c) out[c] = k0[c];
	} else if (s->interpolation == INTERP_CUBIC) {
		/* Hermite spline from (in-tangent, value, out-tangent) triplets. */
		double u2 = u * u, u3 = u2 * u;
		double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u;
		double h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
		for (unsigned c = 0; c < n; ++c)
			out[c] = h00 * k0[n + c] + h10 * dt * k0[2 * n + c] +
				h01 * k1[n + c] + h11 * dt * k1[c];
		if (n == 4) quat_normalize(out);
	} else if (n == 4) {
		double a[4] = { k0[0], k0[1], k0[2], k0[3] }, b[4] = { k1[0], k1[1], k1[2], k1[3] };
		quat_slerp(out, a, b, u);
	} else {
		for (unsigned c = 0; c < n; ++c) out[c] = k0[c] + (k1[c] - k0[c]) * u;
	}
}

ptrdiff_t gltf_find_animation(struct gltf *g, const char *name) {
	const struct json *animations = json_get(&g->root, "animations");
	for (size_t i = 0; i < g->animation_count; ++i) {
		const char *n = json_str(json_get(json_at(animations, i), "name"));
		if (n && !strcmp(n, name)) return (ptrdiff_t)i;
	}
	return -1;
}

double gltf_animation_duration(struct gltf *g, size_t animation) {
	if (animation >= g->animation_count || !animation_load(g, animation)) return 0.0;
	return g->animations[animation].duration;
}

bool gltf_animation_sample(struct gltf *g, size_t animation, double time,
		struct gltf_trs *pose) {
	g->error[0] = '\0';
	if (animation >= g->animation_count) return fail(g, "no animation %zu", animation);
	if (!animation_load(g, animation)) return false;
	const struct animation *a = &g->animations[animation];
	for (size_t i = 0; i < a->channel_count; ++i) {
		const struct channel *c = &a->channels[i];
		struct gltf_trs *trs = &pose[c->node];
		/* The spec forbids animating a node that has a matrix. */
		if (trs->has_matrix) continue;
		double v[4];
		sampler_value(&a->samplers[c->sampler], time, v);
		double *dst = c->path == PATH_TRANSLATION ? trs->t :
			c->path == PATH_ROTATION ? trs->r : trs->s;
		memcpy(dst, v, (c->path == PATH_ROTATION ? 4 : 3) * sizeof(*v));
	}
	return true;
}

void gltf_pose_blend(struct gltf_trs *a, const struct gltf_trs *b, size_t count, double w) {
	for (size_t i = 0; i < count; ++i) {
		if (a[i].has_matrix || b[i].has_matrix) continue;
		for (int c = 0; c < 3; ++c) {
			a[i].t[c] += (b[i].t[c] - a[i].t[c]) * w;
			a[i].s[c] += (b[i].s[c] - a[i].s[c]) * w;
		}
		quat_slerp(a[i].r, a[i].r, b[i].r, w);
	}
}

/* ------------------------------------------------------------------------ */
/* Public interface                                                         */

static void gltf_free_inputs(struct gltf *g) {
	for (size_t i = 0; i < g->buffer_count; ++i) free(g->buffers[i].owned);
	free(g->buffers);
	free(g->views);
	free(g->parent);
	free(g->chain);
	free(g->world);
	free(g->world_state);
	for (size_t i = 0; i < g->animation_count; ++i) animation_free(&g->animations[i]);
	free(g->animations);
	json_free(&g->root);
	free(g->file);
	free(g->path);
}

static void gltf_free_outputs(struct gltf *g) {
	free(g->vertices.data);
	free(g->indices.data);
	free(g->boxes.data);
	free(g->triangles.data);
	g->vertices = g->indices = g->boxes = g->triangles = (struct array){0};
}

static bool glb_parse(struct gltf *g) {
	if (!read_file(g->path, 0, &g->file, &g->file_size, g->error, sizeof(g->error)))
		return false;
	const char *json;
	size_t json_size;
	if (!parse_glb(g, &json, &json_size)) return false;
	const char *json_error = NULL;
	if (!json_parse(json, json_size, &g->root, &json_error))
		return fail(g, "invalid JSON chunk: %s", json_error);
	if (!check_document(g) || !load_buffers(g) || !build_hierarchy(g)) return false;
	g->animation_count = json_length(json_get(&g->root, "animations"));
	if (g->animation_count) {
		g->animations = calloc(g->animation_count, sizeof(*g->animations));
		if (!g->animations) return fail(g, "out of memory");
	}
	return true;
}

struct gltf *gltf_open(const char *path, char *error, size_t error_size) {
	struct gltf *g = calloc(1, sizeof(*g));
	if (!g || !(g->path = strdup(path))) {
		snprintf(error, error_size, "out of memory");
		free(g);
		return NULL;
	}
	if (!glb_parse(g)) {
		snprintf(error, error_size, "%s", g->error);
		gltf_close(g);
		return NULL;
	}
	return g;
}

void gltf_close(struct gltf *g) {
	if (!g) return;
	gltf_free_inputs(g);
	gltf_free_outputs(g);
	free(g);
}

const char *gltf_error(const struct gltf *g) {
	return g->error;
}

bool gltf_build(struct gltf *g, const struct gltf_build_options *options,
		struct gltf_scene *out) {
	static const struct gltf_build_options defaults = { .collision = true };
	g->options = options ? options : &defaults;
	g->error[0] = '\0';
	g->vertices.count = g->indices.count = g->boxes.count = g->triangles.count = 0;
	g->skipped_primitives = g->ignored_instancing = 0;
	if (g->node_count) memset(g->world_state, NODE_PENDING, g->node_count);
	bool ok = emit_scene(g);
	if (ok && !g->indices.count && !g->triangles.count) ok = fail(g, "the scene has no triangles");
	g->options = NULL;
	if (!ok) return false;
	*out = (struct gltf_scene){
		.vertices = g->vertices.data, .vertex_count = g->vertices.count,
		.indices = g->indices.data, .index_count = g->indices.count,
		.boxes = g->boxes.data, .box_count = g->boxes.count,
		.triangles = g->triangles.data, .triangle_count = g->triangles.count,
		.skipped_primitives = g->skipped_primitives,
		.ignored_instancing = g->ignored_instancing,
	};
	return true;
}

void gltf_take_scene(struct gltf *g, struct gltf_scene *out) {
	out->vertices = g->vertices.data;
	out->vertex_count = g->vertices.count;
	out->indices = g->indices.data;
	out->index_count = g->indices.count;
	out->boxes = g->boxes.data;
	out->box_count = g->boxes.count;
	out->triangles = g->triangles.data;
	out->triangle_count = g->triangles.count;
	g->vertices = g->indices = g->boxes = g->triangles = (struct array){0};
}

size_t gltf_node_count(const struct gltf *g) {
	return g->node_count;
}

ptrdiff_t gltf_find_node(const struct gltf *g, const char *name) {
	for (size_t i = 0; i < g->node_count; ++i) {
		const char *n = json_str(json_get(json_at(g->nodes, i), "name"));
		if (n && !strcmp(n, name)) return (ptrdiff_t)i;
	}
	return -1;
}

bool gltf_rest_pose(struct gltf *g, struct gltf_trs *pose) {
	g->error[0] = '\0';
	for (size_t i = 0; i < g->node_count; ++i)
		if (!node_authored(g, i, &pose[i])) return false;
	return true;
}
