#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <string.h>

/* delay before the tooltip appears after the pointer enters the
 * battery indicator; checked by tooltip_tick (topbar_tick, 500ms) */
#define TOOLTIP_HOVER_DELAY_MS 500
#define TOOLTIP_PAD 8
#define TOOLTIP_H 24

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
	if (tt->active || o->topbar_buffer == NULL || o->topbar_bat_w <= 0) {
		return;
	}
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}

	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	if (snap.bat[0] == '\0') {
		return;
	}

	char text[128];
	char eta[16];
	if (snap.bat_state == 1 && snap.bat_eta_sec > 0) {
		fmt_eta(snap.bat_eta_sec, eta, sizeof(eta));
		snprintf(text, sizeof(text), "BAT %s - %s to full", snap.bat, eta);
	} else if (snap.bat_state == 2 && snap.bat_eta_sec > 0) {
		fmt_eta(snap.bat_eta_sec, eta, sizeof(eta));
		snprintf(text, sizeof(text), "BAT %s - %s left", snap.bat, eta);
	} else if (snap.bat_state == 3) {
		snprintf(text, sizeof(text), "BAT %s - fully charged", snap.bat);
	} else {
		snprintf(text, sizeof(text), "BAT %s", snap.bat);
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int font_px = server->topbar_font_size * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);
	int tw = guibux_text_width(server->launcher.face, text) / scale;
	int bw = tw + 2 * TOOLTIP_PAD;
	int bh = TOOLTIP_H;

	/* centered under the battery indicator, clamped to the output */
	int bx = o->topbar_bat_x + o->topbar_bat_w / 2 - bw / 2;
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
	snprintf(tt->text, sizeof(tt->text), "%s", text);
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
			int baseline = bh / 2 * scale + font_px * 35 / 100;
			launcher_draw_text_on_surface(cs, server->launcher.face, text,
				TOOLTIP_PAD * scale, baseline, server->color_text);
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
	if (topbar_workspace_at(server, x, y, &o, &ws) &&
			topbar_battery_at(server, o, x, y)) {
		if (tt->hover_output != o) {
			tt->hover_output = o;
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
	if (server->tooltip.hover_output != o) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL hover not armed");
		return 0;
	}
	tooltip_tick(server);
	if (!server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL tooltip not shown");
		return 0;
	}
	if (strstr(server->tooltip.text, "85%") == NULL ||
			strstr(server->tooltip.text, "1h 30m") == NULL) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL bad text '%s'",
			server->tooltip.text);
		return 0;
	}
	/* move away: the tooltip must hide */
	server->cursor->x = box.x + 5;
	server->cursor->y = box.y + server->topbar_height / 2.0;
	process_cursor_motion(server, 2);
	if (server->tooltip.active) {
		wlr_log(WLR_ERROR, "tooltip-test: FAIL tooltip not hidden");
		return 0;
	}
	wlr_log(WLR_INFO, "tooltip-test: OK ('%s')", server->tooltip.text);
	return 0;
}
