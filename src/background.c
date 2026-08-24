#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <drm_fourcc.h>
#include <math.h>

static cairo_surface_t *load_image(const char *path) {
	int w, h, n;
	uint8_t *pixels = stbi_load(path, &w, &h, &n, 4);
	if (!pixels) {
		wlr_log(WLR_ERROR, "background: failed to load '%s'", path);
		return NULL;
	}
	/* stbi returns straight-alpha RGBA; cairo ARGB32 is premultiplied
	 * BGRA in memory on little-endian: swap R/B and premultiply */
	for (int i = 0; i < w * h; i++) {
		uint8_t *px = pixels + (size_t)i * 4;
		uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
		px[0] = (uint8_t)((int)b * a / 255);
		px[1] = (uint8_t)((int)g * a / 255);
		px[2] = (uint8_t)((int)r * a / 255);
	}
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_surface_set_user_data(surface, NULL, pixels, free);
	return surface;
}

void background_load_images(struct guibux_server *server) {
	/* workspaces without a per-workspace image share one decode of
	 * the default background */
	cairo_surface_t *fallback = NULL;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		const char *path = server->bg_paths[ws - 1]
			? server->bg_paths[ws - 1] : server->background_path;
		if (!path) {
			continue;
		}
		if (server->bg_paths[ws - 1] == NULL && fallback != NULL) {
			server->bg_surfaces[ws - 1] = fallback;
			continue;
		}
		cairo_surface_t *s = load_image(path);
		if (server->bg_paths[ws - 1] == NULL) {
			fallback = s;
		}
		server->bg_surfaces[ws - 1] = s;
	}
}

void background_destroy_images(struct guibux_server *server) {
	/* the fallback surface is shared between workspaces: the first slot
	 * holding it destroys it, the rest skip. The slots must stay intact
	 * during the scan (nulling them first would hide the share from the
	 * later workspaces and free the surface once per workspace) */
	for (int ws = 0; ws < NUM_WORKSPACES; ws++) {
		if (!server->bg_surfaces[ws]) {
			continue;
		}
		bool shared = false;
		for (int j = 0; j < ws; j++) {
			if (server->bg_surfaces[j] == server->bg_surfaces[ws]) {
				shared = true;
				break;
			}
		}
		if (!shared) {
			cairo_surface_destroy(server->bg_surfaces[ws]);
		}
	}
	for (int ws = 0; ws < NUM_WORKSPACES; ws++) {
		server->bg_surfaces[ws] = NULL;
		free(server->bg_paths[ws]);
		server->bg_paths[ws] = NULL;
	}
}

void background_create(struct guibux_output *o) {
	background_render(o);
}

static void background_drop_ws_buffers(struct guibux_output *o) {
	for (int i = 0; i < NUM_WORKSPACES; i++) {
		if (o->bg_ws_buffers[i]) {
			wlr_buffer_drop(o->bg_ws_buffers[i]);
			o->bg_ws_buffers[i] = NULL;
		}
	}
	o->bg_w = 0;
	o->bg_h = 0;
}

/* Render workspace ws's image into a fresh buffer at the output's
 * current size. The caller stores it in o->bg_ws_buffers[ws-1] so
 * workspace switches become a cheap buffer swap instead of a full
 * cairo rescale of the image */
static struct wlr_buffer *background_render_workspace(struct guibux_output *o,
		int ws, struct wlr_box box) {
	struct guibux_server *server = o->server;
	cairo_surface_t *img = server->bg_surfaces[ws - 1];
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int w = box.width * scale;
	int h = box.height * scale;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	struct wlr_buffer *buf = wlr_allocator_create_buffer(
		server->launcher.shm_alloc, w, h, &format);
	if (!buf) {
		wlr_log(WLR_ERROR, "background: failed to create buffer on %s",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return NULL;
	}

	void *data;
	uint32_t fmt;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buf,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &fmt, &stride)) {
		wlr_log(WLR_ERROR, "background: cannot access buffer data");
		wlr_buffer_drop(buf);
		return NULL;
	}
	if (fmt != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "background: unexpected buffer format 0x%x", fmt);
		wlr_buffer_end_data_ptr_access(buf);
		wlr_buffer_drop(buf);
		return NULL;
	}

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	int img_w = cairo_image_surface_get_width(img);
	int img_h = cairo_image_surface_get_height(img);

	switch (server->background_scale) {
	case BG_STRETCH:
		cairo_scale(cr, (double)w / img_w, (double)h / img_h);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
		break;

	case BG_FIT: {
		double scale_w = (double)w / img_w;
		double scale_h = (double)h / img_h;
		double s = fmin(scale_w, scale_h);
		double nw = img_w * s;
		double nh = img_h * s;
		double ox = (w - nw) / 2;
		double oy = (h - nh) / 2;
		cairo_translate(cr, ox, oy);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
		break;
	}

	case BG_FILL: {
		double scale_w = (double)w / img_w;
		double scale_h = (double)h / img_h;
		double s = fmax(scale_w, scale_h);
		double nw = img_w * s;
		double nh = img_h * s;
		double ox = (w - nw) / 2;
		double oy = (h - nh) / 2;
		cairo_translate(cr, ox, oy);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
		break;
	}

	case BG_TILE:
		/* repeat the image across the whole output; cairo clips each
		 * tile to the surface */
		for (int ty = 0; ty < h; ty += img_h) {
			for (int tx = 0; tx < w; tx += img_w) {
				cairo_save(cr);
				cairo_translate(cr, tx, ty);
				cairo_set_source_surface(cr, img, 0, 0);
				cairo_paint(cr);
				cairo_restore(cr);
			}
		}
		break;
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(buf);
	return buf;
}

void background_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	int ws = o->current_workspace;
	cairo_surface_t *img = server->bg_surfaces[ws - 1];

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0)
		return;

	if (!img) {
		if (o->bg_node) {
			background_drop_ws_buffers(o);
			wlr_scene_buffer_set_buffer(o->bg_node, NULL);
			wlr_output_schedule_frame(o->wlr_output);
		}
		return;
	}

	if (!o->bg_node) {
		o->bg_node = wlr_scene_buffer_create(&server->scene->tree, NULL);
		wlr_scene_node_set_position(&o->bg_node->node, box.x, box.y);
	}

	/* output resized: all cached workspace buffers are stale */
	if (o->bg_w != box.width || o->bg_h != box.height) {
		background_drop_ws_buffers(o);
		o->bg_w = box.width;
		o->bg_h = box.height;
	}

	/* already rendered for this workspace: just swap the buffer in */
	if (o->bg_ws_buffers[ws - 1]) {
		wlr_scene_buffer_set_buffer(o->bg_node, o->bg_ws_buffers[ws - 1]);
		wlr_output_schedule_frame(o->wlr_output);
		return;
	}

	struct wlr_buffer *buf = background_render_workspace(o, ws, box);
	if (!buf) {
		return;
	}
	o->bg_ws_buffers[ws - 1] = buf;
	wlr_scene_buffer_set_buffer(o->bg_node, buf);
	wlr_output_schedule_frame(o->wlr_output);
}

void background_destroy(struct guibux_output *o) {
	if (o->bg_node) {
		wlr_scene_node_destroy(&o->bg_node->node);
		o->bg_node = NULL;
	}
	background_drop_ws_buffers(o);
}
