#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SWITCHER_LINE_H 28
#define SWITCHER_PAD 12
#define SWITCHER_MAX_LINES 16

static uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void switcher_warp_to_selection(struct guibux_server *server) {
	struct guibux_switcher *s = &server->switcher;
	if (s->selection >= s->num_wins) return;
	struct guibux_toplevel *t = s->wins[s->selection];
	struct wlr_box geo;
	toplevel_get_geometry(t, &geo);
	double cx = t->scene_tree->node.x + geo.width / 2.0;
	double cy = t->scene_tree->node.y + geo.height / 2.0;
	wlr_cursor_warp(server->cursor, NULL, cx, cy);
	process_cursor_motion(server, now_msec());
}

static void switcher_render(struct guibux_server *server) {
	struct guibux_switcher *s = &server->switcher;
	if (s->buffer == NULL || server->launcher.face == NULL) {
		return;
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(s->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(s->buffer);
		return;
	}

	int w = s->box_w * s->box_scale;
	int h = s->box_h * s->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, s->box_scale);
	cairo_rectangle(cr, s->box_scale / 2.0, s->box_scale / 2.0,
		w - s->box_scale, h - s->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * s->box_scale;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	int count = s->num_wins < SWITCHER_MAX_LINES ? s->num_wins : SWITCHER_MAX_LINES;
	for (int i = 0; i < count; i++) {
		int ly = i * SWITCHER_LINE_H * s->box_scale;
		int lh = SWITCHER_LINE_H * s->box_scale;
		if (i == s->selection) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, s->box_scale, ly, w - 2 * s->box_scale, lh);
			cairo_fill(cr);
		}

		struct guibux_toplevel *t = s->wins[i];
		struct wlr_output *toutput = toplevel_output_for(t);
		struct guibux_output *o = guibux_output_for(server, toutput);
		char monletter = 'A';
		if (o != NULL) {
			monletter = 'A' + (o->topbar_number - 1);
		}
		const char *title = toplevel_get_title(t)
			? toplevel_get_title(t) : "(untitled)";
		char label[256];
		snprintf(label, sizeof(label), "%c%d: %s", monletter, t->workspace, title);

		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (i == s->selection) ? server->color_text :
			server->color_dim;
		launcher_draw_text_on_surface(cs, face, label,
			SWITCHER_PAD * s->box_scale, mb, mc);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(s->buffer);
	if (s->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(s->scene_node, s->buffer);
	}
	if (s->output != NULL) {
		wlr_output_schedule_frame(s->output);
	}
}

void switcher_show(struct guibux_server *server) {
	struct guibux_switcher *s = &server->switcher;
	if (s->active) {
		return;
	}

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = output->scale > 1 ? (int)output->scale : 1;

	s->num_wins = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (s->num_wins >= MAX_WINDOWS) break;
		s->wins[s->num_wins++] = t;
	}
	if (s->num_wins == 0) {
		return;
	}

	s->selection = 0;
	int lines = s->num_wins < SWITCHER_MAX_LINES ? s->num_wins : SWITCHER_MAX_LINES;
	int bw = 520;
	if (bw > ew - 20) bw = ew - 20;
	s->box_w = bw;
	s->box_h = lines * SWITCHER_LINE_H;
	s->box_scale = scale;
	s->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	s->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, s->box_h * scale, &format);
	if (s->buffer == NULL) {
		return;
	}
	s->scene_node = wlr_scene_buffer_create(&server->scene->tree, s->buffer);
	wlr_scene_node_set_position(&s->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - s->box_h) / 2);

	s->active = true;
	switcher_render(server);
	switcher_warp_to_selection(server);
}

void switcher_hide(struct guibux_server *server) {
	struct guibux_switcher *s = &server->switcher;
	if (!s->active) {
		return;
	}
	s->active = false;
	s->num_wins = 0;
	s->output = NULL;
	if (s->scene_node != NULL) {
		wlr_scene_node_destroy(&s->scene_node->node);
		s->scene_node = NULL;
	}
	if (s->buffer != NULL) {
		wlr_buffer_drop(s->buffer);
		s->buffer = NULL;
	}
	process_cursor_motion(server, now_msec());
}

static void switcher_select(struct guibux_server *server) {
	struct guibux_switcher *s = &server->switcher;
	if (s->selection >= s->num_wins) return;
	struct guibux_toplevel *t = s->wins[s->selection];

	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(t));
	if (o && t->workspace != o->current_workspace) {
		switch_workspace(o, t->workspace);
	}
	focus_toplevel(t);
	switcher_hide(server);
}

void switcher_on_modifier_release(struct guibux_server *server, uint32_t modifiers) {
	struct guibux_switcher *s = &server->switcher;
	if (!s->active) return;
	if (!(modifiers & WLR_MODIFIER_ALT)) {
		switcher_select(server);
	}
}

bool switcher_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_switcher *s = &server->switcher;
	if (!s->active) return false;

	switch (sym) {
	case XKB_KEY_Escape:
		switcher_hide(server);
		return true;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
		if (s->num_wins > 0) {
			s->selection = (s->selection + 1) % s->num_wins;
			switcher_render(server);
			switcher_warp_to_selection(server);
		}
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		switcher_select(server);
		return true;
	default:
		break;
	}
	return true;
}
