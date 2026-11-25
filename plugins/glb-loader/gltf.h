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

struct gltf;

/* A node's local transform. Nodes authored with a matrix keep it (and are
 * never animated, by the spec); the others use translation/rotation/scale. */
struct gltf_trs {
	double t[3], r[4], s[3];
	bool has_matrix;
	double m[16];
};

struct gltf_build_options {
	/* Local transform per node (gltf_node_count entries); NULL: as authored. */
	const struct gltf_trs *pose;
	/* Per node: hide it and its subtree (drawn nowhere, still collides). */
	const bool *hidden;
	/* Column-major transform applied above the root nodes; NULL: identity. */
	const double *root;
	/* Export collision_* nodes as collision. */
	bool collision;
};

/* Output arrays stay owned by the model and are reused by the next build. */
struct gltf_scene {
	const struct shady_environment_vertex *vertices;
	size_t vertex_count;
	const uint32_t *indices;
	size_t index_count;
	const struct shady_environment_box *boxes;
	size_t box_count;
	const struct shady_environment_triangle *triangles;
	size_t triangle_count;
	/* Notes for the log; zero normally. */
	size_t skipped_primitives;
	size_t ignored_instancing;
};

/* NULL on failure, with the reason in error. */
struct gltf *gltf_open(const char *path, char *error, size_t error_size);
void gltf_close(struct gltf *g);
/* Why the last call on g failed. */
const char *gltf_error(const struct gltf *g);

bool gltf_build(struct gltf *g, const struct gltf_build_options *options,
	struct gltf_scene *out);
/* Hand the arrays of the last build to the caller, to free() each. */
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
