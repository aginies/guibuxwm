#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#define POWER_LINE_H 28
#define POWER_PAD 16
#define POWER_BOX_W 280

static const char *power_labels[POWER_COUNT] = {
	"Suspend",
	"Hibernate",
	"Lock",
	"Log out",
	"Restart",
	"Shut down",
};

static const char *power_keys[POWER_COUNT] = {
	"s",
	"h",
	"l",
	"o",
	"r",
	"q",
};

static const char *power_commands[POWER_COUNT] = {
	"systemctl suspend",
	"systemctl hibernate",
	NULL,  /* lock: handled in-process via lock_show() */
	"loginctl terminate-session '$XDG_SESSION_ID' 2>/dev/null || "
		"loginctl terminate-user '$USER'",
	"systemctl reboot",
	"systemctl poweroff",
};


/* which binaries back each action; an action is available when its
 * binary is on $PATH. Probed once at startup by power_panel_init. Lock is
 * NULL: it runs in-process (lock_show) and needs no binary */
static const char *power_binaries[POWER_COUNT] = {
	"systemctl",  /* suspend   */
	"systemctl",  /* hibernate */
	NULL,         /* lock      */
	"loginctl",   /* log out   */
	"systemctl",  /* restart   */
	"systemctl",  /* shut down */
};

/* true when `name` is an executable somewhere on $PATH. A direct scan
 * replaces the old `sh -c "command -v X"` probe, which forked six shells
 * with a blocking waitpid on the main thread and stalled the compositor
 * the first time the power menu was opened */
static bool binary_on_path(const char *name) {
	const char *path = getenv("PATH");
	if (path == NULL || path[0] == '\0') {
		path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	}
	size_t name_len = strlen(name);
	char probe[PATH_MAX];
	const char *seg = path;
	while (seg != NULL) {
		const char *colon = strchr(seg, ':');
		size_t len = colon != NULL ? (size_t)(colon - seg) : strlen(seg);
		/* +2 for the '/' and the NUL */
		if (len > 0 && len + name_len + 2 <= sizeof(probe)) {
			memcpy(probe, seg, len);
			probe[len] = '/';
			memcpy(probe + len + 1, name, name_len + 1);
			if (access(probe, X_OK) == 0) {
				return true;
			}
		}
		seg = colon != NULL ? colon + 1 : NULL;
	}
	return false;
}

void power_panel_init(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	if (p->probed) {
		return;
	}
	p->probed = true;
	for (int i = 0; i < POWER_COUNT; i++) {
		/* no binary = always available (in-process action) */
		p->avail[i] = power_binaries[i] == NULL ||
			binary_on_path(power_binaries[i]);
	}
}

static void power_panel_render(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	if (p->buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	struct guibux_panel_draw d;
	if (!panel_begin_draw(p->buffer, p->box_w * p->box_scale,
			p->box_h * p->box_scale, p->box_scale, &d)) {
		return;
	}
	int w = d.w;
	int hgt = d.h;
	cairo_t *cr = d.cr;
	cairo_surface_t *cs = d.surface;

	panel_draw_frame(&d, server->color_bg);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * p->box_scale;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	if (p->confirming) {
		/* confirmation overlay: dim the list, show a prompt */
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
		cairo_rectangle(cr, 0, 0, w, hgt);
		cairo_fill(cr);
		char msg[96];
		snprintf(msg, sizeof(msg), "Confirm %s?", power_labels[p->confirm_idx]);
		int mw = guibux_text_width(face, msg) / p->box_scale;
		uint32_t mc = server->color_highlight;
		launcher_draw_text_on_surface(cs, face, msg,
			(w - mw * p->box_scale) / 2,
			(hgt / 2) * p->box_scale - font_px * p->box_scale / 2, mc);
		char hint[64];
		snprintf(hint, sizeof(hint), "%s", "Enter=yes  Esc=no");
		int hw = guibux_text_width(face, hint) / p->box_scale;
		uint32_t hc = server->color_text;
		launcher_draw_text_on_surface(cs, face, hint,
			(w - hw * p->box_scale) / 2,
			(hgt / 2) * p->box_scale + font_px * p->box_scale / 2, hc);
		panel_end_draw(&d, p->buffer, p->scene_node, p->output);
		return;
	}

	for (int i = 0; i < POWER_COUNT; i++) {
		int ly = i * POWER_LINE_H * p->box_scale;
		int lh = POWER_LINE_H * p->box_scale;
		bool avail = p->avail[i];
		if (i == p->selected && avail) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, 0, ly, w, lh);
			cairo_fill(cr);
		}
		/* unavailable actions (missing binary) are dimmed and not
		 * selectable */
		uint32_t tc = !avail ? server->color_dim : server->color_text;
		int mb = ly + lh / 2 + font_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, power_labels[i],
			POWER_PAD * p->box_scale, mb, tc);
		/* right-aligned key hint */
		int kw = guibux_text_width(face, power_keys[i]) / p->box_scale;
		launcher_draw_text_on_surface(cs, face, power_keys[i],
			w - POWER_PAD * p->box_scale - kw * p->box_scale,
			mb, server->color_dim);
	}

	panel_end_draw(&d, p->buffer, p->scene_node, p->output);
}

void power_panel_show(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	if (p->active) {
		return;
	}
	tooltip_hide(server);
	osd_hide(server);

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = guibux_scale_round(output->scale);

	/* start on the first available action */
	p->selected = 0;
	p->confirming = false;
	p->confirm_idx = 0;
	for (int i = 0; i < POWER_COUNT; i++) {
		if (p->avail[i]) {
			p->selected = i;
			break;
		}
	}
	int bw = POWER_BOX_W;
	if (bw > ew - 20) {
		bw = ew - 20;
	}
	p->box_w = bw;
	p->box_h = POWER_COUNT * POWER_LINE_H;
	p->box_scale = scale;
	p->output = output;

	int lx, ly;
	panel_centred_pos(server, output, bw, p->box_h, &lx, &ly);
	if (!panel_create(server, lx, ly, scale, bw, p->box_h,
			&p->buffer, &p->scene_node)) {
		p->output = NULL;
		return;
	}
	wlr_scene_node_raise_to_top(&p->scene_node->node);
	topbar_raise_all(server);

	p->active = true;
	power_panel_render(server);
}

void power_panel_hide(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	if (!p->active) {
		return;
	}
	p->active = false;
	p->selected = 0;
	p->output = NULL;
	panel_destroy(&p->buffer, &p->scene_node);
}

void power_panel_select(struct guibux_server *server, int idx) {
	struct guibux_power_panel *p = &server->power_panel;
	if (idx < 0 || idx >= POWER_COUNT || !p->avail[idx]) {
		return;
	}
	if (idx == POWER_LOCK) {
		power_panel_hide(server);
		/* in-process lock screen: PAM auth, no external helper */
		lock_show(server);
		return;
	}
	p->confirming = true;
	p->confirm_idx = idx;
	power_panel_render(server);
}

static void power_panel_confirm(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	int idx = p->confirm_idx;
	power_panel_hide(server);
	guibux_spawn(power_commands[idx], "power");
}

static void power_panel_cancel(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	p->confirming = false;
	p->confirm_idx = 0;
	power_panel_render(server);
}

/* move the selection by dir, skipping unavailable actions; wraps around */
static void power_panel_move(struct guibux_server *server, int dir) {
	struct guibux_power_panel *p = &server->power_panel;
	for (int step = 1; step <= POWER_COUNT; step++) {
		int i = (p->selected + dir * step + POWER_COUNT * 2) % POWER_COUNT;
		if (p->avail[i]) {
			p->selected = i;
			break;
		}
	}
	power_panel_render(server);
}

bool power_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_power_panel *p = &server->power_panel;
	if (!p->active) {
		return false;
	}
	if (p->confirming) {
		switch (sym) {
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			power_panel_confirm(server);
			return true;
		case XKB_KEY_Escape:
			power_panel_cancel(server);
			return true;
		default:
			return true;
		}
	}
	switch (sym) {
	case XKB_KEY_Escape:
		power_panel_hide(server);
		return true;
	case XKB_KEY_Up:
		power_panel_move(server, -1);
		return true;
	case XKB_KEY_Down:
		power_panel_move(server, 1);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		power_panel_select(server, p->selected);
		return true;
	case XKB_KEY_s:
		power_panel_select(server, POWER_SUSPEND);
		return true;
	case XKB_KEY_h:
		power_panel_select(server, POWER_HIBERNATE);
		return true;
	case XKB_KEY_l:
		power_panel_select(server, POWER_LOCK);
		return true;
	case XKB_KEY_o:
		power_panel_select(server, POWER_LOGOUT);
		return true;
	case XKB_KEY_r:
		power_panel_select(server, POWER_RESTART);
		return true;
	case XKB_KEY_q:
		power_panel_select(server, POWER_SHUTDOWN);
		return true;
	default:
		/* unhandled keys (e.g. Mod+p to toggle the panel) fall
		 * through to normal keybind dispatch */
		return false;
	}
}

int power_panel_action_at(struct guibux_server *server, double lx, double ly) {
	struct guibux_power_panel *p = &server->power_panel;
	if (!p->active) {
		return -1;
	}
	int idx = panel_row_at(server, p->output, p->box_w, p->box_h,
		POWER_LINE_H, POWER_COUNT, lx, ly);
	/* unavailable actions (missing binary) are dimmed and not clickable */
	if (idx >= 0 && !p->avail[idx]) {
		return -1;
	}
	return idx;
}

void power_panel_destroy(struct guibux_server *server) {
	power_panel_hide(server);
}

int power_panel_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		wlr_log(WLR_ERROR, "power-test: FAIL no output at cursor");
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);

	power_panel_show(server);
	if (!server->power_panel.active) {
		wlr_log(WLR_ERROR, "power-test: FAIL panel not shown");
		return 0;
	}
	if (server->power_panel.buffer == NULL) {
		wlr_log(WLR_ERROR, "power-test: FAIL no buffer");
		return 0;
	}
	if (server->power_panel.box_h != POWER_COUNT * POWER_LINE_H) {
		wlr_log(WLR_ERROR, "power-test: FAIL bad height %d",
			server->power_panel.box_h);
		return 0;
	}
	/* click the middle of the first row: must resolve to Suspend */
	int px = (ew - server->power_panel.box_w) / 2;
	int py = (eh - server->power_panel.box_h) / 2;
	int idx = power_panel_action_at(server, box.x + px + 10,
		box.y + py + POWER_LINE_H / 2.0);
	if (idx != POWER_SUSPEND) {
		wlr_log(WLR_ERROR, "power-test: FAIL row hit (got %d, want %d)",
			idx, POWER_SUSPEND);
		return 0;
	}
	/* a point below the panel must miss */
	idx = power_panel_action_at(server, box.x + px + 10,
		box.y + py + server->power_panel.box_h + 50);
	if (idx != -1) {
		wlr_log(WLR_ERROR, "power-test: FAIL out-of-bounds hit %d", idx);
		return 0;
	}
	/* Esc must close the panel */
	power_panel_handle_key(server, XKB_KEY_Escape);
	if (server->power_panel.active) {
		wlr_log(WLR_ERROR, "power-test: FAIL panel not hidden on Esc");
		return 0;
	}
	wlr_log(WLR_INFO, "power-test: OK (%d actions, box %dx%d)",
		POWER_COUNT, server->power_panel.box_w, server->power_panel.box_h);
	return 0;
}
