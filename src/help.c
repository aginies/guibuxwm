#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define HELP_LINE_H 24
#define HELP_PAD 12
#define HELP_BOX_W 560
#define HELP_MAX_LINES NUM_KEYBINDS

/* section order in the help overlay; a section is shown only when it has
 * at least one bound keybind. Audio (volume/mute/mic) is intentionally
 * not listed */
enum help_section {
	HELP_SEC_WINDOWS = 0,
	HELP_SEC_WORKSPACES,
	HELP_SEC_SNAP,
	HELP_SEC_DISPLAY,
	HELP_SEC_SYSTEM,
	HELP_SEC_COUNT,
};

static const char *help_section_names[HELP_SEC_COUNT] = {
	"WINDOWS",
	"WORKSPACES",
	"SNAP",
	"DISPLAY",
	"SYSTEM",
};

static enum help_section action_section(enum guibux_action action) {
	switch (action) {
	case GUIBUX_ACT_TERMINAL:
	case GUIBUX_ACT_CLOSE:
	case GUIBUX_ACT_FULLSCREEN:
	case GUIBUX_ACT_TILE:
	case GUIBUX_ACT_FOCUS_NEXT:
	case GUIBUX_ACT_LAUNCHER:
		return HELP_SEC_WINDOWS;
	case GUIBUX_ACT_SWITCH_WS:
	case GUIBUX_ACT_MOVE_WS:
	case GUIBUX_ACT_SWITCH_WS_LEFT:
	case GUIBUX_ACT_SWITCH_WS_RIGHT:
	case GUIBUX_ACT_MOVE_MON_LEFT:
	case GUIBUX_ACT_MOVE_MON_RIGHT:
		return HELP_SEC_WORKSPACES;
	case GUIBUX_ACT_SNAP_LEFT:
	case GUIBUX_ACT_SNAP_RIGHT:
	case GUIBUX_ACT_SNAP_TOP:
	case GUIBUX_ACT_SNAP_BOTTOM:
		return HELP_SEC_SNAP;
	case GUIBUX_ACT_BRIGHTNESS_UP:
	case GUIBUX_ACT_BRIGHTNESS_DOWN:
		return HELP_SEC_DISPLAY;
	case GUIBUX_ACT_VOLUME_UP:
	case GUIBUX_ACT_VOLUME_DOWN:
	case GUIBUX_ACT_MUTE:
	case GUIBUX_ACT_MIC_UP:
	case GUIBUX_ACT_MIC_DOWN:
	case GUIBUX_ACT_MIC_MUTE:
		/* audio is not shown in the help */
		return HELP_SEC_COUNT;
	default:
		return HELP_SEC_SYSTEM;
	}
}

static const char *action_label(enum guibux_action action, int arg) {
	switch (action) {
	case GUIBUX_ACT_TERMINAL:      return "new terminal";
	case GUIBUX_ACT_CLOSE:         return "close window";
	case GUIBUX_ACT_FULLSCREEN:    return "toggle fullscreen";
	case GUIBUX_ACT_TILE:          return "cycle tile mode";
	case GUIBUX_ACT_LAUNCHER:      return "command box";
	case GUIBUX_ACT_FOCUS_NEXT:    return "cycle focus";
	case GUIBUX_ACT_QUIT:          return "quit";
	case GUIBUX_ACT_SWITCH_WS:     return "workspace";
	case GUIBUX_ACT_MOVE_WS:       return "move to workspace";
	case GUIBUX_ACT_MOVE_MON_LEFT: return "move window left";
	case GUIBUX_ACT_MOVE_MON_RIGHT:return "move window right";
	case GUIBUX_ACT_SNAP_LEFT:     return "snap left";
	case GUIBUX_ACT_SNAP_RIGHT:    return "snap right";
	case GUIBUX_ACT_SNAP_TOP:      return "snap top";
	case GUIBUX_ACT_SNAP_BOTTOM:   return "snap bottom";
	case GUIBUX_ACT_SWITCH_WS_LEFT: return "previous workspace";
	case GUIBUX_ACT_SWITCH_WS_RIGHT: return "next workspace";
	case GUIBUX_ACT_SHOW_HELP:     return "this help";
	case GUIBUX_ACT_BRIGHTNESS_UP: return "brightness up";
	case GUIBUX_ACT_BRIGHTNESS_DOWN: return "brightness down";
	case GUIBUX_ACT_OUTPUTS_APPLY:   return "re-apply monitors";
	case GUIBUX_ACT_OUTPUTS_PANEL:  return "monitor layout";
	case GUIBUX_ACT_POWER:          return "power menu";
	case GUIBUX_ACT_RELOAD_CONFIG:  return "reload config";
	case GUIBUX_ACT_TOPBAR_ITEMS:   return "topbar items";
	case GUIBUX_ACT_LOCK:           return "lock screen";
	case GUIBUX_ACT_SCREENSHOT_FULLSCREEN: return "screenshot: monitor";
	case GUIBUX_ACT_SCREENSHOT_REGION:     return "screenshot: region";
	case GUIBUX_ACT_SCREENSHOT_WINDOW:     return "screenshot: window";
	default:                       return "unknown";
	}
}

static void format_keybind(struct guibux_keybind *kb, char *out, int sz) {
	char mods[64] = {0};
	int mi = 0;
	if (kb->modifiers & WLR_MODIFIER_LOGO)    mi += snprintf(mods + mi, sizeof(mods) - mi, "Mod+");
	if (kb->modifiers & WLR_MODIFIER_SHIFT)   mi += snprintf(mods + mi, sizeof(mods) - mi, "Shift+");
	if (kb->modifiers & WLR_MODIFIER_ALT)     mi += snprintf(mods + mi, sizeof(mods) - mi, "Alt+");
	if (kb->modifiers & WLR_MODIFIER_CTRL)    mi += snprintf(mods + mi, sizeof(mods) - mi, "Ctrl+");

	char keyname[32];
	xkb_keysym_get_name(kb->keysym, keyname, sizeof(keyname));
	char *k = keyname;
	if (*k >= 'a' && *k <= 'z') *k = *k - 'a' + 'A';

	const char *label = action_label(kb->action, kb->arg);
	if (kb->action == GUIBUX_ACT_SWITCH_WS || kb->action == GUIBUX_ACT_MOVE_WS) {
		snprintf(out, sz, "%s%s: %s %d", mods, k, label, kb->arg);
	} else {
		snprintf(out, sz, "%s%s: %s", mods, k, label);
	}
}

static void help_render(struct guibux_server *server) {
	struct guibux_help *h = &server->help;
	if (h->buffer == NULL || server->launcher.face == NULL) return;

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(h->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) return;
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(h->buffer);
		return;
	}

	int w = h->box_w * h->box_scale;
	int hgt = h->box_h * h->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, hgt, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, h->box_scale);
	cairo_rectangle(cr, h->box_scale / 2.0, h->box_scale / 2.0,
		w - h->box_scale, hgt - h->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * h->box_scale;

	int count = h->num_lines < HELP_MAX_LINES ? h->num_lines : HELP_MAX_LINES;
	for (int i = 0; i < count; i++) {
		int ly = i * HELP_LINE_H * h->box_scale;
		int lh = HELP_LINE_H * h->box_scale;
		if (h->header[i]) {
			/* section header: highlight color, slightly larger */
			int hp = (font_px * 11) / 10;
			FT_Set_Pixel_Sizes(face, 0, hp);
			int mb = ly + lh / 2 + hp * 35 / 100;
			launcher_draw_text_on_surface(cs, face, h->lines[i],
				HELP_PAD * h->box_scale, mb, server->color_highlight);
		} else {
			FT_Set_Pixel_Sizes(face, 0, font_px);
			int mb = ly + lh / 2 + font_px * 35 / 100;
			launcher_draw_text_on_surface(cs, face, h->lines[i],
				HELP_PAD * h->box_scale, mb, server->color_text);
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(h->buffer);
	if (h->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(h->scene_node, h->buffer);
	}
	if (h->output != NULL) {
		wlr_output_schedule_frame(h->output);
	}
}

void help_show(struct guibux_server *server) {
	struct guibux_help *h = &server->help;
	if (h->active) return;
	tooltip_hide(server);
	osd_hide(server);
	power_panel_hide(server);

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) return;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = guibux_scale_round(output->scale);

	/* bucket every keybind into its section (audio is skipped); a section
	 * gets a header line the first time one of its binds is emitted */
	bool section_open[HELP_SEC_COUNT] = { false };
	h->num_lines = 0;
	for (int sec = 0; sec < HELP_SEC_COUNT; sec++) {
		for (int i = 0; i < server->num_keybinds; i++) {
			struct guibux_keybind *kb = &server->keybinds[i];
			if (action_section(kb->action) != (enum help_section)sec) {
				continue;
			}
			if (h->num_lines >= HELP_MAX_LINES) {
				break;
			}
			if (!section_open[sec]) {
				section_open[sec] = true;
				snprintf(h->lines[h->num_lines], sizeof(h->lines[0]),
					"%s", help_section_names[sec]);
				h->header[h->num_lines] = 1;
				h->num_lines++;
				if (h->num_lines >= HELP_MAX_LINES) {
					break;
				}
			}
			format_keybind(kb, h->lines[h->num_lines], sizeof(h->lines[0]));
			h->header[h->num_lines] = 0;
			h->num_lines++;
		}
	}
	if (h->num_lines == 0) return;

	int lines = h->num_lines < HELP_MAX_LINES ? h->num_lines : HELP_MAX_LINES;
	int bw = HELP_BOX_W;
	if (bw > ew - 20) bw = ew - 20;
	h->box_w = bw;
	h->box_h = lines * HELP_LINE_H;
	h->box_scale = scale;
	h->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	h->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, h->box_h * scale, &format);
	if (h->buffer == NULL) return;
	h->scene_node = wlr_scene_buffer_create(&server->scene->tree, h->buffer);
	wlr_scene_buffer_set_dest_size(h->scene_node, bw, h->box_h);
	wlr_scene_node_set_position(&h->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - h->box_h) / 2);

	h->active = true;
	help_render(server);
}

void help_hide(struct guibux_server *server) {
	struct guibux_help *h = &server->help;
	if (!h->active) return;
	h->active = false;
	h->num_lines = 0;
	h->output = NULL;
	if (h->scene_node != NULL) {
		wlr_scene_node_destroy(&h->scene_node->node);
		h->scene_node = NULL;
	}
	if (h->buffer != NULL) {
		wlr_buffer_drop(h->buffer);
		h->buffer = NULL;
	}
}

bool help_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_help *h = &server->help;
	if (!h->active) return false;

	switch (sym) {
	case XKB_KEY_Escape:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		help_hide(server);
		return true;
	default:
		break;
	}
	return true;
}
