/*
 * A glTF 2.0 binary (.glb) model, flattened into a Shady environment scene.
 *
 * Shared by the glb-loader plugin (a static environment) and glb-wanderer (an
 * animated character). Plugin-independent: no Shady host calls, errors are
 * returned as text.
 */
#ifndef SHADY_PLUGINS_GLTF_H
#define SHADY_PLUGINS_GLTF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <shady/environment.h>

/* Shady 0.5.1 and older draw the environment in one colour and their scene
 * has no colors; the reader still makes them, and the plugins drop them. */
#ifdef SHADY_ENVIRONMENT_SCENE_HAS
#define GLTF_HOST_HAS_COLORS 1
#else
#define GLTF_HOST_HAS_COLORS 0
struct shady_environment_color {
	float rgba[4];
};
#endif
/* Toon shading, culling and a texture came after colours. */
#ifdef SHADY_ENVIRONMENT_SCENE_TOON
#define GLTF_HOST_HAS_LOOKS 1
#else
#define GLTF_HOST_HAS_LOOKS 0
#endif

struct gltf;

/* A node's local transform. Nodes authored with a matrix keep it (and are
 * never animated, by the spec); the others use translation/rotation/scale. */
struct gltf_trs {
	double t[3], r[4], s[3];
	bool has_matrix;
	double m[16];
};

/* An ink line: a back-facing shell pushed out `width` model units along the
 * normals (0: none), drawn unlit in `rgba` (display sRGB). Needs
 * cull_back_faces. */
struct gltf_ink {
	float rgba[4];
	double width;
};

/* How a primitive is drawn beyond its material. */
struct gltf_look {
	/* How strongly light and shade fall on it (the colours' alpha): 1 fully. */
	float shade;
	struct gltf_ink ink;
};

/* Adjust *look (it comes as shade 1, no ink) for a primitive of the node named
 * `node` in the material named `material` (either may be ""). */
typedef void (*gltf_look_fn)(const char *node, const char *material, struct gltf_look *look,
	void *data);

struct gltf_build_options {
	/* Local transform per node (gltf_node_count entries); NULL: as authored. */
	const struct gltf_trs *pose;
	/* Per node: hide it and its subtree (drawn nowhere, still collides). */
	const bool *hidden;
	/* Column-major transform applied above the root nodes; NULL: identity. */
	const double *root;
	/* Export collision_* nodes as collision. */
	bool collision;
	/* The host culls back faces: give double-sided materials their back
	 * faces as triangles of their own. */
	bool cull_back_faces;
	/* Per-primitive looks; NULL: shade 1, no ink. */
	gltf_look_fn look;
	void *look_data;
};

/* Output arrays stay owned by the model and are reused by the next build. */
struct gltf_scene {
	const struct shady_environment_vertex *vertices;
	/* vertex_count display colours: the material's base colour times COLOR_0;
	 * alpha is the look's shade (1 without a look function). */
	const struct shady_environment_color *colors;
	size_t vertex_count;
	const uint32_t *indices;
	size_t index_count;
	const struct shady_environment_box *boxes;
	size_t box_count;
	const struct shady_environment_triangle *triangles;
	size_t triangle_count;
	/* The materials' base colour textures in one atlas (RGBA8, rows top
	 * first), which the vertices' uvs point into; NULL without textures.
	 * Owned by the model and the same for every build, with one key. */
	const uint8_t *texture;
	uint32_t texture_width, texture_height;
	uint64_t texture_key;
	/* Notes for the log; zero normally. */
	size_t skipped_primitives;
	size_t ignored_instancing;
	size_t skipped_images; /* JPEG or broken: drawn untextured */
};

/* NULL on failure, with the reason in error. */
struct gltf *gltf_open(const char *path, char *error, size_t error_size);
void gltf_close(struct gltf *g);
/* Why the last call on g failed. */
const char *gltf_error(const struct gltf *g);

bool gltf_build(struct gltf *g, const struct gltf_build_options *options,
	struct gltf_scene *out);
/* Hand the arrays of the last build, and the texture atlas, to the caller,
 * to free() each; later builds have no texture. */
void gltf_take_scene(struct gltf *g, struct gltf_scene *out);

size_t gltf_node_count(const struct gltf *g);
/* -1 when no node has that name. */
ptrdiff_t gltf_find_node(const struct gltf *g, const char *name);
/* Fill pose (gltf_node_count entries) with the authored transforms. */
bool gltf_rest_pose(struct gltf *g, struct gltf_trs *pose);

/* -1 when no animation has that name. */
ptrdiff_t gltf_find_animation(struct gltf *g, const char *name);
/* Seconds; 0 for an empty or broken animation. */
double gltf_animation_duration(struct gltf *g, size_t animation);
/* Overwrite the animated translations, rotations and scales in pose with
 * their values at time (clamped to the animation). */
bool gltf_animation_sample(struct gltf *g, size_t animation, double time,
	struct gltf_trs *pose);

/* Blend b into a by weight w in [0, 1], node by node. */
void gltf_pose_blend(struct gltf_trs *a, const struct gltf_trs *b, size_t count, double w);

#endif
