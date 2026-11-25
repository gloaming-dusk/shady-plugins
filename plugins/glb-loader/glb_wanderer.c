/*
 * glb-wanderer: an animated glTF character that wanders about Shady's floor.
 *
 * It picks a spot on the floor, turns and walks there with the model's walk
 * clip, idles a while with its idle clip, and picks another, crossfading
 * between the two. Every frame (at most `fps` times a second) the posed model
 * is flattened with gltf.c and handed to the host as the procedural
 * environment scene, which the host draws behind the windows.
 *
 * Load it in config.lua; configure it with published values from init.lua
 * (config.lua cannot publish). It watches them, so they can change at any
 * time, e.g. with `shadyctl lua`:
 *
 *   shady.publish("glb-wanderer.model", "/path/to/character.glb")  -- required
 *   shady.publish("glb-wanderer.hide", "Hat,Rod")   -- nodes not to draw
 *   shady.publish("glb-wanderer.walk", "Walk")      -- clip names
 *   shady.publish("glb-wanderer.idle", "Idle")
 *   shady.publish("glb-wanderer.height", "0.6")     -- world units, idle pose
 *   shady.publish("glb-wanderer.speed", "1")        -- model units per second
 *   shady.publish("glb-wanderer.area", "-1.1,-2.8,1.1,-1") -- x0,z0,x1,z1 on the floor
 *   shady.publish("glb-wanderer.fps", "0")        -- poses a second; 0: every frame
 *
 * The scene replaces a configured environment while a model is shown.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shady/environment.h>
#include <shady/plugin.h>

#include "gltf.h"

#define PREFIX "glb-wanderer"
/* Shady's floor plane (compositor/src/world/floor.h). */
#define FLOOR_Y -0.62
#define TURN_RATE 3.0      /* radians per second */
#define BLEND_RATE 4.0     /* crossfades take a quarter of a second */
#define ARRIVED 0.04
#define POLL_INTERVAL 0.5  /* seconds between looks at the settings */
/* Without a model, frames are kept coming this long after loading, so the
 * settings init.lua publishes are seen; later changes are seen with the next
 * frame anything draws. */
#define STARTUP_WATCH 10.0
#define PI 3.14159265358979323846

static const char *const setting_keys[] = {
	"model", "hide", "walk", "idle", "height", "speed", "area", "fps",
};
#define SETTING_COUNT (sizeof(setting_keys) / sizeof(*setting_keys))

static const struct shady_plugin_api_v1 *api;
static const struct shady_environment_api_v1 *environment;
static shady_host host;
static shady_render_hook_id frame_hook;

enum mode { IDLE, WALK };

struct wanderer {
	/* The settings the model was set up with; NULL entries are unset. */
	char *settings[SETTING_COUNT];
	double since_poll, since_init;
	bool shown; /* our scene is the host's */

	struct gltf *model; /* NULL: nothing to show */
	size_t node_count;
	struct gltf_trs *rest, *pose, *blend;
	bool *hidden;
	ptrdiff_t walk_clip, idle_clip;
	double walk_duration, idle_duration;

	double scale, ground, speed;
	double area[4];
	double frame_interval;

	enum mode mode;
	double x, z, yaw;
	double target_x, target_z;
	double idle_left;
	double walk_time, idle_time, walk_weight;
	double since_frame;
	uint64_t rng;
};

static struct wanderer *w;

static void log_message(enum shady_plugin_log_level level, const char *fmt, ...) {
	char message[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	api->log(level, message);
}

static const char *published(const char *key) {
	char name[128];
	snprintf(name, sizeof(name), PREFIX ".%s", key);
	return api->published_value(host, name);
}

/* The setting as last read by poll_settings. */
static const char *setting(const char *key) {
	for (size_t i = 0; i < SETTING_COUNT; ++i)
		if (!strcmp(setting_keys[i], key)) return w->settings[i];
	return NULL;
}

static double setting_number(const char *key, double def, double min, double max) {
	const char *v = setting(key);
	if (!v) return def;
	char *end;
	double n = strtod(v, &end);
	if (end == v || *end || !isfinite(n) || n < min || n > max) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": ignoring %s = '%s'", key, v);
		return def;
	}
	return n;
}

static double random_unit(void) {
	/* xorshift64* */
	w->rng ^= w->rng >> 12;
	w->rng ^= w->rng << 25;
	w->rng ^= w->rng >> 27;
	return (double)((w->rng * 2685821657736338717ull) >> 11) / 9007199254740992.0;
}

static void pick_target(void) {
	for (int tries = 0; tries < 16; ++tries) {
		w->target_x = w->area[0] + (w->area[2] - w->area[0]) * random_unit();
		w->target_z = w->area[1] + (w->area[3] - w->area[1]) * random_unit();
		if (hypot(w->target_x - w->x, w->target_z - w->z) > 0.25 * (w->area[2] - w->area[0]))
			break;
	}
}

/* T(x, ground, z) * Ry(yaw) * S(scale), column-major. The model faces +Z. */
static void root_matrix(double m[16]) {
	double c = cos(w->yaw), s = sin(w->yaw), k = w->scale;
	double r[16] = {
		c * k, 0, -s * k, 0,
		0, k, 0, 0,
		s * k, 0, c * k, 0,
		w->x, w->ground, w->z, 1,
	};
	memcpy(m, r, sizeof(r));
}

static bool pose_now(void) {
	memcpy(w->pose, w->rest, w->node_count * sizeof(*w->pose));
	if (w->idle_clip >= 0 && !gltf_animation_sample(w->model, (size_t)w->idle_clip,
			fmod(w->idle_time, w->idle_duration > 0 ? w->idle_duration : 1), w->pose))
		return false;
	if (w->walk_clip >= 0 && w->walk_weight > 0.0) {
		memcpy(w->blend, w->rest, w->node_count * sizeof(*w->blend));
		if (!gltf_animation_sample(w->model, (size_t)w->walk_clip,
				fmod(w->walk_time, w->walk_duration > 0 ? w->walk_duration : 1), w->blend))
			return false;
		gltf_pose_blend(w->pose, w->blend, w->node_count, w->walk_weight);
	}
	return true;
}

static bool build(const double *root, struct gltf_scene *out) {
	struct gltf_build_options options = {
		.pose = w->pose, .hidden = w->hidden, .root = root, .collision = false,
	};
	return pose_now() && gltf_build(w->model, &options, out);
}

static void close_model(void) {
	gltf_close(w->model);
	free(w->rest);
	free(w->pose);
	free(w->blend);
	free(w->hidden);
	w->model = NULL;
	w->rest = w->pose = w->blend = NULL;
	w->hidden = NULL;
}

static void hide_scene(void) {
	if (w->shown) environment->clear_scene(host);
	w->shown = false;
}

static void submit(void) {
	double root[16];
	root_matrix(root);
	struct gltf_scene out;
	if (!build(root, &out)) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": %s", gltf_error(w->model));
		close_model();
		hide_scene();
		return;
	}
	struct shady_environment_scene scene = {
		.struct_size = sizeof(scene),
		.vertices = out.vertices, .vertex_count = out.vertex_count,
#if GLTF_HOST_HAS_COLORS
		.colors = out.colors,
#endif
		.indices = out.indices, .index_count = out.index_count,
	};
	if (!environment->set_scene(host, &scene)) {
		log_message(SHADY_PLUGIN_LOG_ERROR,
			PREFIX ": the host refused the scene (another plugin's scene is shown?)");
		close_model();
		return;
	}
	w->shown = true;
}

static double wrap_angle(double a) {
	while (a > PI) a -= 2 * PI;
	while (a < -PI) a += 2 * PI;
	return a;
}

static void step(double dt) {
	w->idle_time += dt;
	if (w->mode == IDLE) {
		w->walk_weight = fmax(0.0, w->walk_weight - BLEND_RATE * dt);
		w->idle_left -= dt;
		if (w->idle_left <= 0.0) {
			pick_target();
			w->mode = WALK;
		}
		return;
	}

	double dx = w->target_x - w->x, dz = w->target_z - w->z;
	double distance = hypot(dx, dz);
	if (distance < ARRIVED) {
		w->mode = IDLE;
		w->idle_left = 1.5 + 3.0 * random_unit();
		return;
	}
	double turn = wrap_angle(atan2(dx, dz) - w->yaw);
	double max_turn = TURN_RATE * dt;
	w->yaw = wrap_angle(w->yaw + fmax(-max_turn, fmin(max_turn, turn)));
	/* Walk on only while roughly facing the target, so she turns on the spot. */
	double forward = w->speed * fmax(0.0, cos(turn)) * dt;
	if (forward > distance) forward = distance;
	w->x += sin(w->yaw) * forward;
	w->z += cos(w->yaw) * forward;
	w->walk_weight = fmin(1.0, w->walk_weight + BLEND_RATE * dt);
	w->walk_time += dt;
}

static void hide_nodes(void) {
	const char *list = setting("hide");
	char *copy = list ? strdup(list) : NULL, *save = NULL;
	if (!copy) return;
	for (char *name = strtok_r(copy, ", ", &save); name; name = strtok_r(NULL, ", ", &save)) {
		ptrdiff_t node = gltf_find_node(w->model, name);
		if (node < 0) log_message(SHADY_PLUGIN_LOG_INFO, PREFIX ": no node named '%s' to hide", name);
		else w->hidden[node] = true;
	}
	free(copy);
}

static ptrdiff_t find_clip(const char *key, const char *def, double *duration) {
	const char *name = setting(key);
	if (!name) name = def;
	ptrdiff_t clip = gltf_find_animation(w->model, name);
	if (clip < 0) {
		log_message(SHADY_PLUGIN_LOG_INFO, PREFIX ": no animation named '%s'; the %s pose is still",
			name, key);
		return -1;
	}
	*duration = gltf_animation_duration(w->model, (size_t)clip);
	return clip;
}

/* Scale the idle pose to the configured height and stand it on the floor. */
static bool measure(double height) {
	double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	struct gltf_scene out;
	if (!build(identity, &out)) return false;
	double lo = INFINITY, hi = -INFINITY;
	for (size_t i = 0; i < out.vertex_count; ++i) {
		lo = fmin(lo, out.vertices[i].position[1]);
		hi = fmax(hi, out.vertices[i].position[1]);
	}
	if (!(hi > lo)) return false;
	w->scale = height / (hi - lo);
	w->ground = FLOOR_Y - lo * w->scale;
	log_message(SHADY_PLUGIN_LOG_INFO, PREFIX ": %s: %zu triangles, %.2f model units tall, scale %.3f",
		setting("model"), out.index_count / 3, hi - lo, w->scale);
	return true;
}

/* Open the configured model, or show nothing. Where she stands is kept, so
 * a settings change does not make her jump. */
static void configure(void) {
	close_model();
	const char *path = setting("model");
	if (!path) {
		hide_scene();
		return;
	}
	char error[512];
	w->model = gltf_open(path, error, sizeof(error));
	if (!w->model) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": %s: %s", path, error);
		hide_scene();
		return;
	}
	w->node_count = gltf_node_count(w->model);
	size_t n = w->node_count ? w->node_count : 1;
	w->rest = calloc(n, sizeof(*w->rest));
	w->pose = calloc(n, sizeof(*w->pose));
	w->blend = calloc(n, sizeof(*w->blend));
	w->hidden = calloc(n, sizeof(*w->hidden));
	if (!w->rest || !w->pose || !w->blend || !w->hidden || !gltf_rest_pose(w->model, w->rest)) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": %s: %s", path,
			gltf_error(w->model)[0] ? gltf_error(w->model) : "out of memory");
		close_model();
		hide_scene();
		return;
	}
	hide_nodes();
	w->walk_clip = find_clip("walk", "Walk", &w->walk_duration);
	w->idle_clip = find_clip("idle", "Idle", &w->idle_duration);

	/* In the model's units, the speed the walk clip's feet move at, so a
	 * height change keeps them planted. */
	double speed = setting_number("speed", 1.0, 0.0, 1000.0);
	/* 0: a new pose every frame the compositor draws. */
	double fps = setting_number("fps", 0.0, 0.0, 1000.0);
	w->frame_interval = fps > 0.0 ? 1.0 / fps : 0.0;
	double area[4] = { -1.1, -2.8, 1.1, -1.0 };
	const char *a = setting("area");
	if (a && (sscanf(a, "%lf,%lf,%lf,%lf", &area[0], &area[1], &area[2], &area[3]) != 4 ||
			!(area[0] < area[2] && area[1] < area[3]))) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": ignoring area = '%s'", a);
		area[0] = -1.1, area[1] = -2.8, area[2] = 1.1, area[3] = -1.0;
	}
	memcpy(w->area, area, sizeof(area));
	if (!(w->x >= area[0] && w->x <= area[2] && w->z >= area[1] && w->z <= area[3])) {
		w->x = (area[0] + area[2]) / 2;
		w->z = (area[1] + area[3]) / 2;
	}

	if (!measure(setting_number("height", 0.6, 0.01, 100.0))) {
		log_message(SHADY_PLUGIN_LOG_ERROR, PREFIX ": %s: %s", path,
			gltf_error(w->model)[0] ? gltf_error(w->model) : "the model has no height");
		close_model();
		hide_scene();
		return;
	}
	w->speed = speed * w->scale;
	w->mode = IDLE;
	w->idle_left = 1.0;
	w->walk_weight = 0.0;
	submit();
}

static bool same(const char *a, const char *b) {
	return a == b || (a && b && !strcmp(a, b));
}

/* Reconfigure when a setting changed. */
static void poll_settings(void) {
	bool changed = false;
	for (size_t i = 0; i < SETTING_COUNT; ++i) {
		const char *now = published(setting_keys[i]);
		if (same(now, w->settings[i])) continue;
		free(w->settings[i]);
		w->settings[i] = now ? strdup(now) : NULL;
		changed = true;
	}
	if (changed) configure();
}

static bool wants_frames(void) {
	return w && (w->model || w->since_init < STARTUP_WATCH);
}

/* Draws nothing: it keeps frames coming. The host skips module ticks on the
 * first frame, so asking from tick alone could stall before the first one.
 * OVERLAY hooks run every frame; AFTER_BACKGROUND ones only when the cached
 * sky is redrawn, and asking for frames from one would redraw it each time. */
static void keep_frames(shady_host h, const struct shady_render_context *ctx, void *data) {
	(void)h;
	(void)ctx;
	(void)data;
	if (wants_frames()) api->schedule_render(host);
}

static bool wanderer_init(struct shady_server *server) {
	(void)server;
	w = calloc(1, sizeof(*w));
	if (!w) return false;
	w->rng = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull | 1;
	w->x = w->z = NAN; /* placed by configure */
	frame_hook = api->render_hook_add(host, SHADY_RENDER_STAGE_OVERLAY, keep_frames, NULL);
	api->schedule_render(host);
	poll_settings();
	if (!w->model && !setting("model"))
		api->log(SHADY_PLUGIN_LOG_INFO,
			PREFIX ": waiting for shady.publish(\"glb-wanderer.model\", \"/path/to.glb\")");
	return true;
}

static void wanderer_tick(struct shady_server *server, float dt, float width, float height) {
	(void)server;
	(void)width;
	(void)height;
	if (!w || !(dt > 0.0f)) return;
	/* A long stall (suspend, a slow frame) should not teleport her. */
	double step_dt = fmin((double)dt, 0.1);
	w->since_init += step_dt;
	w->since_poll += step_dt;
	/* Without a model, look every frame: the settings are about to come. */
	if (w->since_poll >= POLL_INTERVAL || !w->model) {
		w->since_poll = 0.0;
		poll_settings();
	}
	if (w->model) {
		step(step_dt);
		w->since_frame += step_dt;
		if (w->since_frame >= w->frame_interval) {
			w->since_frame = w->frame_interval > 0.0 ? fmod(w->since_frame, w->frame_interval) : 0.0;
			submit();
		}
	}
	if (wants_frames()) api->schedule_render(host);
}

static void wanderer_destroy(struct shady_server *server) {
	(void)server;
	if (frame_hook) api->render_hook_remove(host, frame_hook);
	frame_hook = 0;
	if (!w) return;
	hide_scene();
	close_model();
	for (size_t i = 0; i < SETTING_COUNT; ++i) free(w->settings[i]);
	free(w);
	w = NULL;
}

static const char *const provides[] = { "glb-wanderer", NULL };
static const char *const requires[] = { "spatial", NULL };

static const struct shady_module wanderer_module = {
	.name = "glb-wanderer",
	.provides = provides,
	.requires = requires,
	.init = wanderer_init,
	.destroy = wanderer_destroy,
	.tick = wanderer_tick,
};

const struct shady_module *shady_plugin_entry_v1(uint32_t host_abi,
		const struct shady_plugin_api_v1 *host_api, shady_host host_context) {
	if (host_abi != SHADY_PLUGIN_ABI_V1 || !host_api ||
			host_api->abi_version != SHADY_PLUGIN_ABI_V1 ||
			!SHADY_API_HAS(host_api, query_api) || !SHADY_API_HAS(host_api, published_value) ||
			!host_api->render_hook_add || !host_api->render_hook_remove || !host_api->schedule_render)
		return NULL;

	const struct shady_environment_api_v1 *env = host_api->query_api(host_context,
		SHADY_ENVIRONMENT_API, SHADY_ENVIRONMENT_API_VERSION);
	if (!env || env->struct_size < sizeof(*env) || !env->set_scene || !env->clear_scene)
		return NULL;

	api = host_api;
	environment = env;
	host = host_context;
	return &wanderer_module;
}
