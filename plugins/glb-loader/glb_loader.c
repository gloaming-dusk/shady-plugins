/*
 * glTF 2.0 binary (.glb) environment loader.
 *
 * Registers itself for ".glb" with the host's shady.environment API. The host
 * decides when to load (config `environment_path`), copies the scene, and owns
 * rendering and collision; this plugin only parses, with gltf.c.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <shady/environment.h>
#include <shady/plugin.h>

#include "gltf.h"

static const struct shady_plugin_api_v1 *api;
static const struct shady_environment_api_v1 *environment;
static shady_host host;
static shady_environment_loader_id loader_id;

static void log_message(enum shady_plugin_log_level level, const char *fmt, ...) {
	char message[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	api->log(level, message);
}

static bool glb_load(shady_host h, const char *path,
		struct shady_environment_scene *scene, void *user_data) {
	(void)h;
	(void)user_data;
	char error[512];
	struct gltf *g = gltf_open(path, error, sizeof(error));
	if (!g) {
		log_message(SHADY_PLUGIN_LOG_ERROR, "glb-loader: %s: %s", path, error);
		return false;
	}
	struct gltf_scene out;
	if (!gltf_build(g, NULL, &out)) {
		log_message(SHADY_PLUGIN_LOG_ERROR, "glb-loader: %s: %s", path, gltf_error(g));
		gltf_close(g);
		return false;
	}
	if (out.skipped_primitives)
		log_message(SHADY_PLUGIN_LOG_INFO,
			"glb-loader: %s: skipped %zu point, line or position-less primitives",
			path, out.skipped_primitives);
	if (out.ignored_instancing)
		log_message(SHADY_PLUGIN_LOG_INFO,
			"glb-loader: %s: drew %zu skinned instanced nodes once", path, out.ignored_instancing);
	gltf_take_scene(g, &out);
	gltf_close(g);

	/* The host frees nothing; colours are dropped for a host too old to
	 * have room for them in the struct it passed. */
#if GLTF_HOST_HAS_COLORS
	if (SHADY_ENVIRONMENT_SCENE_HAS(scene, colors)) scene->colors = out.colors;
	else
#endif
		free((void *)out.colors);
#if GLTF_HOST_HAS_LOOKS
	if (SHADY_ENVIRONMENT_SCENE_HAS(scene, texture_key)) {
		scene->texture = out.texture;
		scene->texture_width = out.texture_width;
		scene->texture_height = out.texture_height;
	} else
#endif
		free((void *)out.texture);
	scene->vertices = out.vertices;
	scene->vertex_count = out.vertex_count;
	scene->indices = out.indices;
	scene->index_count = out.index_count;
	scene->boxes = out.boxes;
	scene->box_count = out.box_count;
	scene->triangles = out.triangles;
	scene->triangle_count = out.triangle_count;
	return true;
}

static void glb_release(shady_host h, struct shady_environment_scene *scene,
		void *user_data) {
	(void)h;
	(void)user_data;
#if GLTF_HOST_HAS_COLORS
	if (SHADY_ENVIRONMENT_SCENE_HAS(scene, colors)) {
		free((void *)scene->colors);
		scene->colors = NULL;
	}
#endif
#if GLTF_HOST_HAS_LOOKS
	if (SHADY_ENVIRONMENT_SCENE_HAS(scene, texture_key)) {
		free((void *)scene->texture);
		scene->texture = NULL;
	}
#endif
	free((void *)scene->vertices);
	free((void *)scene->indices);
	free((void *)scene->boxes);
	free((void *)scene->triangles);
	/* Only the members every host's struct has: it may be smaller than ours. */
	scene->vertices = NULL;
	scene->indices = NULL;
	scene->boxes = NULL;
	scene->triangles = NULL;
	scene->vertex_count = scene->index_count = scene->box_count = scene->triangle_count = 0;
}

static const char *const glb_extensions[] = { "glb", NULL };

static const struct shady_environment_loader glb_loader = {
	.struct_size = sizeof(glb_loader),
	.name = "glb",
	.extensions = glb_extensions,
	.load = glb_load,
	.release = glb_release,
};

static bool glb_loader_init(struct shady_server *server) {
	(void)server;
	loader_id = environment->register_loader(host, &glb_loader);
	if (!loader_id) {
		api->log(SHADY_PLUGIN_LOG_ERROR,
			"glb-loader: host rejected the .glb loader (another loader claims .glb?)");
		return false;
	}
	return true;
}

static void glb_loader_destroy(struct shady_server *server) {
	(void)server;
	if (loader_id) environment->unregister_loader(host, loader_id);
	loader_id = 0;
}

static const char *const provides[] = { "environment-loader.glb", NULL };
static const char *const requires[] = { "spatial", NULL };

static const struct shady_module glb_loader_module = {
	.name = "glb-loader",
	.provides = provides,
	.requires = requires,
	.init = glb_loader_init,
	.destroy = glb_loader_destroy,
};

const struct shady_module *shady_plugin_entry_v1(uint32_t host_abi,
		const struct shady_plugin_api_v1 *host_api, shady_host host_context) {
	if (host_abi != SHADY_PLUGIN_ABI_V1 || !host_api ||
			host_api->abi_version != SHADY_PLUGIN_ABI_V1 ||
			!SHADY_API_HAS(host_api, query_api))
		return NULL;

	const struct shady_environment_api_v1 *env = host_api->query_api(host_context,
		SHADY_ENVIRONMENT_API, SHADY_ENVIRONMENT_API_VERSION);
	if (!env || env->struct_size < sizeof(*env) ||
			!env->register_loader || !env->unregister_loader)
		return NULL;

	api = host_api;
	environment = env;
	host = host_context;
	return &glb_loader_module;
}
