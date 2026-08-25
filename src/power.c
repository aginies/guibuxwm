#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
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
	"guibuxwm-lock 2>/dev/null || loginctl lock-session",
	"loginctl terminate-session '$XDG_SESSION_ID' 2>/dev/null || "
		"loginctl terminate-user '$USER'",
	"systemctl reboot",
	"systemctl poweroff",
};

/* run a power command through the shell with the session env available */
static void power_run(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "power: fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	wlr_log(WLR_INFO, "power: %s (pid %d)", cmd, pid);
}

/* which binaries back each action; an action is available when its
 * binary is on $PATH. Probed once, on the first panel show */
static const char *power_binaries[POWER_COUNT] = {
	"systemctl",  /* suspend   */
	"systemctl",  /* hibernate */
	"loginctl",   /* lock      */
	"loginctl",   /* log out   */
	"systemctl",  /* restart   */
	"systemctl",  /* shut down */
};

static bool power_avail[POWER_COUNT];
static bool power_probed = false;

static void power_probe(void) {
	if (power_probed) {
		return;
	}
	power_probed = true;
	for (int i = 0; i < POWER_COUNT; i++) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1",
			power_binaries[i]);
		pid_t pid = fork();
		if (pid < 0) {
			power_avail[i] = false;
			continue;
		}
		if (pid == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
			_exit(127);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		power_avail[i] = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	}
}

static void power_panel_render(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
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

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, p->box_scale);
	cairo_rectangle(cr, p->box_scale / 2.0, p->box_scale / 2.0,
		w - p->box_scale, hgt - p->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * p->box_scale;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	for (int i = 0; i < POWER_COUNT; i++) {
		int ly = i * POWER_LINE_H * p->box_scale;
		int lh = POWER_LINE_H * p->box_scale;
		bool avail = power_avail[i];
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
		char key[8];
		snprintf(key, sizeof(key), "%s", power_keys[i]);
		int kw = guibux_text_width(face, key) / p->box_scale;
		uint32_t kc = !avail ? server->color_dim : server->color_dim;
		launcher_draw_text_on_surface(cs, face, key,
			w - POWER_PAD * p->box_scale - kw * p->box_scale,
			mb, kc);
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

void power_panel_show(struct guibux_server *server) {
	struct guibux_power_panel *p = &server->power_panel;
	if (p->active) {
		return;
	}
	tooltip_hide(server);
	osd_hide(server);

	power_probe();

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = output->scale > 1 ? (int)output->scale : 1;

	/* start on the first available action */
	p->selected = 0;
	for (int i = 0; i < POWER_COUNT; i++) {
		if (power_avail[i]) {
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
	if (p->scene_node != NULL) {
		wlr_scene_node_destroy(&p->scene_node->node);
		p->scene_node = NULL;
	}
	if (p->buffer != NULL) {
		wlr_buffer_drop(p->buffer);
		p->buffer = NULL;
	}
}

void power_panel_select(struct guibux_server *server, int idx) {
	if (idx < 0 || idx >= POWER_COUNT || !power_avail[idx]) {
		return;
	}
	power_panel_hide(server);
	power_run(power_commands[idx]);
}

/* move the selection by dir, skipping unavailable actions; wraps around */
static void power_panel_move(struct guibux_server *server, int dir) {
	struct guibux_power_panel *p = &server->power_panel;
	for (int step = 1; step <= POWER_COUNT; step++) {
		int i = (p->selected + dir * step + POWER_COUNT * 2) % POWER_COUNT;
		if (power_avail[i]) {
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
	int idx = (int)((y - py) / POWER_LINE_H);
	if (idx < 0 || idx >= POWER_COUNT || !power_avail[idx]) {
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
