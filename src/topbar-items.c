#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define ITEMS_LINE_H 28
#define ITEMS_PAD 16
#define ITEMS_BOX_W 280

/* the panel lists every known item in canonical order; the enabled set
 * is server->topbar_items (config `topbar_items`) */
static const char *item_names[TOPBAR_ITEMS_MAX] = {
	"network",
	"volume",
	"mic",
	"battery",
	"notifications",
	"clock",
};

static bool item_enabled(struct guibux_server *server, int id) {
	for (int i = 0; i < server->topbar_item_count; i++) {
		if (server->topbar_items[i] == id) {
			return true;
		}
	}
	return false;
}

static void topbar_items_panel_render(struct guibux_server *server) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	if (p->buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(p->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(p->buffer);
		return;
	}
	int w = p->box_w * p->box_scale;
	int hgt = p->box_h * p->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, hgt, (int)stride);
	cairo_t *cr = cairo_create(cs);

	/* translucent rounded background */
	cairo_set_source_rgba(cr,
		((server->color_bg >> 16) & 0xFF) / 255.0,
		((server->color_bg >> 8) & 0xFF) / 255.0,
		(server->color_bg & 0xFF) / 255.0, 0.88);
	topbar_rounded_rect(cr, 0, 0, w, hgt, 12 * p->box_scale);
	cairo_fill(cr);
	/* subtle border */
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
	cairo_set_line_width(cr, p->box_scale);
	topbar_rounded_rect(cr, p->box_scale / 2.0, p->box_scale / 2.0,
		w - p->box_scale, hgt - p->box_scale, 11 * p->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * p->box_scale;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	for (int i = 0; i < TOPBAR_ITEMS_MAX; i++) {
		int ly = i * ITEMS_LINE_H * p->box_scale;
		int lh = ITEMS_LINE_H * p->box_scale;
		if (i == p->selected) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, 0, ly, w, lh);
			cairo_fill(cr);
		}
		uint32_t tc = server->color_text;
		int mb = ly + lh / 2 + font_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, item_names[i],
			ITEMS_PAD * p->box_scale, mb, tc);
		/* right-aligned on/off state */
		bool on = item_enabled(server, i);
		const char *state = on ? "on" : "off";
		uint32_t sc = on ? server->color_text : server->color_dim;
		int sw = guibux_text_width(face, state) / p->box_scale;
		launcher_draw_text_on_surface(cs, face, state,
			w - ITEMS_PAD * p->box_scale - sw * p->box_scale,
			mb, sc);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(p->buffer);
	if (p->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(p->scene_node, p->buffer);
	}
	if (p->output != NULL) {
		wlr_output_schedule_frame(p->output);
	}
}

void topbar_items_panel_show(struct guibux_server *server) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	if (p->active) {
		return;
	}
	tooltip_hide(server);
	osd_hide(server);

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = guibux_scale_round(output->scale);

	p->selected = 0;
	int bw = ITEMS_BOX_W;
	if (bw > ew - 20) {
		bw = ew - 20;
	}
	p->box_w = bw;
	p->box_h = TOPBAR_ITEMS_MAX * ITEMS_LINE_H;
	p->box_scale = scale;
	p->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	p->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, p->box_h * scale, &format);
	if (p->buffer == NULL) {
		return;
	}
	p->scene_node = wlr_scene_buffer_create(&server->scene->tree, p->buffer);
	wlr_scene_buffer_set_dest_size(p->scene_node, bw, p->box_h);
	wlr_scene_node_set_position(&p->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - p->box_h) / 2);
	wlr_scene_node_raise_to_top(&p->scene_node->node);
	topbar_raise_all(server);

	p->active = true;
	topbar_items_panel_render(server);
}

void topbar_items_panel_hide(struct guibux_server *server) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	if (!p->active) {
		return;
	}
	p->active = false;
	p->selected = 0;
	p->output = NULL;
	if (p->scene_node != NULL) {
		wlr_scene_node_destroy(&p->scene_node->node);
		p->scene_node = NULL;
	}
	if (p->buffer != NULL) {
		wlr_buffer_drop(p->buffer);
		p->buffer = NULL;
	}
}

void topbar_items_panel_toggle(struct guibux_server *server, int idx) {
	if (idx < 0 || idx >= TOPBAR_ITEMS_MAX) {
		return;
	}
	int id = idx;
	if (item_enabled(server, id)) {
		/* remove: shift the rest down */
		for (int i = 0; i < server->topbar_item_count; i++) {
			if (server->topbar_items[i] == id) {
				memmove(&server->topbar_items[i], &server->topbar_items[i + 1],
					(server->topbar_item_count - i - 1) *
					sizeof(server->topbar_items[0]));
				server->topbar_item_count--;
				break;
			}
		}
	} else {
		/* re-enable: insert at the canonical position so the default
		 * layout order is restored */
		if (server->topbar_item_count >= TOPBAR_ITEMS_MAX) {
			return;
		}
		int pos = id;
		if (pos > server->topbar_item_count) {
			pos = server->topbar_item_count;
		}
		memmove(&server->topbar_items[pos + 1], &server->topbar_items[pos],
			(server->topbar_item_count - pos) *
			sizeof(server->topbar_items[0]));
		server->topbar_items[pos] = id;
		server->topbar_item_count++;
	}
	/* live: re-render every topbar immediately */
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		topbar_mark_dirty(o);
		topbar_render(o);
	}
	topbar_items_panel_render(server);
}

static void topbar_items_panel_move(struct guibux_server *server, int dir) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	p->selected = (p->selected + dir + TOPBAR_ITEMS_MAX) % TOPBAR_ITEMS_MAX;
	topbar_items_panel_render(server);
}

bool topbar_items_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	if (!p->active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		topbar_items_panel_hide(server);
		return true;
	case XKB_KEY_Up:
		topbar_items_panel_move(server, -1);
		return true;
	case XKB_KEY_Down:
		topbar_items_panel_move(server, 1);
		return true;
	case XKB_KEY_d:
		topbar_items_panel_toggle(server, p->selected);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		topbar_items_panel_toggle(server, p->selected);
		return true;
	default:
		/* unhandled keys (e.g. Mod+l to toggle the panel) fall
		 * through to normal keybind dispatch */
		return false;
	}
}

int topbar_items_panel_row_at(struct guibux_server *server, double lx, double ly) {
	struct guibux_topbar_items_panel *p = &server->topbar_items_panel;
	if (!p->active || p->output == NULL) {
		return -1;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, p->output, &box);
	int ew, eh;
	wlr_output_effective_resolution(p->output, &ew, &eh);
	int px = (ew - p->box_w) / 2;
	int py = (eh - p->box_h) / 2;
	double x = lx - box.x;
	double y = ly - box.y;
	if (x < px || x >= px + p->box_w || y < py || y >= py + p->box_h) {
		return -1;
	}
	int idx = (int)((y - py) / ITEMS_LINE_H);
	if (idx < 0 || idx >= TOPBAR_ITEMS_MAX) {
		return -1;
	}
	return idx;
}

void topbar_items_panel_destroy(struct guibux_server *server) {
	topbar_items_panel_hide(server);
}

int topbar_items_panel_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL no output at cursor");
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);

	topbar_items_panel_show(server);
	if (!server->topbar_items_panel.active) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL panel not shown");
		return 0;
	}
	if (server->topbar_items_panel.buffer == NULL) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL no buffer");
		return 0;
	}
	if (server->topbar_items_panel.box_h != TOPBAR_ITEMS_MAX * ITEMS_LINE_H) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL bad height %d",
			server->topbar_items_panel.box_h);
		return 0;
	}
	/* click the middle of the first row: must resolve to network (0) */
	int px = (ew - server->topbar_items_panel.box_w) / 2;
	int py = (eh - server->topbar_items_panel.box_h) / 2;
	int idx = topbar_items_panel_row_at(server, box.x + px + 10,
		box.y + py + ITEMS_LINE_H / 2.0);
	if (idx != TOPBAR_ITEM_NETWORK) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL row hit (got %d, want %d)",
			idx, TOPBAR_ITEM_NETWORK);
		return 0;
	}
	/* a point below the panel must miss */
	idx = topbar_items_panel_row_at(server, box.x + px + 10,
		box.y + py + server->topbar_items_panel.box_h + 50);
	if (idx != -1) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL out-of-bounds hit %d", idx);
		return 0;
	}
	/* toggle network off: it must leave the enabled set */
	bool was_on = item_enabled(server, TOPBAR_ITEM_NETWORK);
	topbar_items_panel_toggle(server, TOPBAR_ITEM_NETWORK);
	if (item_enabled(server, TOPBAR_ITEM_NETWORK) == was_on) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL toggle did not change state");
		return 0;
	}
	/* toggle it back on: it must return to the enabled set */
	topbar_items_panel_toggle(server, TOPBAR_ITEM_NETWORK);
	if (item_enabled(server, TOPBAR_ITEM_NETWORK) != was_on) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL re-toggle did not restore state");
		return 0;
	}
	/* Esc must close the panel */
	topbar_items_panel_handle_key(server, XKB_KEY_Escape);
	if (server->topbar_items_panel.active) {
		wlr_log(WLR_ERROR, "topbar-items-test: FAIL panel not hidden on Esc");
		return 0;
	}
	wlr_log(WLR_INFO, "topbar-items-test: OK (%d items, box %dx%d)",
		TOPBAR_ITEMS_MAX, server->topbar_items_panel.box_w,
		server->topbar_items_panel.box_h);
	return 0;
}
