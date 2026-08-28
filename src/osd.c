#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <string.h>

#define OSD_PAD 12
#define OSD_MIN_W 220
#define OSD_SHOT_MIN_W 480
#define OSD_TEXT_H 24
#define OSD_WS_TEXT_H 96
#define OSD_BAR_H 8
#define OSD_TEXT_BAR_GAP 8

static uint32_t osd_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void osd_free_node(struct guibux_server *server) {
	struct guibux_osd *osd = &server->osd;
	if (osd->scene_node != NULL) {
		wlr_scene_node_destroy(&osd->scene_node->node);
		osd->scene_node = NULL;
	}
	if (osd->buffer != NULL) {
		wlr_buffer_drop(osd->buffer);
		osd->buffer = NULL;
	}
}

void osd_hide(struct guibux_server *server) {
	struct guibux_osd *osd = &server->osd;
	if (!osd->active) {
		return;
	}
	osd->active = false;
	osd->output = NULL;
	osd_free_node(server);
}

void osd_show(struct guibux_server *server, enum guibux_osd_kind kind,
		int value, bool muted) {
	if (!server->osd_enabled || server->osd_timeout_ms <= 0) {
		return;
	}
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}

	/* cancel any in-flight previous OSD */
	osd_hide(server);

	struct guibux_osd *osd = &server->osd;
	osd->kind = kind;
	osd->value = value;
	osd->muted = muted;
	osd->output = output;

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int scale = guibux_scale_round(output->scale);
	int font_scale = osd->font_scale > 0 ? osd->font_scale : 1;
	int font_px = server->topbar_font_size * scale * font_scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);

	char text[256];
	if (kind == OSD_SHOT || kind == OSD_WS) {
		snprintf(text, sizeof(text), "%s", osd->text);
	} else {
		const char *label;
		switch (kind) {
		case OSD_MIC:
			label = "MIC";
			break;
		case OSD_BRIGHTNESS:
			label = "BRI";
			break;
		default:
			label = "VOL";
			break;
		}
		if (muted) {
			snprintf(text, sizeof(text), "%s MUTE", label);
		} else {
			snprintf(text, sizeof(text), "%s %d%%", label, value);
		}
	}

	int tw = guibux_text_width(server->launcher.face, text) / scale;
	int bw = tw + 2 * OSD_PAD;
	int min_w = (kind == OSD_SHOT) ? OSD_SHOT_MIN_W : OSD_MIN_W;
	if (bw < min_w)
		bw = min_w;
	int text_h = (kind == OSD_WS) ? OSD_WS_TEXT_H : OSD_TEXT_H;
	int bh;
	if (kind == OSD_SHOT || kind == OSD_WS) {
		bh = 2 * OSD_PAD + text_h;
	} else {
		bh = OSD_PAD + OSD_TEXT_H + OSD_TEXT_BAR_GAP + OSD_BAR_H + OSD_PAD;
	}

	int bx = (box.width - bw) / 2;
	if (bx < 4)
		bx = 4;
	int by = (box.height - bh) / 2;
	if (by < 4)
		by = 4;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	osd->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, bh * scale, &format);
	if (osd->buffer == NULL) {
		wlr_log(WLR_ERROR, "osd: failed to create buffer on %s",
			output->name ? output->name : "(unknown)");
		return;
	}
	osd->scene_node = wlr_scene_buffer_create(&server->scene->tree, osd->buffer);
	if (osd->scene_node == NULL) {
		wlr_buffer_drop(osd->buffer);
		osd->buffer = NULL;
		return;
	}
	osd->box_w = bw;
	osd->box_h = bh;
	osd->box_x = bx;
	osd->box_y = by;
	osd->box_scale = scale;
	wlr_scene_buffer_set_dest_size(osd->scene_node, bw, bh);
	wlr_scene_node_set_position(&osd->scene_node->node, box.x + bx, box.y + by);
	wlr_scene_node_raise_to_top(&osd->scene_node->node);
	topbar_raise_all(server);
	osd->active = true;
	osd->hide_at_ms = osd_now_ms() + server->osd_timeout_ms;

	void *data;
	uint32_t buf_format;
	size_t stride;
	if (wlr_buffer_begin_data_ptr_access(osd->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &buf_format, &stride)) {
		if (buf_format == DRM_FORMAT_XRGB8888) {
			int w = bw * scale, h = bh * scale;
			cairo_surface_t *cs = cairo_image_surface_create_for_data(
				data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
			cairo_t *cr = cairo_create(cs);
			memset(data, 0, stride * h);
			/* translucent rounded background */
			cairo_set_source_rgba(cr,
				((server->color_bg >> 16) & 0xFF) / 255.0,
				((server->color_bg >> 8) & 0xFF) / 255.0,
				(server->color_bg & 0xFF) / 255.0, 0.85);
			topbar_rounded_rect(cr, 0, 0, w, h, 12 * scale);
			cairo_fill(cr);
			/* subtle border */
			cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
			cairo_set_line_width(cr, scale);
			topbar_rounded_rect(cr, scale / 2.0, scale / 2.0,
				w - scale, h - scale, 11 * scale);
			cairo_stroke(cr);
			int baseline = OSD_PAD * scale + text_h * scale / 2 +
				font_px * 35 / 100;
			launcher_draw_text_on_surface(cs, server->launcher.face, text,
				OSD_PAD * scale, baseline, server->color_text);
			if (kind != OSD_SHOT && kind != OSD_WS) {
				int bar_y = (OSD_PAD + OSD_TEXT_H + OSD_TEXT_BAR_GAP) * scale;
				int bar_w = (w - 2 * OSD_PAD * scale);
				cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
				topbar_rounded_rect(cr, OSD_PAD * scale, bar_y,
					bar_w, OSD_BAR_H * scale, (OSD_BAR_H / 2) * scale);
				cairo_fill(cr);
				int fill_w = muted ? 0 : (bar_w * value) / 100;
				if (fill_w > bar_w)
					fill_w = bar_w;
				if (fill_w < 0)
					fill_w = 0;
				if (fill_w > 0) {
					set_color(cr, server->color_accent);
					topbar_rounded_rect(cr, OSD_PAD * scale, bar_y,
						fill_w, OSD_BAR_H * scale, (OSD_BAR_H / 2) * scale);
					cairo_fill(cr);
				}
			}
			cairo_destroy(cr);
			cairo_surface_destroy(cs);
		}
		wlr_buffer_end_data_ptr_access(osd->buffer);
	}
	wlr_scene_buffer_set_buffer(osd->scene_node, osd->buffer);
	wlr_output_schedule_frame(output);
}

void osd_shot(struct guibux_server *server, const char *msg) {
	struct guibux_osd *osd = &server->osd;
	snprintf(osd->text, sizeof(osd->text), "%s", msg);
	osd_show(server, OSD_SHOT, 0, false);
}

void osd_ws(struct guibux_server *server, struct guibux_output *output, int ws) {
	struct guibux_osd *osd = &server->osd;
	char mon = output != NULL ? 'A' + (output->topbar_number - 1) : 'A';
	snprintf(osd->text, sizeof(osd->text), "%c%d", mon, ws);
	osd->font_scale = 4;
	osd_show(server, OSD_WS, 0, false);
	osd->font_scale = 1;
}

void osd_tick(struct guibux_server *server) {
	struct guibux_osd *osd = &server->osd;
	if (!osd->active) {
		return;
	}
	uint32_t now = osd_now_ms();
	if (now >= osd->hide_at_ms) {
		osd_hide(server);
	}
}

void osd_destroy(struct guibux_server *server) {
	osd_hide(server);
}

int osd_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		wlr_log(WLR_ERROR, "osd-test: FAIL no output at cursor");
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);

	osd_show(server, OSD_VOLUME, 65, false);
	if (!server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL osd not shown");
		return 0;
	}
	if (server->osd.box_x < 0 || server->osd.box_y < 0 ||
			server->osd.box_x + server->osd.box_w > box.width ||
			server->osd.box_y + server->osd.box_h > box.height) {
		wlr_log(WLR_ERROR, "osd-test: FAIL box out of bounds (%d,%d %dx%d in %dx%d)",
			server->osd.box_x, server->osd.box_y,
			server->osd.box_w, server->osd.box_h, box.width, box.height);
		return 0;
	}
	if (server->osd.buffer == NULL) {
		wlr_log(WLR_ERROR, "osd-test: FAIL no buffer");
		return 0;
	}
	if (server->osd.value != 65 || server->osd.muted != false) {
		wlr_log(WLR_ERROR, "osd-test: FAIL bad state (value=%d muted=%d)",
			server->osd.value, server->osd.muted);
		return 0;
	}
	/* tick past the timeout: the OSD must hide */
	server->osd.hide_at_ms = 0;
	osd_tick(server);
	if (server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL osd not hidden after timeout");
		return 0;
	}
	/* OSD_SHOT: text-only, no bar */
	osd_shot(server, "/home/user/Pictures/guibuxwm-20260101-120000.png");
	if (!server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL shot osd not shown");
		return 0;
	}
	if (server->osd.kind != OSD_SHOT) {
		wlr_log(WLR_ERROR, "osd-test: FAIL shot osd wrong kind");
		return 0;
	}
	if (server->osd.box_h != 2 * OSD_PAD + OSD_TEXT_H) {
		wlr_log(WLR_ERROR, "osd-test: FAIL shot osd height %d (want %d, no bar)",
			server->osd.box_h, 2 * OSD_PAD + OSD_TEXT_H);
		return 0;
	}
	server->osd.hide_at_ms = 0;
	osd_tick(server);
	if (server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL shot osd not hidden");
		return 0;
	}
	/* OSD_WS: big font, text-only, no bar */
	osd_ws(server, guibux_output_for(server, output), 3);
	if (!server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL ws osd not shown");
		return 0;
	}
	if (server->osd.kind != OSD_WS) {
		wlr_log(WLR_ERROR, "osd-test: FAIL ws osd wrong kind");
		return 0;
	}
	if (server->osd.box_h != 2 * OSD_PAD + OSD_WS_TEXT_H) {
		wlr_log(WLR_ERROR, "osd-test: FAIL ws osd height %d (want %d, big font)",
			server->osd.box_h, 2 * OSD_PAD + OSD_WS_TEXT_H);
		return 0;
	}
	server->osd.hide_at_ms = 0;
	osd_tick(server);
	if (server->osd.active) {
		wlr_log(WLR_ERROR, "osd-test: FAIL ws osd not hidden");
		return 0;
	}
	wlr_log(WLR_INFO, "osd-test: OK (volume 65%%, box %dx%d at %d,%d)",
		server->osd.box_w, server->osd.box_h,
		server->osd.box_x, server->osd.box_y);
	return 0;
}
