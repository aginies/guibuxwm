#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_buffer.h>

// width in px of text at the face's current pixel size
int guibux_text_width(FT_Face face, const char *text) {
	int w = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_RENDER) != 0) {
			continue;
		}
		w += face->glyph->advance.x / 64;
	}
	return w;
}

void set_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0,
		((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}

int launcher_draw_text_on_surface(cairo_surface_t *cs, FT_Face face,
		const char *text, int x, int baseline, uint32_t color) {
	uint32_t *data = (uint32_t *)cairo_image_surface_get_data(cs);
	int stride = cairo_image_surface_get_stride(cs) / 4;
	int sw = cairo_image_surface_get_width(cs);
	int sh = cairo_image_surface_get_height(cs);
	int cx = x;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_RENDER) != 0) {
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		for (uint32_t row = 0; row < g->bitmap.rows; row++) {
			int py = baseline - (int)g->bitmap_top + (int)row;
			if (py < 0 || py >= sh) {
				continue;
			}
			for (uint32_t col = 0; col < g->bitmap.width; col++) {
				int px = cx + (int)g->bitmap_left + (int)col;
				if (px < 0 || px >= sw) {
					continue;
				}
				uint8_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
				if (a == 0) {
					continue;
				}
				uint32_t *dst = &data[py * stride + px];
				uint8_t r = (color >> 16) & 0xFF;
				uint8_t gg = (color >> 8) & 0xFF;
				uint8_t b = color & 0xFF;
				uint8_t dr = (*dst >> 16) & 0xFF;
				uint8_t dg = (*dst >> 8) & 0xFF;
				uint8_t db = *dst & 0xFF;
				r = (r * a + dr * (255 - a)) / 255;
				gg = (gg * a + dg * (255 - a)) / 255;
				b = (b * a + db * (255 - a)) / 255;
				*dst = (r << 16) | (gg << 8) | b;
			}
		}
		cx += g->advance.x / 64;
	}
	return cx - x;
}

void topbar_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (o->topbar_buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->topbar_buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "topbar: cannot access buffer data");
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "topbar: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(o->topbar_buffer);
		return;
	}

	int ew, eh;
	wlr_output_effective_resolution(o->wlr_output, &ew, &eh);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int w = ew * scale;
	int h = TOPBAR_H * scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_topbar_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_rectangle(cr, 0, h - scale, w, scale);
	cairo_fill(cr);

	int font_px = TOPBAR_FONT_PX * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);
	int baseline = TOPBAR_H / 2 * scale + font_px * 35 / 100;

	char left[16];
	snprintf(left, sizeof(left), "%c", 'A' + (o->topbar_number - 1));
	launcher_draw_text_on_surface(cs, server->launcher.face, left,
		TOPBAR_PAD * scale, baseline, server->color_topbar_text);

	int cell_w = guibux_text_width(server->launcher.face, "9") / scale + 8;
	o->topbar_ws_cell_w = cell_w;
	int x = TOPBAR_PAD + guibux_text_width(server->launcher.face, left) / scale + 12;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		o->topbar_ws_x[ws] = x;
		char num[8];
		snprintf(num, sizeof(num), "%d", ws);
		if (ws == o->current_workspace) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, x * scale, (TOPBAR_H / 4) * scale,
				cell_w * scale, (TOPBAR_H / 2) * scale);
			cairo_fill(cr);
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_text);
		} else {
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_topbar_text);
		}
		x += cell_w;
	}

	/* vertical separator after workspaces */
	int sep_gap = 12;
	cairo_set_source_rgb(cr,
		((server->color_border >> 16) & 0xFF) / 255.0,
		((server->color_border >> 8) & 0xFF) / 255.0,
		(server->color_border & 0xFF) / 255.0);
	cairo_rectangle(cr, (int)((x + sep_gap / 2) * scale) - (int)(scale / 2.f),
		(TOPBAR_H / 4) * scale, 1 * scale,
		(TOPBAR_H / 2) * scale);
	cairo_fill(cr);

	/* render window titles between workspaces and date */
	int win_x = x + sep_gap;
	o->topbar_win_count = 0;
	struct guibux_toplevel *t;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *kb_focus = seat->keyboard_state.focused_surface;
	struct guibux_toplevel *kb_focus_t = NULL;
	if (kb_focus) {
		struct wlr_xdg_toplevel *kb_xdg =
			wlr_xdg_toplevel_try_from_wlr_surface(kb_focus);
		if (kb_xdg) {
			wl_list_for_each(t, &server->toplevels, link) {
				if (t->xdg_toplevel == kb_xdg) {
					kb_focus_t = t;
					break;
				}
			}
		}
	}

	struct guibux_toplevel *wins[TOPBAR_WIN_MAX];
	int nwins = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || !toplevel_visible(t))
			continue;
		if (toplevel_output_for(t) != o->wlr_output)
			continue;
		if (t->workspace != o->current_workspace)
			continue;
		if (nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}

	/* put focused first */
	for (int i = 0; i < nwins; i++) {
		if (wins[i] == kb_focus_t) {
			struct guibux_toplevel *f = wins[i];
			memmove(&wins[1], &wins[0], i * sizeof(*wins));
			wins[0] = f;
			break;
		}
	}

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(o->topbar_right, sizeof(o->topbar_right),
		"%a %d %b %Y  %H:%M", &tm);
	int date_w = guibux_text_width(server->launcher.face, o->topbar_right);
	int date_x = w / scale - TOPBAR_PAD - date_w;
	int win_end = date_x - sep_gap;
	if (win_end < win_x)
		win_end = win_x;

	/* vertical separator before date */
	if (win_x < date_x - sep_gap) {
		cairo_set_source_rgb(cr,
			((server->color_border >> 16) & 0xFF) / 255.0,
			((server->color_border >> 8) & 0xFF) / 255.0,
			(server->color_border & 0xFF) / 255.0);
		cairo_rectangle(cr, (int)((date_x - sep_gap / 2) * scale) - (int)(scale / 2.f),
			(TOPBAR_H / 4) * scale, 1 * scale,
			(TOPBAR_H / 2) * scale);
		cairo_fill(cr);
	}

	/* calculate max width per window to fit all in available space */
	int avail = win_end - win_x;
	int max_w = 30;
	if (nwins > 0) {
		int per = (avail - (nwins - 1) * TOPBAR_WIN_GAP) / nwins;
		if (per < 30)
			per = 30;
		max_w = per;
	}

	int rendered = 0;
	for (int i = 0; i < nwins && win_x < win_end; i++) {
		char *title = wins[i]->xdg_toplevel->title ?
			wins[i]->xdg_toplevel->title : "(untitled)";
		char buf[64];
		int tw = guibux_text_width(server->launcher.face, title);
		if (tw > max_w - 16) {
			snprintf(buf, sizeof(buf), "%.20s...", title);
			tw = guibux_text_width(server->launcher.face, buf);
			while (tw > max_w - 16 && strlen(buf) > 4) {
				buf[strlen(buf) - 4] = '\0';
				strcat(buf, "...");
				tw = guibux_text_width(server->launcher.face, buf);
			}
		} else {
			snprintf(buf, sizeof(buf), "%s", title);
		}
		int cell_w = tw + 16;
		if (cell_w > max_w)
			cell_w = max_w;
		strncpy(o->topbar_win_titles[i], buf, 63);
		o->topbar_win_titles[i][63] = '\0';
		o->topbar_win_x[i] = win_x;
		o->topbar_win_w[i] = cell_w;

		if (wins[i] == kb_focus_t) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, win_x * scale,
				(TOPBAR_H / 4) * scale,
				cell_w * scale,
				(TOPBAR_H / 2) * scale);
			cairo_fill(cr);
			launcher_draw_text_on_surface(cs,
				server->launcher.face, buf,
				(win_x + 8) * scale, baseline,
				server->color_text);
		} else {
			launcher_draw_text_on_surface(cs,
				server->launcher.face, buf,
				(win_x + 8) * scale, baseline,
				server->color_dim);
		}
		win_x += cell_w + TOPBAR_WIN_GAP;
		rendered++;
	}
	o->topbar_win_count = rendered;

	launcher_draw_text_on_surface(cs, server->launcher.face, o->topbar_right,
		date_x * scale,
		baseline, server->color_topbar_text);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->topbar_buffer);
	if (o->topbar_node != NULL) {
		wlr_scene_buffer_set_buffer(o->topbar_node, o->topbar_buffer);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

struct guibux_toplevel *topbar_win_at(struct guibux_output *o,
		double lx, double ly) {
	struct guibux_server *server = o->server;
	if (o->topbar_buffer == NULL)
		return NULL;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + TOPBAR_H)
		return NULL;
	double rel = lx - box.x;
	struct guibux_toplevel *wins[TOPBAR_WIN_MAX];
	int nwins = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || !toplevel_visible(t))
			continue;
		if (toplevel_output_for(t) != o->wlr_output)
			continue;
		if (nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}
	for (int i = 0; i < o->topbar_win_count && i < nwins; i++) {
		if (rel >= o->topbar_win_x[i] &&
				rel < o->topbar_win_x[i] + o->topbar_win_w[i])
			return wins[i];
	}
	return NULL;
}

bool topbar_workspace_at(struct guibux_server *server, double lx,
		double ly, struct guibux_output **output, int *ws) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_buffer == NULL) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		if (lx < box.x || lx >= box.x + box.width ||
				ly < box.y || ly >= box.y + TOPBAR_H) {
			continue;
		}
		if (output != NULL) {
			*output = o;
		}
		if (ws != NULL) {
			*ws = 0;
			double rel = lx - box.x;
			for (int i = 1; i <= NUM_WORKSPACES; i++) {
				if (rel >= o->topbar_ws_x[i] &&
						rel < o->topbar_ws_x[i] + o->topbar_ws_cell_w) {
					*ws = i;
					break;
				}
			}
		}
		return true;
	}
	return false;
}

void topbar_create(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (server->launcher.shm_alloc == NULL || server->launcher.face == NULL) {
		wlr_log(WLR_ERROR, "topbar: disabled on %s (no allocator or font)",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	int ew, eh;
	wlr_output_effective_resolution(o->wlr_output, &ew, &eh);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	o->topbar_buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		ew * scale, TOPBAR_H * scale, &format);
	if (o->topbar_buffer == NULL) {
		wlr_log(WLR_ERROR, "topbar: failed to create buffer on %s",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	o->topbar_node = wlr_scene_buffer_create(&server->scene->tree, o->topbar_buffer);
	wlr_scene_node_set_position(&o->topbar_node->node, box.x, box.y);
	topbar_render(o);
}

void topbar_destroy(struct guibux_output *o) {
	if (o->topbar_node != NULL) {
		wlr_scene_node_destroy(&o->topbar_node->node);
		o->topbar_node = NULL;
	}
	if (o->topbar_buffer != NULL) {
		wlr_buffer_drop(o->topbar_buffer);
		o->topbar_buffer = NULL;
	}
}

void topbar_renumber(struct guibux_server *server) {
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o != NULL && o->topbar_buffer != NULL) {
			o->topbar_number = i + 1;
			topbar_render(o);
		}
	}
}

int topbar_tick(void *data) {
	struct guibux_server *server = data;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		topbar_render(o);
	}
	wl_event_source_timer_update(server->topbar_timer, 1000);
	return 0;
}

int topbar_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL no buffer on output %d", i + 1);
			return 0;
		}
		if (o->topbar_number != i + 1) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL number (got %d, want %d)",
				o->topbar_number, i + 1);
			return 0;
		}
		if (strlen(o->topbar_right) < 10) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL time string '%s'",
				o->topbar_right);
			return 0;
		}
	}
	wlr_log(WLR_INFO, "topbar-test: OK (%d outputs)", n);
	return 0;
}
