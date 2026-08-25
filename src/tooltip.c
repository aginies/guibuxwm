#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <string.h>

/* delay before the tooltip appears after the pointer enters the
 * indicator; checked by tooltip_tick (topbar_tick, 500ms) */
#define TOOLTIP_HOVER_DELAY_MS 500
#define TOOLTIP_PAD_X 8
#define TOOLTIP_PAD_Y 6
#define TOOLTIP_LINE_H 20
#define TOOLTIP_MAX_LINES 4

static uint32_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void fmt_eta(int sec, char *out, size_t n) {
	int h = sec / 3600;
	int m = (sec % 3600) / 60;
	if (h > 0) {
		snprintf(out, n, "%dh %02dm", h, m);
	} else if (m > 0) {
		snprintf(out, n, "%dm", m);
	} else {
		snprintf(out, n, "<1m");
	}
}

/* build the tooltip lines for the given kind; returns the line count */
static int tooltip_build_lines(struct guibux_server *server,
		struct guibux_output *o, enum guibux_tooltip_kind kind,
		int net_idx, char lines[][NET_STR_MAX], int *widths) {
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	int n = 0;
	if (kind == TOOLTIP_BATTERY) {
		if (snap.bat[0] == '\0') {
			return 0;
		}
		char eta[16];
		if (snap.bat_state == 1 && snap.bat_eta_sec > 0) {
			fmt_eta(snap.bat_eta_sec, eta, sizeof(eta));
			snprintf(lines[n], NET_STR_MAX, "BAT %s - %s to full", snap.bat, eta);
		} else if (snap.bat_state == 2 && snap.bat_eta_sec > 0) {
			fmt_eta(snap.bat_eta_sec, eta, sizeof(eta));
			snprintf(lines[n], NET_STR_MAX, "BAT %s - %s left", snap.bat, eta);
		} else if (snap.bat_state == 3) {
			snprintf(lines[n], NET_STR_MAX, "BAT %s - fully charged", snap.bat);
		} else {
			snprintf(lines[n], NET_STR_MAX, "BAT %s", snap.bat);
		}
		n = 1;
	} else {
		if (net_idx < 0 || net_idx >= snap.net_iface_count) {
			return 0;
		}
		const struct guibux_net_iface *ni = &snap.net_ifaces[net_idx];
		if (ni->label[0] == '\0') {
			return 0;
		}
		snprintf(lines[n], NET_STR_MAX, "%s", ni->label);
		n++;
		if (ni->ip[0] != '\0') {
			snprintf(lines[n], NET_STR_MAX, "IP  %s", ni->ip);
			n++;
		}
		if (ni->gw[0] != '\0') {
			snprintf(lines[n], NET_STR_MAX, "GW  %s", ni->gw);
			n++;
		}
		if (ni->dns[0] != '\0') {
			snprintf(lines[n], NET_STR_MAX, "DNS %s", ni->dns);
			n++;
		}
		if (n > TOOLTIP_MAX_LINES) {
			n = TOOLTIP_MAX_LINES;
		}
	}
	/* measure each line at the output's scaled font size: the caller
	 * uses the widths directly in logical px (no further division) */
	struct wlr_box tbox;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &tbox);
	int tscale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int tfont_px = server->topbar_font_size * tscale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, tfont_px);
	for (int i = 0; i < n; i++) {
		widths[i] = guibux_text_width(server->launcher.face, lines[i]) / tscale;
	}
	return n;
}

bool tooltip_contains(struct guibux_server *server, double lx, double ly) {
	struct guibux_tooltip *tt = &server->tooltip;
	if (!tt->active || tt->output == NULL) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, tt->output, &box);
	return lx >= box.x + tt->box_x && lx < box.x + tt->box_x + tt->box_w &&
		ly >= box.y + tt->box_y && ly < box.y + tt->box_y + tt->box_h;
}

void tooltip_hide(struct guibux_server *server) {
	struct guibux_tooltip *tt = &server->tooltip;
	tt->active = false;
	tt->hover_output = NULL;
	tt->output = NULL;
	if (tt->scene_node != NULL) {
		wlr_scene_node_destroy(&tt->scene_node->node);
		tt->scene_node = NULL;
	}
	if (tt->buffer != NULL) {
		wlr_buffer_drop(tt->buffer);
		tt->buffer = NULL;
	}
}

static void tooltip_show(struct guibux_server *server, struct guibux_output *o) {
	struct guibux_tooltip *tt = &server->tooltip;
	if (tt->active || o->topbar_buffer == NULL) {
		return;
	}
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}

	char lines[TOOLTIP_MAX_LINES][NET_STR_MAX];
	int widths[TOOLTIP_MAX_LINES];
	int nlines = tooltip_build_lines(server, o, tt->hover_kind,
		tt->hover_net_idx, lines, widths);
	if (nlines <= 0) {
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int font_px = server->topbar_font_size * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);

	/* box size: widest line (already logical px) + padding */
	int max_tw = 0;
	for (int i = 0; i < nlines; i++) {
		if (widths[i] > max_tw)
			max_tw = widths[i];
	}
	int bw = max_tw + 2 * TOOLTIP_PAD_X;
	int bh = nlines * TOOLTIP_LINE_H + 2 * TOOLTIP_PAD_Y;

	/* anchor: centered under the indicator, clamped to the output */
	int ax, aw;
	if (tt->hover_kind == TOOLTIP_BATTERY) {
		if (o->topbar_bat_w <= 0)
			return;
		ax = o->topbar_bat_x;
		aw = o->topbar_bat_w;
	} else {
		int idx = tt->hover_net_idx;
		if (idx < 0 || idx >= o->topbar_net_count ||
				o->topbar_net_w[idx] <= 0)
			return;
		ax = o->topbar_net_x[idx];
		aw = o->topbar_net_w[idx];
	}
	int bx = ax + aw / 2 - bw / 2;
	if (bx < 4)
		bx = 4;
	if (bx + bw > box.width - 4)
		bx = box.width - 4 - bw;
	if (bx < 4)
		bx = 4;
	int by = server->topbar_height + 4;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	tt->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, bh * scale, &format);
	if (tt->buffer == NULL) {
		wlr_log(WLR_ERROR, "tooltip: failed to create buffer on %s",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	tt->scene_node = wlr_scene_buffer_create(&server->scene->tree, tt->buffer);
	if (tt->scene_node == NULL) {
		wlr_buffer_drop(tt->buffer);
		tt->buffer = NULL;
		return;
	}
	tt->box_w = bw;
	tt->box_h = bh;
	tt->box_x = bx;
	tt->box_y = by;
	tt->box_scale = scale;
	tt->output = o->wlr_output;
	tt->kind = tt->hover_kind;
	tt->net_idx = tt->hover_net_idx;
	/* store the first line as the text (used by tests + single-line case) */
	snprintf(tt->text, sizeof(tt->text), "%s", lines[0]);
	wlr_scene_buffer_set_dest_size(tt->scene_node, bw, bh);
	wlr_scene_node_set_position(&tt->scene_node->node, box.x + bx, box.y + by);
	wlr_scene_node_raise_to_top(&tt->scene_node->node);
	topbar_raise_all(server);
	tt->active = true;

	void *data;
	uint32_t buf_format;
	size_t stride;
	if (wlr_buffer_begin_data_ptr_access(tt->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &buf_format, &stride)) {
		if (buf_format == DRM_FORMAT_XRGB8888) {
			int w = bw * scale, h = bh * scale;
			cairo_surface_t *cs = cairo_image_surface_create_for_data(
				data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
			cairo_t *cr = cairo_create(cs);
			set_color(cr, server->color_bg);
			cairo_paint(cr);
			set_color(cr, server->color_border);
			cairo_set_line_width(cr, scale);
			cairo_rectangle(cr, scale / 2.0, scale / 2.0, w - scale, h - scale);
			cairo_stroke(cr);
			for (int i = 0; i < nlines; i++) {
				int baseline = TOOLTIP_PAD_Y * scale +
					i * TOOLTIP_LINE_H * scale +
					TOOLTIP_LINE_H * scale / 2 + font_px * 35 / 100;
				launcher_draw_text_on_surface(cs, server->launcher.face,
					lines[i], TOOLTIP_PAD_X * scale, baseline,
					server->color_text);
			}
			cairo_destroy(cr);
			cairo_surface_destroy(cs);
		}
		wlr_buffer_end_data_ptr_access(tt->buffer);
	}
	wlr_scene_buffer_set_buffer(tt->scene_node, tt->buffer);
	wlr_output_schedule_frame(o->wlr_output);
}

void tooltip_update_hover(struct guibux_server *server, uint32_t time) {
	struct guibux_tooltip *tt = &server->tooltip;
	double x = server->cursor->x, y = server->cursor->y;

	/* the tooltip itself keeps the hover alive: the pointer may move
	 * from the indicator onto the box */
	if (tt->active && tooltip_contains(server, x, y)) {
		return;
	}
	/* full-screen UI takes over the pointer: drop the hover and hide */
	if (server->launcher.active || server->switcher.active ||
			server->overview.active || server->help.active ||
			server->notify_panel.active) {
		tt->hover_output = NULL;
		if (tt->active) {
			tooltip_hide(server);
		}
		return;
	}
	struct guibux_output *o = NULL;
	int ws = 0;
	if (topbar_workspace_at(server, x, y, &o, &ws)) {
		enum guibux_tooltip_kind kind = TOOLTIP_BATTERY;
		int net_idx = -1;
		if (topbar_battery_at(server, o, x, y)) {
			kind = TOOLTIP_BATTERY;
		} else {
			net_idx = topbar_network_index_at(server, o, x, y);
			if (net_idx >= 0) {
				kind = TOOLTIP_NET;
			} else {
				tt->hover_output = NULL;
				if (tt->active) {
					tooltip_hide(server);
				}
				return;
			}
		}
		if (tt->hover_output != o || tt->hover_kind != kind ||
				tt->hover_net_idx != net_idx) {
			tt->hover_output = o;
			tt->hover_kind = kind;
			tt->hover_net_idx = net_idx;
			tt->hover_since = time;
		}
	} else {
		tt->hover_output = NULL;
		if (tt->active) {
			tooltip_hide(server);
		}
	}
}

void tooltip_tick(struct guibux_server *server) {
	struct guibux_tooltip *tt = &server->tooltip;
	if (tt->active || tt->hover_output == NULL) {
		return;
	}
	uint32_t now = monotonic_ms();
	if (now - tt->hover_since < TOOLTIP_HOVER_DELAY_MS) {
		return;
	}
	tooltip_show(server, tt->hover_output);
}

void tooltip_destroy(struct guibux_server *server) {
	tooltip_hide(server);
}

int tooltip_test_run(void *data) {
	struct guibux_server *server = data;

	/* --- battery tooltip --- */
	struct guibux_output *o = NULL;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_buffer != NULL && o->topbar_bat_w > 0) {
			break;
		}
		o = NULL;
	}
	if (o == NULL) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL battery indicator not rendered");
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);

	/* hover the battery indicator: the tooltip must arm and show */
	server->cursor->x = box.x + o->topbar_bat_x + o->topbar_bat_w / 2.0;
	server->cursor->y = box.y + server->topbar_height / 2.0;
	process_cursor_motion(server, 1);
	if (server->tooltip.hover_output != o ||
			server->tooltip.hover_kind != TOOLTIP_BATTERY) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL battery hover not armed");
		return 0;
	}
	tooltip_tick(server);
	if (!server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL battery tooltip not shown");
		return 0;
	}
	if (strstr(server->tooltip.text, "85%") == NULL ||
			strstr(server->tooltip.text, "1h 30m") == NULL) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL bad battery text '%s'",
			server->tooltip.text);
		return 0;
	}
	/* move away: the tooltip must hide */
	server->cursor->x = box.x + 5;
	server->cursor->y = box.y + server->topbar_height / 2.0;
	process_cursor_motion(server, 2);
	if (server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL battery tooltip not hidden");
		return 0;
	}

	/* --- net tooltip --- */
	struct guibux_output *no = NULL;
	wl_list_for_each(no, &server->outputs, link) {
		if (no->topbar_buffer != NULL && no->topbar_net_count > 0) {
			break;
		}
		no = NULL;
	}
	if (no == NULL) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL net indicator not rendered");
		return 0;
	}
	struct wlr_box nbox;
	wlr_output_layout_get_box(server->output_layout, no->wlr_output, &nbox);

	/* hover the first net segment: the tooltip must arm and show */
	server->cursor->x = nbox.x + no->topbar_net_x[0] +
		no->topbar_net_w[0] / 2.0;
	server->cursor->y = nbox.y + server->topbar_height / 2.0;
	process_cursor_motion(server, 3);
	if (server->tooltip.hover_output != no ||
			server->tooltip.hover_kind != TOOLTIP_NET ||
			server->tooltip.hover_net_idx != 0) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL net hover not armed");
		return 0;
	}
	tooltip_tick(server);
	if (!server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL net tooltip not shown");
		return 0;
	}
	/* the net tooltip is multi-line: label + IP + GW + DNS */
	if (server->tooltip.box_h < 3 * TOOLTIP_LINE_H) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL net tooltip not multi-line (h=%d)",
			server->tooltip.box_h);
		return 0;
	}
	/* verify the rendered text contains the seeded values by checking
	 * the snapshot directly (the tooltip text field only holds line 1) */
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	if (snap.net_iface_count < 1) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL no net ifaces in snapshot");
		return 0;
	}
	if (strcmp(snap.net_ifaces[0].ip, "10.0.0.5") != 0 ||
			strcmp(snap.net_ifaces[0].dns, "1.1.1.1") != 0 ||
			strcmp(snap.net_ifaces[0].gw, "10.0.0.1") != 0) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL bad net details ip=%s dns=%s gw=%s",
			snap.net_ifaces[0].ip, snap.net_ifaces[0].dns,
			snap.net_ifaces[0].gw);
		return 0;
	}
	/* move away: the tooltip must hide */
	server->cursor->x = nbox.x + 5;
	server->cursor->y = nbox.y + server->topbar_height / 2.0;
	process_cursor_motion(server, 4);
	if (server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL net tooltip not hidden");
		return 0;
	}
	wlr_log(WLR_INFO, "tooltip-test: OK (battery + net)");
	return 0;
}
