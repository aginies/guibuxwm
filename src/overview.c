#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <drm_fourcc.h>
#include <string.h>

#define OVERVIEW_LABEL_FONT 60
#define OVERVIEW_LABEL_PAD 6

/*
 * GNOME-style overview: every output shows its 4 workspaces as rows
 * (workspace 1 on top), the windows of each workspace as equal-width
 * cells in that row. All windows stay live; geometry is restored on
 * hide. A uniform semi-transparent dark dim covers each whole output
 * (windows and labels included); a big A1/B2 label is centered on
 * every window cell.
 */

static void overview_render_label(struct guibux_server *server, int idx) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->label_buf[idx] || !server->launcher.face) return;

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(ov->label_buf[idx],
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) return;
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(ov->label_buf[idx]);
		return;
	}

	int sc = ov->label_scale[idx];
	int w = ov->label_w[idx] * sc;
	int h = ov->label_h[idx] * sc;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, 0x1e1e2e);
	cairo_paint(cr);

	FT_Face face = server->launcher.face;
	int font_px = OVERVIEW_LABEL_FONT * sc;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	int mb = h / 2 + font_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face, ov->label_text[idx],
		OVERVIEW_LABEL_PAD * sc, mb, 0xffffff);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(ov->label_buf[idx]);

	if (ov->label_node[idx]) {
		wlr_scene_buffer_set_buffer(ov->label_node[idx], ov->label_buf[idx]);
	}
}

static void overview_position_overlay(struct wlr_scene_node *node,
		int lw, int lh, int cw, int ch) {
	int x_off = (cw - lw) / 2;
	if (x_off < 0) x_off = 0;
	int y_off = (ch - lh) / 2;
	if (y_off < 0) y_off = 0;
	wlr_scene_node_set_position(node, x_off, y_off);
}

/*
 * Cell size for window i, computed the same way as overview_layout.
 * base->geometry is not usable here: wlr_xdg_toplevel_set_size only
 * schedules a configure, the client commits the new size later.
 */
static void overview_cell_size(struct guibux_server *server, int i,
		int *cell_w, int *row_h) {
	struct guibux_overview *ov = &server->overview;
	struct guibux_toplevel *t = ov->wins[i];
	struct guibux_output *o = guibux_output_for(server, ov->win_output[i]);
	*cell_w = 0;
	*row_h = 0;
	if (!o) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	int area_h = box.height - server->topbar_height;
	if (area_h <= 0) {
		return;
	}
	*row_h = area_h / NUM_WORKSPACES;
	if (*row_h <= 0) {
		return;
	}
	int n = 0;
	for (int j = 0; j < ov->num_wins; j++) {
		if (ov->wins[j]->workspace == t->workspace &&
				ov->win_output[j] == o->wlr_output) {
			n++;
		}
	}
	if (n <= 0) {
		n = 1;
	}
	*cell_w = box.width / n;
	if (*cell_w <= 0) {
		*cell_w = 1;
	}
}

static void overview_create_overlays(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;

	for (int i = 0; i < ov->num_wins; i++) {
		struct guibux_toplevel *t = ov->wins[i];

		int cw, ch;
		overview_cell_size(server, i, &cw, &ch);
		if (cw <= 0 || ch <= 0) continue;

		int sc = 1;
		struct wlr_output *wout = ov->win_output[i];
		if (wout && wout->scale > 1) sc = (int)wout->scale;

		/* label */
		const char *title = toplevel_get_title(t) ? toplevel_get_title(t) : "";
		FT_Face face = server->launcher.face;
		if (!face) continue;

		struct guibux_output *o = guibux_output_for(server, wout);
		char mon = o ? 'A' + (o->topbar_number - 1) : '?';
		snprintf(ov->label_text[i], sizeof(ov->label_text[i]),
			"%c%d: %s", mon, t->workspace, title);

		int font_px = OVERVIEW_LABEL_FONT;
		FT_Set_Pixel_Sizes(face, 0, font_px);

		/* truncate long titles so the label fits the cell (codepoint
		 * safe: byte-wise truncation would split multi-byte UTF-8) */
		int max_w = cw - 2 * OVERVIEW_LABEL_PAD;
		if (max_w < 30) max_w = 30;
		if (guibux_text_width(face, ov->label_text[i]) > max_w) {
			const char *p = ov->label_text[i];
			int cps = 0;
			while (*p) {
				utf8_next(&p);
				cps++;
			}
			char tmp[sizeof(ov->label_text[i])];
			for (int trunc = cps; trunc >= 0; trunc--) {
				utf8_truncate(ov->label_text[i], tmp, sizeof(tmp), trunc);
				size_t n = strlen(tmp);
				if (n + 4 > sizeof(tmp)) {
					n = sizeof(tmp) - 4;
					tmp[n] = '\0';
				}
				snprintf(tmp + n, sizeof(tmp) - n, "...");
				if (guibux_text_width(face, tmp) <= max_w) {
					snprintf(ov->label_text[i], sizeof(ov->label_text[i]),
						"%s", tmp);
					break;
				}
				if (trunc == 0) {
					snprintf(ov->label_text[i], sizeof(ov->label_text[i]),
						"...");
				}
			}
		}

		int tw = guibux_text_width(face, ov->label_text[i]);
		int lw = tw + 2 * OVERVIEW_LABEL_PAD;
		if (lw > cw) lw = cw;
		if (lw < 30) lw = 30;
		int lh = OVERVIEW_LABEL_FONT + 10;

		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format fmt = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		ov->label_buf[i] = wlr_allocator_create_buffer(
			server->launcher.shm_alloc, lw * sc, lh * sc, &fmt);
		if (!ov->label_buf[i]) continue;

		ov->label_w[i] = lw;
		ov->label_h[i] = lh;
		ov->label_scale[i] = sc;

		ov->label_node[i] = wlr_scene_buffer_create(t->scene_tree, ov->label_buf[i]);
		if (!ov->label_node[i]) {
			wlr_buffer_drop(ov->label_buf[i]);
			ov->label_buf[i] = NULL;
			continue;
		}
		wlr_scene_buffer_set_dest_size(ov->label_node[i], lw, lh);
		overview_render_label(server, i);
		overview_position_overlay(&ov->label_node[i]->node, lw, lh, cw, ch);
	}
}

static void overview_destroy_overlays(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;

	for (int i = 0; i < ov->num_wins; i++) {
		if (ov->label_node[i]) {
			wlr_scene_node_destroy(&ov->label_node[i]->node);
			ov->label_node[i] = NULL;
		}
		if (ov->label_buf[i]) {
			wlr_buffer_drop(ov->label_buf[i]);
			ov->label_buf[i] = NULL;
		}
	}
}

static void overview_layout(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;

	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		int area_y = box.y + server->topbar_height;
		int area_h = box.height - server->topbar_height;
		if (area_h <= 0) {
			continue;
		}
		int row_h = area_h / NUM_WORKSPACES;
		if (row_h <= 0) {
			continue;
		}
		for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
			int n = 0;
			for (int i = 0; i < ov->num_wins; i++) {
				if (ov->wins[i]->workspace == ws &&
						ov->win_output[i] == o->wlr_output) {
					n++;
				}
			}
			if (n == 0) {
				continue;
			}
			int cell_w = box.width / n;
			if (cell_w <= 0) {
				cell_w = 1;
			}
			int x = box.x;
			for (int i = 0; i < ov->num_wins; i++) {
				struct guibux_toplevel *t = ov->wins[i];
				if (t->workspace == ws &&
						ov->win_output[i] == o->wlr_output) {
					wlr_scene_node_set_position(&t->scene_tree->node,
						x, area_y + (ws - 1) * row_h);
					toplevel_set_size(t, cell_w, row_h);

					if (ov->label_node[i]) {
						int x_off = (cell_w - ov->label_w[i]) / 2;
						if (x_off < 0) x_off = 0;
						int y_off = (row_h - ov->label_h[i]) / 2;
						if (y_off < 0) y_off = 0;
						wlr_scene_node_set_position(&ov->label_node[i]->node,
							x_off, y_off);
					}

					x += cell_w;
				}
			}
		}
	}
}

void overview_show(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	if (ov->active) {
		return;
	}

	ov->num_wins = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (ov->num_wins >= MAX_WINDOWS) {
			break;
		}
		ov->wins[ov->num_wins] = t;
		/* output while the window is still at its normal position;
		 * after the layout below, the stale buffer size would shift
		 * the center point onto a neighboring output */
		ov->win_output[ov->num_wins] = toplevel_output_for(t);
		ov->saved_x[ov->num_wins] = t->scene_tree->node.x;
		ov->saved_y[ov->num_wins] = t->scene_tree->node.y;
		/* geometry (logical) size: wlr_xdg_toplevel_set_size takes
		 * logical pixels, the buffer size is scale-adjusted */
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		ov->saved_w[ov->num_wins] = geo.width;
		ov->saved_h[ov->num_wins] = geo.height;
		ov->label_node[ov->num_wins] = NULL;
		ov->label_buf[ov->num_wins] = NULL;
		ov->num_wins++;
	}
	if (ov->num_wins == 0) {
		return;
	}

	for (int i = 0; i < ov->num_wins; i++) {
		wlr_scene_node_set_enabled(&ov->wins[i]->scene_tree->node, true);
	}

	overview_layout(server);

	/* uniform dim over each whole output, above windows and labels */
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		float color[4] = { 0, 0, 0, 0.4f };
		o->overview_dim = wlr_scene_rect_create(&server->scene->tree,
			box.width, box.height, color);
		if (o->overview_dim) {
			wlr_scene_node_set_position(&o->overview_dim->node, box.x, box.y);
			wlr_scene_node_raise_to_top(&o->overview_dim->node);
		}
	}
	topbar_raise_all(server);

	ov->active = true;

	if (server->launcher.face) {
		overview_create_overlays(server);
	}
}

void overview_hide(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return;
	}
	ov->active = false;

	overview_destroy_overlays(server);

	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->overview_dim) {
			wlr_scene_node_destroy(&o->overview_dim->node);
			o->overview_dim = NULL;
		}
	}

	for (int i = 0; i < ov->num_wins; i++) {
		struct guibux_toplevel *t = ov->wins[i];
		wlr_scene_node_set_position(&t->scene_tree->node,
			ov->saved_x[i], ov->saved_y[i]);
		if (ov->saved_w[i] > 0 && ov->saved_h[i] > 0) {
			toplevel_set_size(t, ov->saved_w[i], ov->saved_h[i]);
		}
	}
	ov->num_wins = 0;

	wl_list_for_each(o, &server->outputs, link) {
		struct guibux_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (toplevel_output_for(t) == o->wlr_output) {
				wlr_scene_node_set_enabled(&t->scene_tree->node,
					t->workspace == o->current_workspace);
			}
		}
		if (o->tile_mode != GUIBUX_TILE_FREE) {
			retile_output(o);
		}
	}
}

bool overview_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
	case XKB_KEY_F12:
		overview_hide(server);
		return true;
	case XKB_KEY_1:
	case XKB_KEY_2:
	case XKB_KEY_3:
	case XKB_KEY_4: {
		int ws = sym - XKB_KEY_1 + 1;
		struct wlr_output *out = output_at_cursor(server);
		struct guibux_output *o = out != NULL
			? guibux_output_for(server, out) : NULL;
		if (o != NULL) {
			switch_workspace(o, ws);
		}
		overview_hide(server);
		return true;
	}
	default:
		return true;
	}
}

void overview_click(struct guibux_server *server, double lx, double ly) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct guibux_toplevel *t = desktop_toplevel_at(server, lx, ly,
		&surface, &sx, &sy);
	if (t == NULL) {
		/* the dim rect sits above the windows; hit-test cells directly
		 * (cell size, not base->geometry: the client may not have
		 * committed the overview size yet) */
		for (int i = 0; i < ov->num_wins; i++) {
			struct guibux_toplevel *w = ov->wins[i];
			int cw, ch;
			overview_cell_size(server, i, &cw, &ch);
			if (cw <= 0 || ch <= 0) {
				continue;
			}
			if (lx >= w->scene_tree->node.x &&
					lx < w->scene_tree->node.x + cw &&
					ly >= w->scene_tree->node.y &&
					ly < w->scene_tree->node.y + ch) {
				t = w;
				break;
			}
		}
	}
	if (t != NULL) {
		overview_hide(server);
		struct guibux_output *o = guibux_output_for(server,
			toplevel_output_for(t));
		if (o != NULL && t->workspace != o->current_workspace) {
			switch_workspace(o, t->workspace);
		}
		focus_toplevel(t);
		return;
	}

	/* empty area: switch to the workspace of the clicked row */
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &box);
		if (lx < box.x || lx >= box.x + box.width ||
				ly < box.y || ly >= box.y + box.height) {
			continue;
		}
		int area_y = box.y + server->topbar_height;
		int area_h = box.height - server->topbar_height;
		if (area_h <= 0 || ly < area_y) {
			continue;
		}
		int row_h = area_h / NUM_WORKSPACES;
		if (row_h <= 0) {
			continue;
		}
		int ws = (int)((ly - area_y) / row_h) + 1;
		if (ws < 1) {
			ws = 1;
		}
		if (ws > NUM_WORKSPACES) {
			ws = NUM_WORKSPACES;
		}
		overview_hide(server);
		switch_workspace(o, ws);
		return;
	}
	overview_hide(server);
}
