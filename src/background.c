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
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_surface_set_user_data(surface, NULL, pixels, free);
	return surface;
}

void background_create(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (!server->background_path)
		return;

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0)
		return;

	o->bg_surface = load_image(server->background_path);
	if (!o->bg_surface)
		return;

	o->bg_node = wlr_scene_buffer_create(&server->scene->tree, NULL);
	wlr_scene_node_set_position(&o->bg_node->node, box.x, box.y);
	o->bg_w = box.width;
	o->bg_h = box.height;
	background_render(o);
}

void background_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (!o->bg_surface || !o->bg_node)
		return;

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int w = box.width * scale;
	int h = box.height * scale;

	if (o->bg_buffer && (o->bg_w != box.width || o->bg_h != box.height)) {
		wlr_buffer_drop(o->bg_buffer);
		o->bg_buffer = NULL;
		o->bg_w = 0;
		o->bg_h = 0;
	}

	if (!o->bg_buffer) {
		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format format = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		o->bg_buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
			w, h, &format);
		if (!o->bg_buffer) {
			wlr_log(WLR_ERROR, "background: failed to create buffer on %s",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)");
			return;
		}
		o->bg_w = box.width;
		o->bg_h = box.height;
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->bg_buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "background: cannot access buffer data");
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "background: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(o->bg_buffer);
		return;
	}

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	int img_w = cairo_image_surface_get_width(o->bg_surface);
	int img_h = cairo_image_surface_get_height(o->bg_surface);

	switch (server->background_scale) {
	case BG_STRETCH:
		cairo_scale(cr, (double)w / img_w, (double)h / img_h);
		cairo_set_source_surface(cr, o->bg_surface, 0, 0);
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
		cairo_set_source_surface(cr, o->bg_surface, 0, 0);
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
		cairo_set_source_surface(cr, o->bg_surface, 0, 0);
		cairo_paint(cr);
		break;
	}

	case BG_TILE:
		cairo_set_source_surface(cr, o->bg_surface, 0, 0);
		cairo_paint(cr);
		break;
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->bg_buffer);
	wlr_scene_buffer_set_buffer(o->bg_node, o->bg_buffer);
	wlr_output_schedule_frame(o->wlr_output);
}

void background_destroy(struct guibux_output *o) {
	if (o->bg_node) {
		wlr_scene_node_destroy(&o->bg_node->node);
		o->bg_node = NULL;
	}
	if (o->bg_buffer) {
		wlr_buffer_drop(o->bg_buffer);
		o->bg_buffer = NULL;
	}
	if (o->bg_surface) {
		cairo_surface_destroy(o->bg_surface);
		o->bg_surface = NULL;
	}
}
