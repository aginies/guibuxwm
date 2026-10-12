#include "guibuxwm.h"
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <string.h>

// ---------------------------------------------------------------------------
// panel - the plumbing shared by every overlay the compositor draws itself
// (power menu, topbar items, outputs panel, help, OSD, switcher, the
// notification panel and tooltips).
//
// Each overlay keeps its own state struct and its own drawing code; what is
// shared is the buffer/scene-node lifecycle, the cairo draw session over an
// XRGB8888 shm buffer, the translucent frame most of them use, and the
// centred-box hit test. All panels draw in *device* pixels (logical size x
// output scale) and are published at their logical size via the scene
// buffer's dest size, so they stay sharp on fractional-scale outputs.
// ---------------------------------------------------------------------------

/* top-left of a centred box_w x box_h panel in output-local logical px;
 * false when there is no output to centre on */
static bool panel_centred_local(struct guibux_server *server,
		struct wlr_output *output, int box_w, int box_h,
		int *px, int *py) {
	if (output == NULL) {
		return false;
	}
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	*px = (ew - box_w) / 2;
	*py = (eh - box_h) / 2;
	return true;
}

bool panel_begin_draw(struct wlr_buffer *buffer, int dev_w, int dev_h,
		int scale, struct guibux_panel_draw *draw) {
	draw->surface = NULL;
	draw->cr = NULL;
	draw->w = dev_w;
	draw->h = dev_h;
	draw->scale = scale;
	if (buffer == NULL || dev_w <= 0 || dev_h <= 0) {
		return false;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return false;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "panel: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(buffer);
		return false;
	}
	/* clear first: the buffer is reused across redraws and cairo only
	 * writes the pixels the drawing covers */
	memset(data, 0, stride * (size_t)dev_h);
	draw->surface = cairo_image_surface_create_for_data(data,
		CAIRO_FORMAT_RGB24, dev_w, dev_h, (int)stride);
	if (cairo_surface_status(draw->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(draw->surface);
		draw->surface = NULL;
		wlr_buffer_end_data_ptr_access(buffer);
		return false;
	}
	draw->cr = cairo_create(draw->surface);
	if (cairo_status(draw->cr) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(draw->cr);
		cairo_surface_destroy(draw->surface);
		draw->cr = NULL;
		draw->surface = NULL;
		wlr_buffer_end_data_ptr_access(buffer);
		return false;
	}
	return true;
}

void panel_end_draw(struct guibux_panel_draw *draw, struct wlr_buffer *buffer,
		struct wlr_scene_buffer *node, struct wlr_output *output) {
	if (draw->cr != NULL) {
		cairo_destroy(draw->cr);
		draw->cr = NULL;
	}
	if (draw->surface != NULL) {
		cairo_surface_destroy(draw->surface);
		draw->surface = NULL;
	}
	if (buffer != NULL) {
		wlr_buffer_end_data_ptr_access(buffer);
	}
	if (node != NULL && buffer != NULL) {
		wlr_scene_buffer_set_buffer(node, buffer);
	}
	if (output != NULL) {
		wlr_output_schedule_frame(output);
	}
}

void panel_draw_frame(const struct guibux_panel_draw *draw, uint32_t color_bg) {
	cairo_t *cr = draw->cr;
	int scale = draw->scale;
	/* translucent rounded background */
	cairo_set_source_rgba(cr,
		((color_bg >> 16) & 0xFF) / 255.0,
		((color_bg >> 8) & 0xFF) / 255.0,
		(color_bg & 0xFF) / 255.0, PANEL_BG_ALPHA);
	topbar_rounded_rect(cr, 0, 0, draw->w, draw->h,
		PANEL_RADIUS * scale);
	cairo_fill(cr);
	/* subtle border, inset by half the line width so it stays inside */
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, PANEL_BORDER_ALPHA);
	cairo_set_line_width(cr, scale);
	topbar_rounded_rect(cr, scale / 2.0, scale / 2.0,
		draw->w - scale, draw->h - scale,
		(PANEL_RADIUS - 1) * scale);
	cairo_stroke(cr);
}

void panel_centred_pos(struct guibux_server *server, struct wlr_output *output,
		int box_w, int box_h, int *lx, int *ly) {
	int px, py;
	if (!panel_centred_local(server, output, box_w, box_h, &px, &py)) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (lx != NULL) {
		*lx = box.x + px;
	}
	if (ly != NULL) {
		*ly = box.y + py;
	}
}

bool panel_create(struct guibux_server *server, int lx, int ly, int scale,
		int box_w, int box_h, struct wlr_buffer **buffer,
		struct wlr_scene_buffer **node) {
	*buffer = NULL;
	*node = NULL;
	if (box_w <= 0 || box_h <= 0 || scale < 1) {
		return false;
	}
	*buffer = guibux_shm_buffer(server, box_w * scale, box_h * scale,
		DRM_FORMAT_XRGB8888);
	if (*buffer == NULL) {
		return false;
	}
	*node = wlr_scene_buffer_create(&server->scene->tree, *buffer);
	if (*node == NULL) {
		wlr_log(WLR_ERROR, "panel: failed to create scene node");
		wlr_buffer_drop(*buffer);
		*buffer = NULL;
		return false;
	}
	wlr_scene_buffer_set_dest_size(*node, box_w, box_h);
	wlr_scene_node_set_position(&(*node)->node, lx, ly);
	return true;
}

void panel_destroy(struct wlr_buffer **buffer, struct wlr_scene_buffer **node) {
	if (node != NULL && *node != NULL) {
		wlr_scene_node_destroy(&(*node)->node);
		*node = NULL;
	}
	if (buffer != NULL && *buffer != NULL) {
		wlr_buffer_drop(*buffer);
		*buffer = NULL;
	}
}

bool panel_centred_box_at(struct guibux_server *server,
		struct wlr_output *output, int box_w, int box_h, double lx, double ly,
		int *out_px, int *out_py) {
	int px, py;
	if (!panel_centred_local(server, output, box_w, box_h, &px, &py)) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	double x = lx - box.x;
	double y = ly - box.y;
	if (x < px || x >= px + box_w || y < py || y >= py + box_h) {
		return false;
	}
	if (out_px != NULL) {
		*out_px = px;
	}
	if (out_py != NULL) {
		*out_py = py;
	}
	return true;
}

int panel_row_at(struct guibux_server *server, struct wlr_output *output,
		int box_w, int box_h, int row_h, int nrows, double lx, double ly) {
	int px, py;
	if (row_h <= 0 ||
			!panel_centred_box_at(server, output, box_w, box_h, lx, ly,
				&px, &py)) {
		return -1;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int idx = (int)((ly - box.y - py) / row_h);
	if (idx < 0 || idx >= nrows) {
		return -1;
	}
	return idx;
}
