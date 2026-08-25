#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <string.h>

#define OSD_PAD 12
#define OSD_MIN_W 220
#define OSD_TEXT_H 24
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
	int font_px = server->topbar_font_size * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);

	const char *label;
	char text[64];
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

	int tw = guibux_text_width(server->launcher.face, text) / scale;
	int bw = tw + 2 * OSD_PAD;
	if (bw < OSD_MIN_W)
		bw = OSD_MIN_W;
	int bh = OSD_PAD + OSD_TEXT_H + OSD_TEXT_BAR_GAP + OSD_BAR_H + OSD_PAD;

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
			set_color(cr, server->color_bg);
			cairo_paint(cr);
			set_color(cr, server->color_border);
			cairo_set_line_width(cr, scale);
			cairo_rectangle(cr, scale / 2.0, scale / 2.0, w - scale, h - scale);
			cairo_stroke(cr);
			int baseline = OSD_PAD * scale + OSD_TEXT_H * scale / 2 +
				font_px * 35 / 100;
			launcher_draw_text_on_surface(cs, server->launcher.face, text,
				OSD_PAD * scale, baseline, server->color_text);
			int bar_y = (OSD_PAD + OSD_TEXT_H + OSD_TEXT_BAR_GAP) * scale;
			int bar_w = (w - 2 * OSD_PAD * scale);
			set_color(cr, server->color_border);
			cairo_rectangle(cr, OSD_PAD * scale, bar_y, bar_w, OSD_BAR_H * scale);
			cairo_fill(cr);
			int fill_w = muted ? 0 : (bar_w * value) / 100;
			if (fill_w > bar_w)
				fill_w = bar_w;
			if (fill_w < 0)
				fill_w = 0;
			if (fill_w > 0) {
				set_color(cr, server->color_text);
				cairo_rectangle(cr, OSD_PAD * scale, bar_y, fill_w, OSD_BAR_H * scale);
				cairo_fill(cr);
			}
			cairo_destroy(cr);
			cairo_surface_destroy(cs);
		}
		wlr_buffer_end_data_ptr_access(osd->buffer);
	}
	wlr_scene_buffer_set_buffer(osd->scene_node, osd->buffer);
	wlr_output_schedule_frame(output);
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
	wlr_log(WLR_INFO, "osd-test: OK (volume 65%%, box %dx%d at %d,%d)",
		server->osd.box_w, server->osd.box_h,
		server->osd.box_x, server->osd.box_y);
	return 0;
}
