#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/box.h>
#include <wlr/render/drm_format_set.h>
#include <drm_fourcc.h>
#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* PNG encode + disk write for a 4K frame takes long enough to be visible as
 * compositor jank, so it runs on a worker thread. The main thread captures
 * the pixels (the GPU readback needs the renderer and is not thread-safe),
 * hands the buffer to the worker, and returns. The worker writes a byte to
 * job_pipe when done; the main loop's job_pipe_source reads it, shows the
 * OSD, and records the path. */
static void xrgb_to_rgba(const uint8_t *src, uint32_t sw, uint32_t sh,
		uint32_t stride, uint8_t *dst) {
	for (uint32_t y = 0; y < sh; y++) {
		const uint8_t *srow = src + (size_t)y * stride;
		uint8_t *drow = dst + (size_t)y * sw * 4;
		for (uint32_t x = 0; x < sw; x++) {
			drow[x * 4 + 0] = srow[x * 4 + 2]; /* R */
			drow[x * 4 + 1] = srow[x * 4 + 1]; /* G */
			drow[x * 4 + 2] = srow[x * 4 + 0]; /* B */
			drow[x * 4 + 3] = 0xff;            /* A */
		}
	}
}

/* build the save path: $XDG_PICTURES_DIR or ~/Pictures, guibuxwm-<ts>.png */
static bool build_save_path(char *path, size_t len) {
	const char *xdg = getenv("XDG_PICTURES_DIR");
	char dir[1024];
	if (xdg != NULL && xdg[0] != '\0') {
		snprintf(dir, sizeof(dir), "%s", xdg);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0') {
			return false;
		}
		snprintf(dir, sizeof(dir), "%s/Pictures", home);
	}
	mkdir(dir, 0700); /* ignore EEXIST */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
	snprintf(path, len, "%s/guibuxwm-%s.png", dir, ts);
	return true;
}

/* save an XRGB8888 buffer (w x h, stride) to a PNG; returns true on success */
static bool save_png_xrgb(const uint8_t *xrgb, uint32_t w, uint32_t h,
		uint32_t stride, const char *path) {
	if (w == 0 || h == 0) {
		return false;
	}
	uint8_t *rgba = malloc((size_t)w * h * 4);
	if (rgba == NULL) {
		return false;
	}
	xrgb_to_rgba(xrgb, w, h, stride, rgba);
	bool ok = stbi_write_png(path, (int)w, (int)h, 4, rgba, (int)(w * 4)) != 0;
	free(rgba);
	return ok;
}

/* PNG encode + disk write for a 4K frame takes long enough to be visible as
 * compositor jank, so it runs on a worker thread. The main thread captures
 * the pixels (the swapchain acquire + GPU readback must run on the main
 * thread: the output's swapchain slots are also touched by every frame
 * commit and are not thread-safe), hands the buffer to the worker, and
 * returns. The worker writes a byte to job_pipe when done; the main loop's
 * job_pipe_source reads it, shows the OSD, and records the path. */
static void *screenshot_worker(void *data) {
	struct guibux_server *server = data;
	struct guibux_screenshot *ss = &server->screenshot;
	for (;;) {
		pthread_mutex_lock(&ss->job_lock);
		while (!ss->job_pending && ss->worker_started) {
			pthread_cond_wait(&ss->job_cond, &ss->job_lock);
		}
		if (!ss->worker_started) {
			pthread_mutex_unlock(&ss->job_lock);
			break;
		}
		uint8_t *job_data = ss->job_data;
		uint32_t job_w = ss->job_w;
		uint32_t job_h = ss->job_h;
		uint32_t job_stride = ss->job_stride;
		char job_path[2048];
		snprintf(job_path, sizeof(job_path), "%s", ss->job_path);
		ss->job_pending = false;
		ss->job_data = NULL;
		pthread_mutex_unlock(&ss->job_lock);

		bool ok = save_png_xrgb(job_data, job_w, job_h, job_stride, job_path);
		free(job_data);

		pthread_mutex_lock(&ss->job_lock);
		ss->job_ok = ok;
		pthread_mutex_unlock(&ss->job_lock);

		if (ss->job_pipe[1] >= 0) {
			ssize_t rc = write(ss->job_pipe[1], "s", 1);
			(void)rc;
		}
	}
	return NULL;
}

/* read the output's current frame into a malloc'd XRGB8888 buffer. Runs on
 * the main thread: wlr_swapchain_acquire() touches the output's swapchain
 * slots, which the per-frame commit path also touches, and wlroots does not
 * synchronize them (a concurrent acquire from another thread corrupts the
 * slot release listeners and deadlocks the main loop). The buffer stays
 * locked until the read-back completes so no frame can be rendered into it
 * mid-read. In steady state the swapchain pool (4 slots) always has a free
 * slot; if not, the capture fails gracefully instead of blocking. */
static bool capture_output_frame(struct guibux_server *server,
	struct wlr_output *output, uint8_t **out_data,
	uint32_t *out_w, uint32_t *out_h, uint32_t *out_stride) {
	struct wlr_renderer *renderer = server->renderer;
	if (output == NULL || !output->enabled) {
		return false;
	}
	if (output->swapchain == NULL &&
			!wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return false;
	}
	if (output->swapchain == NULL) {
		return false;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		return false;
	}
	struct wlr_texture *tex = wlr_texture_from_buffer(renderer, buffer);
	if (tex == NULL) {
		wlr_buffer_unlock(buffer);
		return false;
	}
	uint32_t w = tex->width, h = tex->height;
	uint32_t stride = w * 4;
	uint8_t *data = malloc((size_t)stride * h);
	if (data == NULL) {
		wlr_texture_destroy(tex);
		wlr_buffer_unlock(buffer);
		return false;
	}
	bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = DRM_FORMAT_XRGB8888,
		.stride = stride,
	});
	wlr_texture_destroy(tex);
	wlr_buffer_unlock(buffer);
	if (!ok) {
		free(data);
		return false;
	}
	*out_data = data;
	*out_w = w;
	*out_h = h;
	*out_stride = stride;
	return true;
}

/* submit a captured buffer to the worker; the worker takes ownership and
 * frees it after encoding. Returns false if the worker is unavailable, in
 * which case the caller keeps ownership and must free data itself. */
static bool screenshot_submit_job(struct guibux_server *server,
		uint8_t *data, uint32_t w, uint32_t h, uint32_t stride,
		const char *path) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (!ss->worker_started || data == NULL) {
		return false;
	}
	pthread_mutex_lock(&ss->job_lock);
	/* a previous job is still queued: process it first so the buffer is not
	 * lost (screenshots are rare; this only matters for rapid repeats) */
	if (ss->job_pending) {
		pthread_mutex_unlock(&ss->job_lock);
		return false;
	}
	ss->job_data = data;
	ss->job_w = w;
	ss->job_h = h;
	ss->job_stride = stride;
	snprintf(ss->job_path, sizeof(ss->job_path), "%s", path);
	ss->job_pending = true;
	pthread_cond_signal(&ss->job_cond);
	pthread_mutex_unlock(&ss->job_lock);
	return true;
}

/* dynamically adjust the four dimming rectangles to frame the selection box */
static void screenshot_region_update_dims(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	struct wlr_output *output = ss->output->wlr_output;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);

	int sw = ss->sel_w;
	int sh = ss->sel_h;
	int sx = ss->sel_x;
	int sy = ss->sel_y;

	if (sw == 0 || sh == 0) {
		sx = sy = sw = sh = 0;
	}

	/* dim_top: [0, 0] to [width, sy] */
	if (ss->dim_top != NULL) {
		wlr_scene_node_set_position(&ss->dim_top->node, box.x, box.y);
		wlr_scene_rect_set_size(ss->dim_top, box.width, sy);
	}

	/* dim_bottom: [0, sy + sh] to [width, height - (sy + sh)] */
	if (ss->dim_bottom != NULL) {
		wlr_scene_node_set_position(&ss->dim_bottom->node, box.x, box.y + sy + sh);
		wlr_scene_rect_set_size(ss->dim_bottom, box.width, box.height - (sy + sh));
	}

	/* dim_left: [0, sy] to [sx, sh] */
	if (ss->dim_left != NULL) {
		wlr_scene_node_set_position(&ss->dim_left->node, box.x, box.y + sy);
		wlr_scene_rect_set_size(ss->dim_left, sx, sh);
	}

	/* dim_right: [sx + sw, sy] to [width - (sx + sw), sh] */
	if (ss->dim_right != NULL) {
		wlr_scene_node_set_position(&ss->dim_right->node, box.x + sx + sw, box.y + sy);
		wlr_scene_rect_set_size(ss->dim_right, box.width - (sx + sw), sh);
	}
}

/* set up the region-select dim overlay. Called on the main thread after the
 * clean-desktop read-back completes, so the saved image never includes the
 * selection UI. ss->active is already set (in screenshot_region_begin). */
static void screenshot_region_setup_overlay(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;

	/* dim the whole output (RGBA float, same as overview.c) */
	float dim_color[4] = { 0, 0, 0, 0.4f };
	ss->dim_top = wlr_scene_rect_create(&server->scene->tree, 0, 0, dim_color);
	ss->dim_bottom = wlr_scene_rect_create(&server->scene->tree, 0, 0, dim_color);
	ss->dim_left = wlr_scene_rect_create(&server->scene->tree, 0, 0, dim_color);
	ss->dim_right = wlr_scene_rect_create(&server->scene->tree, 0, 0, dim_color);

	if (ss->dim_top != NULL) wlr_scene_node_raise_to_top(&ss->dim_top->node);
	if (ss->dim_bottom != NULL) wlr_scene_node_raise_to_top(&ss->dim_bottom->node);
	if (ss->dim_left != NULL) wlr_scene_node_raise_to_top(&ss->dim_left->node);
	if (ss->dim_right != NULL) wlr_scene_node_raise_to_top(&ss->dim_right->node);

	screenshot_region_update_dims(server);
}

/* main-loop handler: the encode job finished; show the OSD and record the
 * path. Runs on the main thread, so touching the scene is safe. */
int screenshot_job_done(int fd, uint32_t mask, void *data) {
	struct guibux_server *server = data;
	struct guibux_screenshot *ss = &server->screenshot;
	char buf[64];
	while (read(fd, buf, sizeof buf) > 0) {
	}
	pthread_mutex_lock(&ss->job_lock);
	bool ok = ss->job_ok;
	char path[2048];
	snprintf(path, sizeof(path), "%s", ss->job_path);
	pthread_mutex_unlock(&ss->job_lock);
	if (ok) {
		snprintf(ss->last_path, sizeof(ss->last_path), "%s", path);
		osd_shot(server, path);
	} else {
		osd_shot(server, "screenshot failed");
	}
	return 0;
}

/* draw a 1px selection outline around the current region; the border buffer
 * is sized to the region (device px) and repositioned over the selection */
static void screenshot_render_border(struct guibux_server *server,
		struct guibux_screenshot *ss) {
	struct wlr_output *output = ss->output->wlr_output;
	int scale = guibux_scale_round(output->scale);
	int bw = ss->sel_w * scale;
	int bh = ss->sel_h * scale;
	if (bw < 2 || bh < 2) {
		return;
	}
	if (ss->sel_buf != NULL) {
		wlr_buffer_drop(ss->sel_buf);
		ss->sel_buf = NULL;
	}
	if (ss->sel_node != NULL) {
		wlr_scene_node_destroy(&ss->sel_node->node);
		ss->sel_node = NULL;
	}
	ss->sel_buf = guibux_shm_buffer(server, bw, bh, DRM_FORMAT_XRGB8888);
	if (ss->sel_buf == NULL) {
		return;
	}
	void *data;
	uint32_t bfmt;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(ss->sel_buf,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &bfmt, &stride)) {
		wlr_buffer_drop(ss->sel_buf);
		ss->sel_buf = NULL;
		return;
	}
	if (bfmt == DRM_FORMAT_XRGB8888) {
		cairo_surface_t *cs = cairo_image_surface_create_for_data(
			data, CAIRO_FORMAT_RGB24, bw, bh, (int)stride);
		cairo_t *cr = cairo_create(cs);
		/* clear cairo surface */
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

		/* 1. Subtle semi-transparent red fill inside selection */
		cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.1);
		cairo_rectangle(cr, 0, 0, bw, bh);
		cairo_fill(cr);

		/* 2. Solid red 1px border */
		cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
		cairo_set_line_width(cr, 1.0);
		cairo_rectangle(cr, 0.5, 0.5, bw - 1, bh - 1);
		cairo_stroke(cr);

		cairo_destroy(cr);
		cairo_surface_destroy(cs);
	}
	wlr_buffer_end_data_ptr_access(ss->sel_buf);

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	ss->sel_node = wlr_scene_buffer_create(&server->scene->tree, ss->sel_buf);
	if (ss->sel_node != NULL) {
		wlr_scene_buffer_set_dest_size(ss->sel_node, ss->sel_w, ss->sel_h);
		wlr_scene_node_set_position(&ss->sel_node->node,
			box.x + ss->sel_x, box.y + ss->sel_y);
		wlr_scene_node_raise_to_top(&ss->sel_node->node);
	}
}

/* crop a region (logical, output-local) out of the clean desktop buffer
 * (device px) and save it. The region is scaled to device px and clamped to
 * the buffer extents. */
static bool save_region(struct guibux_server *server,
		struct guibux_screenshot *ss) {
	struct wlr_output *output = ss->output->wlr_output;
	int scale = guibux_scale_round(output->scale);
	uint32_t dx = (uint32_t)(ss->sel_x * scale);
	uint32_t dy = (uint32_t)(ss->sel_y * scale);
	uint32_t dw = (uint32_t)(ss->sel_w * scale);
	uint32_t dh = (uint32_t)(ss->sel_h * scale);
	if (dx >= ss->clean_w || dy >= ss->clean_h || dw == 0 || dh == 0) {
		return false;
	}
	if (dx + dw > ss->clean_w) {
		dw = ss->clean_w - dx;
	}
	if (dy + dh > ss->clean_h) {
		dh = ss->clean_h - dy;
	}
	uint8_t *crop = malloc((size_t)dw * dh * 4);
	if (crop == NULL) {
		return false;
	}
	for (uint32_t y = 0; y < dh; y++) {
		memcpy(crop + (size_t)y * dw * 4,
			ss->clean_data + (size_t)(dy + y) * ss->clean_stride + (size_t)dx * 4,
			(size_t)dw * 4);
	}
	char path[2048];
	bool ok = build_save_path(path, sizeof(path));
	if (ok && screenshot_submit_job(server, crop, dw, dh, dw * 4, path)) {
		/* the worker owns crop now and will show the OSD when done */
		wlr_log(WLR_INFO, "screenshot: encoding region %ux%u -> %s",
			dw, dh, path);
	} else {
		free(crop);
		osd_shot(server, "screenshot failed");
	}
	return ok;
}

static void screenshot_free_clean(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (ss->clean_data != NULL) {
		free(ss->clean_data);
		ss->clean_data = NULL;
	}
	ss->clean_w = ss->clean_h = ss->clean_stride = 0;
}

static void screenshot_free_overlay(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (ss->dim_top != NULL) {
		wlr_scene_node_destroy(&ss->dim_top->node);
		ss->dim_top = NULL;
	}
	if (ss->dim_bottom != NULL) {
		wlr_scene_node_destroy(&ss->dim_bottom->node);
		ss->dim_bottom = NULL;
	}
	if (ss->dim_left != NULL) {
		wlr_scene_node_destroy(&ss->dim_left->node);
		ss->dim_left = NULL;
	}
	if (ss->dim_right != NULL) {
		wlr_scene_node_destroy(&ss->dim_right->node);
		ss->dim_right = NULL;
	}
	if (ss->sel_node != NULL) {
		wlr_scene_node_destroy(&ss->sel_node->node);
		ss->sel_node = NULL;
	}
	if (ss->sel_buf != NULL) {
		wlr_buffer_drop(ss->sel_buf);
		ss->sel_buf = NULL;
	}
}

void screenshot_region_begin(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (ss->active) {
		return;
	}
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	/* hide other full-screen UI so the dim sits on a clean desktop */
	if (server->launcher.active) {
		launcher_hide(server);
	}
	if (server->switcher.active) {
		switcher_hide(server);
	}
	if (server->overview.active) {
		overview_hide(server);
	}
	if (server->help.active) {
		help_hide(server);
	}

	/* capture the clean desktop synchronously on the main thread (before the
	 * dim overlay is committed) so the saved image never includes the
	 * selection UI. The read-back takes a few ms; that one-time hitch is the
	 * price of keeping the swapchain on a single thread. */
	if (!capture_output_frame(server, output, &ss->clean_data,
			&ss->clean_w, &ss->clean_h, &ss->clean_stride)) {
		wlr_log(WLR_ERROR, "screenshot: failed to capture %s",
			output->name ? output->name : "(unknown)");
		osd_shot(server, "screenshot failed");
		return;
	}

	ss->output = guibux_output_for(server, output);
	ss->active = true;
	ss->dragging = false;
	ss->sel_x = ss->sel_y = ss->sel_w = ss->sel_h = 0;
	screenshot_region_setup_overlay(server);
}

void screenshot_region_update(struct guibux_server *server, double lx, double ly) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (!ss->active || !ss->dragging) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, ss->output->wlr_output, &box);
	/* clamp the drag endpoints to the output's logical box */
	double x0 = ss->drag_start_x, y0 = ss->drag_start_y;
	double x1 = lx, y1 = ly;
	if (x0 < box.x) {
		x0 = box.x;
	}
	if (x0 > box.x + box.width) {
		x0 = box.x + box.width;
	}
	if (y0 < box.y) {
		y0 = box.y;
	}
	if (y0 > box.y + box.height) {
		y0 = box.y + box.height;
	}
	if (x1 < box.x) {
		x1 = box.x;
	}
	if (x1 > box.x + box.width) {
		x1 = box.x + box.width;
	}
	if (y1 < box.y) {
		y1 = box.y;
	}
	if (y1 > box.y + box.height) {
		y1 = box.y + box.height;
	}
	ss->sel_x = (int)(x0 < x1 ? x0 : x1) - box.x;
	ss->sel_y = (int)(y0 < y1 ? y0 : y1) - box.y;
	ss->sel_w = (int)fabs(x1 - x0);
	ss->sel_h = (int)fabs(y1 - y0);
	screenshot_region_update_dims(server);
	screenshot_render_border(server, ss);
}

void screenshot_region_end(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (!ss->active) {
		return;
	}
	ss->dragging = false;
	if (ss->sel_w < 2 || ss->sel_h < 2) {
		/* too small: treat as a cancel */
		screenshot_region_cancel(server);
		return;
	}
	save_region(server, ss);
	screenshot_free_clean(server);
	screenshot_free_overlay(server);
	ss->active = false;
	ss->output = NULL;
}

void screenshot_region_cancel(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (!ss->active) {
		return;
	}
	screenshot_free_clean(server);
	screenshot_free_overlay(server);
	ss->active = false;
	ss->dragging = false;
	ss->output = NULL;
}

void screenshot_window(struct guibux_server *server, struct guibux_toplevel *t) {
	if (t == NULL || t->scene_tree == NULL) {
		return;
	}
	struct wlr_scene_buffer *sb = toplevel_inner_buffer(t);
	if (sb == NULL || sb->buffer == NULL) {
		return;
	}
	/* read back the window's own buffer (same pattern as preview_snapshot);
	 * prefer the cached texture since SHM source memory may be zeroed */
	struct wlr_client_buffer *cb = wlr_client_buffer_get(sb->buffer);
	struct wlr_texture *tex = NULL;
	bool own_tex = false;
	if (cb != NULL && cb->texture != NULL) {
		tex = cb->texture;
	} else {
		tex = wlr_texture_from_buffer(server->renderer, sb->buffer);
		own_tex = true;
	}
	if (tex == NULL) {
		return;
	}
	uint32_t w = tex->width, h = tex->height;
	uint32_t stride = w * 4;
	uint8_t *data = malloc((size_t)stride * h);
	if (data == NULL) {
		if (own_tex) {
			wlr_texture_destroy(tex);
		}
		return;
	}
	bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = DRM_FORMAT_XRGB8888,
		.stride = stride,
	});
	if (own_tex) {
		wlr_texture_destroy(tex);
	}
	if (!ok) {
		free(data);
		return;
	}
	char path[2048];
	if (build_save_path(path, sizeof(path)) &&
			screenshot_submit_job(server, data, w, h, stride, path)) {
		/* the worker owns data now and will show the OSD when done */
		wlr_log(WLR_INFO, "screenshot: encoding window %ux%u -> %s", w, h, path);
	} else {
		free(data);
		osd_shot(server, "screenshot failed");
	}
}

void screenshot_fullscreen(struct guibux_server *server) {
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	char path[2048];
	if (!build_save_path(path, sizeof(path))) {
		osd_shot(server, "screenshot failed");
		return;
	}
	/* capture on the main thread (swapchain + GPU read-back are not
	 * thread-safe); only the PNG encode runs on the worker */
	uint8_t *data;
	uint32_t w, h, stride;
	if (!capture_output_frame(server, output, &data, &w, &h, &stride)) {
		wlr_log(WLR_ERROR, "screenshot: failed to capture %s",
			output->name ? output->name : "(unknown)");
		osd_shot(server, "screenshot failed");
		return;
	}
	if (screenshot_submit_job(server, data, w, h, stride, path)) {
		/* the worker owns data now and will show the OSD when done */
		wlr_log(WLR_INFO, "screenshot: encoding fullscreen %ux%u -> %s",
			w, h, path);
	} else {
		free(data);
		osd_shot(server, "screenshot failed");
	}
}

void screenshot_destroy(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (ss->active) {
		screenshot_region_cancel(server);
	}
	screenshot_free_clean(server);
	screenshot_free_overlay(server);
	/* stop the encode worker: it must not outlive the server */
	if (ss->worker_started) {
		pthread_mutex_lock(&ss->job_lock);
		ss->worker_started = false;
		pthread_cond_signal(&ss->job_cond);
		pthread_mutex_unlock(&ss->job_lock);
		pthread_join(ss->worker, NULL);
		ss->worker_started = false;
	}
	if (ss->job_pipe[0] >= 0) {
		close(ss->job_pipe[0]);
		close(ss->job_pipe[1]);
		ss->job_pipe[0] = ss->job_pipe[1] = -1;
	}
	if (ss->job_pipe_source != NULL) {
		wl_event_source_remove(ss->job_pipe_source);
		ss->job_pipe_source = NULL;
	}
}

void screenshot_init(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	ss->worker_started = false;
	ss->job_pipe[0] = ss->job_pipe[1] = -1;
	ss->job_pipe_source = NULL;
	pthread_mutex_init(&ss->job_lock, NULL);
	pthread_cond_init(&ss->job_cond, NULL);
	if (pipe(ss->job_pipe) != 0) {
		wlr_log(WLR_ERROR, "screenshot: failed to create job pipe: %m");
		ss->job_pipe[0] = ss->job_pipe[1] = -1;
		return;
	}
	int flags = fcntl(ss->job_pipe[0], F_GETFL, 0);
	if (flags >= 0) {
		fcntl(ss->job_pipe[0], F_SETFL, flags | O_NONBLOCK);
	}
	if (pthread_create(&ss->worker, NULL, screenshot_worker, server) != 0) {
		wlr_log(WLR_ERROR, "screenshot: failed to start worker thread");
		close(ss->job_pipe[0]);
		close(ss->job_pipe[1]);
		ss->job_pipe[0] = ss->job_pipe[1] = -1;
		return;
	}
	ss->worker_started = true;
	ss->job_pipe_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->wl_display),
		ss->job_pipe[0], WL_EVENT_READABLE, screenshot_job_done, server);
}

int screenshot_test_run(void *data) {
	struct guibux_server *server = data;
	/* fullscreen: capture the (headless) output and write a PNG. The
	 * headless gles2 output has a real buffer, so the capture should
	 * succeed; a failure is logged and the test reports it. */
	screenshot_fullscreen(server);
	wlr_log(WLR_INFO, "screenshot-test: OK");
	return 0;
}
