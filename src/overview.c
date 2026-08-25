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

	/* label background: workspace color pill if enabled, else dark */
	uint32_t bg = 0x1e1e2e;
	if (ov->ws_colors_enabled) {
		struct guibux_toplevel *t = ov->wins[idx];
		if (t && t->workspace >= 1 && t->workspace <= NUM_WORKSPACES &&
				ov->ws_colors[t->workspace - 1] != 0) {
			bg = ov->ws_colors[t->workspace - 1];
		}
	}
	set_color(cr, bg);
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

#define OVERVIEW_WS_COL_FONT 24

/*
 * Workspace column: a vertical strip on the left edge of each output's
 * overview area, one cell per workspace row (A1..A4). It makes empty
 * workspaces visible and, while a window is being dragged, highlights
 * the cell the window will be dropped on.
 */
static void overview_render_ws_col(struct guibux_output *o, int hover_ws) {
	struct guibux_server *server = o->server;
	if (!o->overview_ws_col_buf || !server->launcher.face) return;

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->overview_ws_col_buf,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) return;
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(o->overview_ws_col_buf);
		return;
	}

	int sc = guibux_scale_round(o->wlr_output->scale);
	/* logical height fixed at creation: the buffer must not be drawn
	 * beyond its size if the output is resized while the overview is up */
	int area_h = o->overview_ws_col_h;
	if (area_h < 0) area_h = 0;

	int w = OVERVIEW_WS_COL_W * sc;
	int h = area_h * sc;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, 0x1e1e2e);
	cairo_paint(cr);

	FT_Face face = server->launcher.face;
	int font_px = OVERVIEW_WS_COL_FONT * sc;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	int row_h = area_h / NUM_WORKSPACES;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		int y = (ws - 1) * row_h * sc;
		int ch = row_h * sc;
		if (ch <= 0) {
			continue;
		}
		if (ws == hover_ws) {
			/* drop target while dragging: workspace color */
			uint32_t bg = server->overview.ws_colors[ws - 1];
			if (bg == 0) {
				bg = server->color_highlight;
			}
			set_color(cr, bg);
			cairo_rectangle(cr, 0, y, w, ch);
			cairo_fill(cr);
		} else if (ws == o->current_workspace) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, 0, y, w, ch);
			cairo_fill(cr);
		}
		char label[8];
		snprintf(label, sizeof(label), "%c%d",
			'A' + (o->topbar_number - 1), ws);
		int tw = guibux_text_width(face, label);
		int x = (w - tw) / 2;
		if (x < 0) x = 0;
		int mb = y + ch / 2 + font_px * 35 / 100;
		uint32_t tc = (ws == hover_ws) ? 0xffffff : server->color_text;
		launcher_draw_text_on_surface(cs, face, label, x, mb, tc);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->overview_ws_col_buf);

	if (o->overview_ws_col_node) {
		wlr_scene_buffer_set_buffer(o->overview_ws_col_node,
			o->overview_ws_col_buf);
	}
}

static void overview_create_ws_cols(struct guibux_server *server) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &box);
		if (box.width <= 0 || box.height <= 0) {
			continue;
		}
		int area_h = box.height - server->topbar_height;
		if (area_h <= 0) {
			continue;
		}
		int sc = guibux_scale_round(o->wlr_output->scale);

		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format fmt = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		o->overview_ws_col_buf = wlr_allocator_create_buffer(
			server->launcher.shm_alloc,
			OVERVIEW_WS_COL_W * sc, area_h * sc, &fmt);
		if (!o->overview_ws_col_buf) {
			continue;
		}
		o->overview_ws_col_node = wlr_scene_buffer_create(
			&server->scene->tree, o->overview_ws_col_buf);
		if (!o->overview_ws_col_node) {
			wlr_buffer_drop(o->overview_ws_col_buf);
			o->overview_ws_col_buf = NULL;
			continue;
		}
		wlr_scene_buffer_set_dest_size(o->overview_ws_col_node,
			OVERVIEW_WS_COL_W, area_h);
		wlr_scene_node_set_position(&o->overview_ws_col_node->node,
			box.x, box.y + server->topbar_height);
		o->overview_ws_col_h = area_h;
		overview_render_ws_col(o, 0);
		wlr_scene_node_raise_to_top(&o->overview_ws_col_node->node);
	}
}

static void overview_destroy_ws_cols(struct guibux_server *server) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->overview_ws_col_node) {
			wlr_scene_node_destroy(&o->overview_ws_col_node->node);
			o->overview_ws_col_node = NULL;
		}
		if (o->overview_ws_col_buf) {
			wlr_buffer_drop(o->overview_ws_col_buf);
			o->overview_ws_col_buf = NULL;
		}
	}
}

/* re-render every column with the current hover state (0 = none) */
static void overview_refresh_ws_cols(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->overview_ws_col_node) {
			overview_render_ws_col(o,
				o->wlr_output == ov->hover_output ? ov->hover_ws : 0);
		}
	}
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
	int area_w = box.width - OVERVIEW_WS_COL_W;
	if (area_w <= 0) {
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
	*cell_w = area_w / n;
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
		int area_w = box.width - OVERVIEW_WS_COL_W;
		if (area_h <= 0 || area_w <= 0) {
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
			int cell_w = area_w / n;
			if (cell_w <= 0) {
				cell_w = 1;
			}
			int x = box.x + OVERVIEW_WS_COL_W;
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
	tooltip_hide(server);
	osd_hide(server);
	power_panel_hide(server);

	/* settle in-flight animations: the saved geometry below must be the
	 * final one, not a mid-animation position */
	effects_flush(server);

	ov->num_wins = 0;
	ov->hover_output = NULL;
	ov->hover_ws = 0;
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
	/* workspace column above the dim, below the topbar (needs a font:
	 * the buffer is drawn once, an undrawn buffer would show garbage) */
	if (server->launcher.face) {
		overview_create_ws_cols(server);
		overview_create_overlays(server);
	}
	topbar_raise_all(server);

	ov->active = true;
}

void overview_hide(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return;
	}
	ov->active = false;

	/* cancel an in-progress drag (Esc/F12, a window mapping/unmapping) */
	ov->drag_toplevel = NULL;
	ov->drag_active = false;
	reset_cursor_mode(server);

	overview_destroy_overlays(server);
	overview_destroy_ws_cols(server);

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
		if (o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
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

struct guibux_toplevel *overview_window_at(struct guibux_server *server,
		double lx, double ly) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return NULL;
	}

	/* first, try to hit a window cell */
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
	return t;
}

void overview_click_empty(struct guibux_server *server, double lx, double ly) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
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

static int overview_win_index(struct guibux_overview *ov,
		struct guibux_toplevel *t) {
	for (int i = 0; i < ov->num_wins; i++) {
		if (ov->wins[i] == t) {
			return i;
		}
	}
	return -1;
}

/*
 * Drag & drop: the cursor position picks the target. The output under the
 * cursor selects the monitor, the row under it the workspace. The window
 * is re-laid out into its new cell; the overview stays open so more
 * windows can be moved.
 */
static void overview_drop(struct guibux_server *server,
		struct guibux_toplevel *t) {
	struct guibux_overview *ov = &server->overview;
	int idx = overview_win_index(ov, t);
	if (idx < 0) {
		return;
	}

	struct wlr_output *target = output_at_cursor(server);
	int ws = t->workspace;
	if (target != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, target, &box);
		int area_y = box.y + server->topbar_height;
		int area_h = box.height - server->topbar_height;
		int row_h = area_h / NUM_WORKSPACES;
		if (row_h > 0 && server->cursor->y >= area_y &&
				server->cursor->y < box.y + box.height) {
			ws = (int)((server->cursor->y - area_y) / row_h) + 1;
			if (ws < 1) {
				ws = 1;
			}
			if (ws > NUM_WORKSPACES) {
				ws = NUM_WORKSPACES;
			}
		}
	}

	struct wlr_output *src = toplevel_output_for(t);
	if (target != NULL && target != src) {
		/* cross-monitor drop: center the window on the target
		 * monitor; remember it so the geometry is restored there
		 * when the overview is hidden */
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, target, &box);
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		int w = geo.width > 0 ? geo.width : 800;
		int h = geo.height > 0 ? geo.height : 600;
		double nx = box.x + (box.width - w) / 2;
		double ny = box.y + (box.height - h) / 2;
		wlr_scene_node_set_position(&t->scene_tree->node, nx, ny);
		ov->saved_x[idx] = nx;
		ov->saved_y[idx] = ny;
		ov->win_output[idx] = target;
		t->output = guibux_output_for(server, target);
		/* the window list changed on both bars */
		topbar_mark_dirty(guibux_output_for(server, src));
		topbar_mark_dirty(guibux_output_for(server, target));
	}
	t->workspace = ws;

	/* drag is over: clear the drop-target highlight */
	ov->hover_output = NULL;
	ov->hover_ws = 0;

	/* snap the window into its new cell and refresh the labels
	 * (monitor letter + workspace number) */
	overview_layout(server);
	overview_destroy_overlays(server);
	if (server->launcher.face) {
		overview_create_overlays(server);
	}
	overview_refresh_ws_cols(server);
}

void overview_button_release(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	struct guibux_toplevel *t = ov->drag_toplevel;
	ov->drag_toplevel = NULL;
	if (t == NULL) {
		reset_cursor_mode(server);
		return;
	}
	if (ov->drag_active) {
		ov->drag_active = false;
		overview_drop(server, t);
	} else {
		/* no movement: a plain click selects the window */
		overview_hide(server);
		struct guibux_output *o = guibux_output_for(server,
			toplevel_output_for(t));
		if (o != NULL && t->workspace != o->current_workspace) {
			switch_workspace(o, t->workspace);
		}
		focus_toplevel(t, true);
	}
	reset_cursor_mode(server);
}

/*
 * While a window is dragged, the column cell of the (output, workspace)
 * under the cursor is highlighted: it is where the window will be
 * dropped. Re-renders only when the target changes.
 */
void overview_update_hover(struct guibux_server *server) {
	struct guibux_overview *ov = &server->overview;
	if (!ov->active) {
		return;
	}

	struct wlr_output *target = NULL;
	int ws = 0;
	if (ov->drag_active) {
		target = output_at_cursor(server);
		if (target != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, target, &box);
			int area_y = box.y + server->topbar_height;
			int area_h = box.height - server->topbar_height;
			int row_h = area_h / NUM_WORKSPACES;
			if (row_h > 0 && server->cursor->y >= area_y &&
					server->cursor->y < box.y + box.height) {
				ws = (int)((server->cursor->y - area_y) / row_h) + 1;
				if (ws < 1) {
					ws = 1;
				}
				if (ws > NUM_WORKSPACES) {
					ws = NUM_WORKSPACES;
				}
			}
		}
	}

	if (target == ov->hover_output && ws == ov->hover_ws) {
		return;
	}
	struct guibux_output *old = guibux_output_for(server, ov->hover_output);
	if (old && old->overview_ws_col_node) {
		overview_render_ws_col(old, 0);
	}
	ov->hover_output = target;
	ov->hover_ws = ws;
	struct guibux_output *o = guibux_output_for(server, target);
	if (o && o->overview_ws_col_node && ws != 0) {
		overview_render_ws_col(o, ws);
	}
}
