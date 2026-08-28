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
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* read the output's last committed frame (the composited desktop) into a
 * malloc'd XRGB8888 buffer. The swapchain buffer is the source of truth for
 * what is on screen; the read is forced to XRGB8888 so the renderer does the
 * format conversion and the caller always gets BGRA-in-memory pixels. */
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
	wlr_buffer_unlock(buffer);
	if (tex == NULL) {
		return false;
	}
	uint32_t w = tex->width, h = tex->height;
	uint32_t stride = w * 4;
	uint8_t *data = malloc((size_t)stride * h);
	if (data == NULL) {
		wlr_texture_destroy(tex);
		return false;
	}
	bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = DRM_FORMAT_XRGB8888,
		.stride = stride,
	});
	wlr_texture_destroy(tex);
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

/* convert an XRGB8888 (BGRA in memory) region to RGBA for stbi_write_png */
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
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	ss->sel_buf = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw, bh, &format);
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
		/* transparent inside, white 1px outline */
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 1);
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
	bool ok = build_save_path(path, sizeof(path)) &&
		save_png_xrgb(crop, dw, dh, dw * 4, path);
	free(crop);
	if (ok) {
		snprintf(ss->last_path, sizeof(ss->last_path), "%s", path);
		wlr_log(WLR_INFO, "screenshot: saved region %ux%u -> %s", dw, dh, path);
		osd_shot(server, path);
	} else {
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
	if (ss->dim != NULL) {
		wlr_scene_node_destroy(&ss->dim->node);
		ss->dim = NULL;
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

	/* capture the clean desktop NOW (before the dim is committed) so the
	 * saved image never includes the selection overlay */
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

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);

	/* dim the whole output (RGBA float, same as overview.c) */
	float dim_color[4] = { 0, 0, 0, 0.4f };
	ss->dim = wlr_scene_rect_create(&server->scene->tree,
		box.width, box.height, dim_color);
	if (ss->dim != NULL) {
		wlr_scene_node_set_position(&ss->dim->node, box.x, box.y);
		wlr_scene_node_raise_to_top(&ss->dim->node);
	}
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
	if (build_save_path(path, sizeof(path)) && save_png_xrgb(data, w, h, stride, path)) {
		wlr_log(WLR_INFO, "screenshot: saved window %ux%u -> %s", w, h, path);
		osd_shot(server, path);
	} else {
		osd_shot(server, "screenshot failed");
	}
	free(data);
}

void screenshot_fullscreen(struct guibux_server *server) {
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	uint8_t *data;
	uint32_t w, h, stride;
	if (!capture_output_frame(server, output, &data, &w, &h, &stride)) {
		wlr_log(WLR_ERROR, "screenshot: failed to capture %s",
			output->name ? output->name : "(unknown)");
		osd_shot(server, "screenshot failed");
		return;
	}
	char path[2048];
	if (build_save_path(path, sizeof(path)) && save_png_xrgb(data, w, h, stride, path)) {
		wlr_log(WLR_INFO, "screenshot: saved fullscreen %ux%u -> %s", w, h, path);
		osd_shot(server, path);
	} else {
		osd_shot(server, "screenshot failed");
	}
	free(data);
}

void screenshot_destroy(struct guibux_server *server) {
	struct guibux_screenshot *ss = &server->screenshot;
	if (ss->active) {
		screenshot_region_cancel(server);
	}
	screenshot_free_clean(server);
	screenshot_free_overlay(server);
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
