#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <wlr/backend/libinput.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Spawned children (terminals, launcher commands) are reaped by a
 * SIGCHLD handler that only waits for PIDs we track. SIG_IGN would
 * work for the compositor but is inherited by Xwayland, whose X server
 * relies on waitpid() for xkbcomp and would lose its child; waiting
 * only for tracked PIDs is a no-op in the inherited copy (none of
 * them are Xwayland's children).
 *
 * The table is a fixed lock-free ring: the SIGCHLD handler (any
 * thread, async signal context) and spawn_track (main thread) must
 * not share a mutex (not async-signal-safe) or plain counters (data
 * race). A slot is claimed by writing the PID last; the reaper only
 * trusts a slot whose PID is positive and either reaps it or leaves
 * it for the next pass. A PID can be lost if the ring is full
 * (zombie until exit) - acceptable at SPAWNED_MAX slots.
 */
#define SPAWNED_MAX 256
static pid_t spawned_pids[SPAWNED_MAX];

void spawn_track(pid_t pid) {
	/* scan for a free (0) slot; pid_t 0 is never a valid child pid */
	for (int i = 0; i < SPAWNED_MAX; i++) {
		if (spawned_pids[i] == 0) {
			spawned_pids[i] = pid;
			return;
		}
	}
	/* ring full: the child becomes a zombie until the WM exits */
	wlr_log(WLR_ERROR, "spawn: pid table full, %d will not be reaped", (int)pid);
}

static void spawn_reap(void) {
	for (int i = 0; i < SPAWNED_MAX; i++) {
		pid_t pid = spawned_pids[i];
		if (pid == 0) {
			continue;
		}
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid || r == -1) {
			/* exited (or already reaped: duplicate slot) */
			spawned_pids[i] = 0;
		}
	}
}

void spawn_sigchld_handler(int sig) {
	(void)sig;
	spawn_reap();
}

void spawn_terminal(struct guibux_server *server) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", server->term_cmd, (void *)NULL);
		_exit(127);
	}
	spawn_track(pid);
	wlr_log(WLR_INFO, "spawned terminal (%s) pid %d", server->term_cmd, pid);
}

void spawn_network_info(struct guibux_server *server) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "%s -- bash -c \"nmtui\"", server->term_cmd);
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	spawn_track(pid);
	wlr_log(WLR_INFO, "spawned nmtui terminal pid %d", pid);
}

/* fork /bin/sh -c cmd, track the child for reaping */
static void spawn_cmd(const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	spawn_track(pid);
}

void spawn_mixer(struct guibux_server *server) {
	(void)server;
	spawn_cmd("pavucontrol 2>/dev/null");
	wlr_log(WLR_INFO, "spawned pavucontrol");
}

/*
 * Make the session environment complete so apps can open URLs:
 *  - children we spawn (terminals, launcher apps) inherit our env, so
 *    fix it here: a WM started from a TTY has XDG_SESSION_TYPE=tty and
 *    no XDG_CURRENT_DESKTOP;
 *  - xdg-desktop-portal, its backends and gnome-terminal-server are
 *    systemd user services (D-Bus activated) started with the bare
 *    user-manager env (no DISPLAY / WAYLAND_DISPLAY). A URL click in
 *    such an app forks the browser with that env: no WAYLAND_DISPLAY
 *    means the browser assumes an X11 session, no DISPLAY means it
 *    dies with "no DISPLAY environment variable specified". Set the
 *    display variables on the user manager (SetEnvironment via
 *    busctl; the `systemd` binary is not always installed) and
 *    restart the affected services so the running ones pick them up.
 *    Best effort: never fail startup.
 */
void setup_session_environment(struct guibux_server *server) {
	setenv("XDG_SESSION_TYPE", "wayland", true);
	if (getenv("XDG_CURRENT_DESKTOP") == NULL) {
		setenv("XDG_CURRENT_DESKTOP", "guibuxwm", true);
		setenv("XDG_SESSION_DESKTOP", "guibuxwm", true);
	}
	if (getenv("DBUS_SESSION_BUS_ADDRESS") == NULL) {
		wlr_log(WLR_INFO, "session env: no DBUS_SESSION_BUS_ADDRESS, "
			"skipping portal environment import");
		return;
	}
	if (getenv("GUIBUX_TEST_EXTRA_OUTPUTS") != NULL ||
			(getenv("GUIBUX_OUTPUTS") != NULL &&
			 getenv("GUIBUX_OUTPUTS")[0] == '\0')) {
		/* headless test: don't touch the real user session (the
		 * service restarts below would kill the user's terminals) */
		return;
	}
	/* main.c setenv()s these right before calling us; children we
	 * spawn inherit the same values, so import exactly those */
	const char *wl_socket = getenv("WAYLAND_DISPLAY");
	const char *display = getenv("DISPLAY");
	const char *desktop = getenv("XDG_CURRENT_DESKTOP");
	char cmd[1024];
	if (display != NULL) {
		snprintf(cmd, sizeof(cmd),
			"busctl --user call org.freedesktop.systemd1 "
			"/org/freedesktop/systemd1 "
			"org.freedesktop.systemd1.Manager SetEnvironment as 5 "
			"DISPLAY='%s' WAYLAND_DISPLAY='%s' "
			"XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP='%s' "
			"XDG_SESSION_DESKTOP='%s' 2>/dev/null && "
			"systemctl --user restart 'xdg-desktop-portal*' "
			"2>/dev/null; systemctl --user try-restart "
			"gnome-terminal-server 2>/dev/null",
			display, wl_socket, desktop, desktop);
	} else {
		snprintf(cmd, sizeof(cmd),
			"busctl --user call org.freedesktop.systemd1 "
			"/org/freedesktop/systemd1 "
			"org.freedesktop.systemd1.Manager SetEnvironment as 4 "
			"WAYLAND_DISPLAY='%s' XDG_SESSION_TYPE=wayland "
			"XDG_CURRENT_DESKTOP='%s' XDG_SESSION_DESKTOP='%s' "
			"2>/dev/null && "
			"systemctl --user restart 'xdg-desktop-portal*' "
			"2>/dev/null; systemctl --user try-restart "
			"gnome-terminal-server 2>/dev/null",
			wl_socket, desktop, desktop);
	}
	spawn_cmd(cmd);
	wlr_log(WLR_INFO, "session env: set DISPLAY/WAYLAND_DISPLAY on "
		"systemd --user, restarting portal + terminal services");
}

/*
 * Adjust the default sink (or source, mic) volume by delta_pct
 * (positive = up, negative = down) via pactl, then refresh the
 * topbar so the indicator updates immediately.
 */
/*
 * Concurrent relative pactl volume changes are not safe: PulseAudio
 * can drop one of two simultaneous relative sets (e.g. a scroll step
 * racing the next one). So at most one volume child runs at a time;
 * changes requested while it runs accumulate in the pending counters
 * and are applied when it exits. volume_flush() must be called from
 * the main loop (topbar_tick does this every 500ms).
 */
static int vol_pending_sink = 0;
static int vol_pending_mic = 0;
static pid_t vol_child_pid = -1;

static void volume_spawn(struct guibux_server *server, bool mic,
		int delta_pct) {
	char cmd[128];
	/* pactl >= 17 wants the sign prefixed ("+5%"); the old trailing
	 * form ("5%+") is rejected with "Invalid volume specification" */
	if (mic) {
		snprintf(cmd, sizeof(cmd),
			"pactl set-source-volume @DEFAULT_SOURCE@ %c%d%% 2>/dev/null",
			delta_pct > 0 ? '+' : '-', abs(delta_pct));
	} else {
		snprintf(cmd, sizeof(cmd),
			"pactl set-sink-volume @DEFAULT_SINK@ %c%d%% 2>/dev/null",
			delta_pct > 0 ? '+' : '-', abs(delta_pct));
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %m");
		vol_child_pid = -1;
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	vol_child_pid = pid;
	/* deliberately not spawn_track()'d: volume_flush() owns this
	 * child's reap so the SIGCHLD handler never steals it */
}

void volume_flush(struct guibux_server *server) {
	if (vol_child_pid != -1) {
		int st;
		pid_t r = waitpid(vol_child_pid, &st, WNOHANG);
		if (r == vol_child_pid || (r == -1 && errno != EINTR)) {
			vol_child_pid = -1;
		} else {
			return; /* child still running: keep the pending deltas */
		}
	}
	if (vol_pending_sink != 0) {
		int d = vol_pending_sink;
		vol_pending_sink = 0;
		volume_spawn(server, false, d);
	}
	if (vol_child_pid == -1 && vol_pending_mic != 0) {
		int d = vol_pending_mic;
		vol_pending_mic = 0;
		volume_spawn(server, true, d);
	}
}

void volume_change(struct guibux_server *server, bool mic, int delta_pct) {
	/* apply the change to the published snapshot and re-render the
	 * topbar now: the worker poll would only show the new value up to
	 * 5s later */
	sysinfo_audio_adjust(&server->sysinfo, mic, delta_pct);
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		topbar_mark_dirty(o);
		topbar_render(o);
	}
	if (mic) {
		vol_pending_mic += delta_pct;
	} else {
		vol_pending_sink += delta_pct;
	}
	volume_flush(server);
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	osd_show(server, mic ? OSD_MIC : OSD_VOLUME,
		mic ? snap.mic_volume : snap.volume,
		mic ? snap.mic_muted : snap.muted);
}

void volume_toggle_mute(struct guibux_server *server, bool mic) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), mic ?
		"pactl set-source-mute @DEFAULT_SOURCE@ toggle 2>/dev/null" :
		"pactl set-sink-mute @DEFAULT_SINK@ toggle 2>/dev/null");
	spawn_cmd(cmd);
	sysinfo_audio_toggle_mute(&server->sysinfo, mic);
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		topbar_mark_dirty(o);
		topbar_render(o);
	}
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	osd_show(server, mic ? OSD_MIC : OSD_VOLUME,
		mic ? snap.mic_volume : snap.volume,
		mic ? snap.mic_muted : snap.muted);
}

/*
 * Adjust screen brightness by delta_pct via brightnessctl.
 * `--` ends option parsing; without it brightnessctl's getopt treats
 * `-5%` as flags and fails with "invalid option -- '5'".
 */
static void brightness_change(struct guibux_server *server, int delta_pct) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd),
		"brightnessctl set -- %c%d%% 2>/dev/null",
		delta_pct > 0 ? '+' : '-', abs(delta_pct));
	spawn_cmd(cmd);
	sysinfo_brightness_adjust(&server->sysinfo, delta_pct);
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	osd_show(server, OSD_BRIGHTNESS, snap.brightness, false);
}

// ---------------------------------------------------------------------------
// Keybind actions
// ---------------------------------------------------------------------------

void do_action(struct guibux_server *server, enum guibux_action action,
		int arg, struct guibux_toplevel *toplevel) {
	switch (action) {
	case GUIBUX_ACT_TERMINAL:
		spawn_terminal(server);
		break;
	case GUIBUX_ACT_CLOSE:
		if (toplevel != NULL) {
			toplevel_close(toplevel);
		}
		break;
	case GUIBUX_ACT_FULLSCREEN:
		if (toplevel != NULL) {
			set_fullscreen(toplevel, !toplevel->is_fullscreen, NULL);
		}
		break;
	case GUIBUX_ACT_TILE:
		if (toplevel != NULL) {
			struct guibux_output *o = guibux_output_for(server,
				toplevel_output_for(toplevel));
			if (o != NULL) {
				o->tile_modes[o->current_workspace] = (o->tile_modes[o->current_workspace] + 1) % 3;
				o->tile_mode = o->tile_modes[o->current_workspace];  // sync active
				retile_output(o);
				wlr_log(WLR_INFO, "tile mode on %s: %s",
					o->wlr_output->name ? o->wlr_output->name : "(unknown)",
					o->tile_modes[o->current_workspace] == GUIBUX_TILE_FREE ? "free"
					: o->tile_modes[o->current_workspace] == GUIBUX_TILE_SPLIT ? "split"
					: "main+stack");
			}
		}
		break;
	case GUIBUX_ACT_LAUNCHER:
		launcher_show(server);
		break;
	case GUIBUX_ACT_FOCUS_NEXT:
		switcher_show(server);
		break;
	case GUIBUX_ACT_QUIT:
		wl_display_terminate(server->wl_display);
		break;
	case GUIBUX_ACT_SWITCH_WS: {
		struct wlr_output *out = output_at_cursor(server);
		if (out == NULL && toplevel != NULL) {
			out = toplevel_output_for(toplevel);
		}
		struct guibux_output *o = out != NULL
			? guibux_output_for(server, out) : NULL;
		if (o != NULL) {
			switch_workspace(o, arg);
		}
		break;
	}
	case GUIBUX_ACT_MOVE_WS:
		if (toplevel != NULL) {
			move_toplevel_to_workspace(toplevel, arg);
		}
		break;
	case GUIBUX_ACT_MOVE_MON_LEFT:
		if (toplevel != NULL) {
			move_toplevel_to_adjacent_output(server, toplevel, -1);
		}
		break;
	case GUIBUX_ACT_MOVE_MON_RIGHT:
		if (toplevel != NULL) {
			move_toplevel_to_adjacent_output(server, toplevel, 1);
		}
		break;
	case GUIBUX_ACT_SNAP_LEFT:
		if (toplevel != NULL) {
			snap_toplevel_left(toplevel);
		}
		break;
	case GUIBUX_ACT_SNAP_RIGHT:
		if (toplevel != NULL) {
			snap_toplevel_right(toplevel);
		}
		break;
	case GUIBUX_ACT_SNAP_TOP:
		if (toplevel != NULL) {
			snap_toplevel_top(toplevel);
		}
		break;
	case GUIBUX_ACT_SNAP_BOTTOM:
		if (toplevel != NULL) {
			snap_toplevel_bottom(toplevel);
		}
		break;
	case GUIBUX_ACT_SWITCH_WS_LEFT: {
		struct wlr_output *out = output_at_cursor(server);
		if (out == NULL && toplevel != NULL) {
			out = toplevel_output_for(toplevel);
		}
		struct guibux_output *o = out != NULL
			? guibux_output_for(server, out) : NULL;
		if (o != NULL) {
			int ws = o->current_workspace - 1;
			if (ws < 1) ws = NUM_WORKSPACES;
			switch_workspace(o, ws);
		}
		break;
	}
	case GUIBUX_ACT_SWITCH_WS_RIGHT: {
		struct wlr_output *out = output_at_cursor(server);
		if (out == NULL && toplevel != NULL) {
			out = toplevel_output_for(toplevel);
		}
		struct guibux_output *o = out != NULL
			? guibux_output_for(server, out) : NULL;
		if (o != NULL) {
			int ws = o->current_workspace + 1;
			if (ws > NUM_WORKSPACES) ws = 1;
			switch_workspace(o, ws);
		}
		break;
	}
	case GUIBUX_ACT_SHOW_HELP:
		help_show(server);
		break;
	case GUIBUX_ACT_VOLUME_UP:
		volume_change(server, false, 5);
		break;
	case GUIBUX_ACT_VOLUME_DOWN:
		volume_change(server, false, -5);
		break;
	case GUIBUX_ACT_MUTE:
		volume_toggle_mute(server, false);
		break;
	case GUIBUX_ACT_MIC_UP:
		volume_change(server, true, 5);
		break;
	case GUIBUX_ACT_MIC_DOWN:
		volume_change(server, true, -5);
		break;
	case GUIBUX_ACT_MIC_MUTE:
		volume_toggle_mute(server, true);
		break;
	case GUIBUX_ACT_BRIGHTNESS_UP:
		brightness_change(server, 5);
		break;
	case GUIBUX_ACT_BRIGHTNESS_DOWN:
		brightness_change(server, -5);
		break;
	case GUIBUX_ACT_OUTPUTS_APPLY:
		outputs_apply(server);
		break;
	case GUIBUX_ACT_OUTPUTS_PANEL:
		if (server->outputs_panel.active) {
			outputs_panel_hide(server);
		} else {
			outputs_panel_show(server);
		}
		break;
	case GUIBUX_ACT_POWER:
		if (server->power_panel.active) {
			power_panel_hide(server);
		} else {
			power_panel_show(server);
		}
		break;
	case GUIBUX_ACT_RELOAD_CONFIG:
		config_reload(server);
		break;
	case GUIBUX_ACT_TOPBAR_ITEMS:
		if (server->topbar_items_panel.active) {
			topbar_items_panel_hide(server);
		} else {
			topbar_items_panel_show(server);
		}
		break;
	case GUIBUX_ACT_LOCK:
		lock_show(server);
		break;
	case GUIBUX_ACT_SCREENSHOT_FULLSCREEN:
		screenshot_fullscreen(server);
		break;
	case GUIBUX_ACT_SCREENSHOT_REGION:
		screenshot_region_begin(server);
		break;
	case GUIBUX_ACT_SCREENSHOT_WINDOW:
		screenshot_window(server, toplevel);
		break;
	}
}

// ---------------------------------------------------------------------------
// Keybind table
// ---------------------------------------------------------------------------

void keybinds_reset(struct guibux_server *server) {
	server->num_keybinds = 0;
}

void keybind_add(struct guibux_server *server, uint32_t modifiers,
		xkb_keysym_t keysym, enum guibux_action action, int arg) {
	for (int i = 0; i < server->num_keybinds; i++) {
		struct guibux_keybind *kb = &server->keybinds[i];
		if (kb->modifiers == modifiers && kb->keysym == keysym) {
			kb->action = action;
			kb->arg = arg;
			return;
		}
	}
	if (server->num_keybinds >= NUM_KEYBINDS) {
		wlr_log(WLR_ERROR, "config: too many keybinds (max %d)", NUM_KEYBINDS);
		return;
	}
	struct guibux_keybind *kb = &server->keybinds[server->num_keybinds++];
	kb->modifiers = modifiers;
	kb->keysym = keysym;
	kb->action = action;
	kb->arg = arg;
}

void keybinds_defaults(struct guibux_server *server) {
	keybind_add(server, WLR_MODIFIER_ALT, XKB_KEY_Escape, GUIBUX_ACT_QUIT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Escape,
		GUIBUX_ACT_QUIT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Return, GUIBUX_ACT_TERMINAL, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_q, GUIBUX_ACT_CLOSE, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_f, GUIBUX_ACT_FULLSCREEN, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_t, GUIBUX_ACT_TILE, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_e, GUIBUX_ACT_LAUNCHER, 0);
	keybind_add(server, WLR_MODIFIER_ALT, XKB_KEY_Tab, GUIBUX_ACT_FOCUS_NEXT, 0);
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_1 + ws - 1,
			GUIBUX_ACT_SWITCH_WS, ws);
		keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT,
			XKB_KEY_1 + ws - 1, GUIBUX_ACT_MOVE_WS, ws);
	}
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Left,
		GUIBUX_ACT_MOVE_MON_LEFT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Right,
		GUIBUX_ACT_MOVE_MON_RIGHT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Left,
		GUIBUX_ACT_SNAP_LEFT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Right,
		GUIBUX_ACT_SNAP_RIGHT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Up,
		GUIBUX_ACT_FULLSCREEN, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Left,
		GUIBUX_ACT_SWITCH_WS_LEFT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Right,
		GUIBUX_ACT_SWITCH_WS_RIGHT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Up,
		GUIBUX_ACT_SNAP_TOP, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_Down,
		GUIBUX_ACT_SNAP_BOTTOM, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_q,
		GUIBUX_ACT_QUIT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_h,
		GUIBUX_ACT_SHOW_HELP, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_m,
		GUIBUX_ACT_OUTPUTS_PANEL, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_p,
		GUIBUX_ACT_POWER, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_l,
		GUIBUX_ACT_TOPBAR_ITEMS, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_l,
		GUIBUX_ACT_LOCK, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Print,
		GUIBUX_ACT_SCREENSHOT_FULLSCREEN, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Print,
		GUIBUX_ACT_SCREENSHOT_REGION, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_Print,
		GUIBUX_ACT_SCREENSHOT_WINDOW, 0);
}

bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym,
		uint32_t modifiers) {
	uint32_t mods = modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT |
		WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL);
	/* the action targets the keyboard-focused window, not the most
	 * recently mapped one (map order and focus can differ after a
	 * workspace switch or clear_keyboard_focus) */
	struct guibux_toplevel *toplevel = NULL;
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (focused != NULL) {
		struct guibux_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (toplevel_get_surface(t) == focused) {
				toplevel = t;
				break;
			}
		}
	}
	for (int i = 0; i < server->num_keybinds; i++) {
		struct guibux_keybind *kb = &server->keybinds[i];
		if (kb->modifiers == mods && kb->keysym == sym) {
			do_action(server, kb->action, kb->arg, toplevel);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Keyboard input
// ---------------------------------------------------------------------------

/* set the seat's active keyboard and notify the client of the current
 * modifier state; called before every key/modifier event so the client
 * sees the right keymap */
static void keyboard_notify_modifiers(struct guibux_server *server,
		struct guibux_keyboard *keyboard) {
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	struct guibux_server *server = keyboard->server;
	keyboard_notify_modifiers(server, keyboard);
	/* fires after wlroots cleared the released key from the modifier
	 * state; the key event itself still carries the stale mask */
	if (server->switcher.active) {
		switcher_on_modifier_release(server,
			wlr_keyboard_get_modifiers(keyboard->wlr_keyboard));
	}
}

void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct guibux_server *server = keyboard->server;
	struct wlr_seat *seat = server->seat;

	screensaver_notify_activity(server);

	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	/* keybinds are defined with the unshifted (base) keysym; with Shift
	 * held the syms above are the shifted ones ('!'/'Q'/...), so match on
	 * level 0 to keep Shift+letter/number binds working on any layout.
	 * The actual (shifted) sym is tried too: on layouts like AZERTY the
	 * "normal" symbol is the shifted one ('1' needs Shift, base is '&'),
	 * so base-only matching would never fire Shift+number binds there */
	xkb_keysym_t base_sym = XKB_KEY_NoSymbol;
	if (keyboard->wlr_keyboard->keymap != NULL) {
		const xkb_keysym_t *base_syms;
		xkb_layout_index_t layout = xkb_state_key_get_layout(
			keyboard->wlr_keyboard->xkb_state, keycode);
		if (xkb_keymap_key_get_syms_by_level(
				keyboard->wlr_keyboard->keymap, keycode, layout, 0,
				&base_syms) > 0) {
			base_sym = base_syms[0];
		}
	}

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* wlroots synthesizes repeats as fresh PRESSED events; track
		 * per-keyboard state so keybinds don't re-fire while held
		 * (the launcher/switcher keep their repeats for typing) */
		bool repeat = false;
		for (int i = 0; i < keyboard->num_pressed; i++) {
			if (keyboard->pressed[i] == keycode) {
				repeat = true;
				break;
			}
		}
		if (!repeat && keyboard->num_pressed < 64) {
			keyboard->pressed[keyboard->num_pressed++] = keycode;
		}

		for (int i = 0; i < nsyms; i++) {
			if (server->lock.active) {
				/* the lock consumes every key: nothing may reach
				 * a client while the screen is locked */
				handled = lock_handle_key(server, syms[i]);
			} else if (server->launcher.active) {
				handled = launcher_handle_key(server, syms[i]);
			} else if (server->switcher.active) {
				handled = switcher_handle_key(server, syms[i]);
			} else if (server->help.active) {
				handled = help_handle_key(server, syms[i]);
			} else if (server->notify_panel.active) {
				handled = notify_panel_handle_key(server, syms[i]);
			} else if (server->outputs_panel.active) {
				handled = outputs_panel_handle_key(server, syms[i]);
			} else if (server->power_panel.active) {
				handled = power_panel_handle_key(server, syms[i]);
				if (!handled) {
					/* unhandled keys (e.g. Mod+p to toggle the
					 * panel) fall through to keybind dispatch */
					handled = handle_keybinding(server, base_sym, modifiers) ||
						handle_keybinding(server, syms[i], modifiers);
				}
			} else if (server->topbar_items_panel.active) {
				handled = topbar_items_panel_handle_key(server, syms[i]);
				if (!handled) {
					/* unhandled keys (e.g. Mod+l to toggle the
					 * panel) fall through to keybind dispatch */
					handled = handle_keybinding(server, base_sym, modifiers) ||
						handle_keybinding(server, syms[i], modifiers);
				}
			} else if (server->screenshot.active) {
				/* region-select owns the keyboard: Esc cancels, everything
				 * else is swallowed so no key reaches a client */
				if (syms[i] == XKB_KEY_Escape) {
					screenshot_region_cancel(server);
				}
				handled = true;
			} else if (repeat) {
				break;
			} else if (syms[i] == XKB_KEY_F12) {
				if (server->overview.active) {
					overview_hide(server);
				} else {
					overview_show(server);
				}
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86AudioRaiseVolume) {
				volume_change(server, false, 5);
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86AudioLowerVolume) {
				volume_change(server, false, -5);
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86AudioMute) {
				volume_toggle_mute(server, false);
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86AudioMicMute) {
				volume_toggle_mute(server, true);
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86MonBrightnessUp) {
				brightness_change(server, 5);
				handled = true;
			} else if (syms[i] == XKB_KEY_XF86MonBrightnessDown) {
				brightness_change(server, -5);
				handled = true;
			} else if (server->overview.active) {
				handled = overview_handle_key(server, syms[i]);
			} else {
				handled = handle_keybinding(server, base_sym, modifiers) ||
					handle_keybinding(server, syms[i], modifiers);
			}
			if (handled) {
				break;
			}
		}
	} else {
		for (int i = 0; i < keyboard->num_pressed; i++) {
			if (keyboard->pressed[i] == keycode) {
				keyboard->pressed[i] =
					keyboard->pressed[--keyboard->num_pressed];
				break;
			}
		}
	}

	if (!handled) {
		keyboard_notify_modifiers(server, keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

void server_new_keyboard(struct guibux_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct guibux_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap;
	if (server->xkb_layout != NULL || server->xkb_variant != NULL ||
			server->xkb_options != NULL) {
		struct xkb_rule_names rules = {0};
		rules.layout = server->xkb_layout;
		rules.variant = server->xkb_variant;
		rules.options = server->xkb_options;
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	} else {
		keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (!keymap) {
		wlr_log(WLR_ERROR, "failed to compile keymap for layout '%s'",
			server->xkb_layout ? server->xkb_layout : "default");
		xkb_context_unref(context);
		free(keyboard);
		return;
	}
	wlr_log(WLR_INFO, "keyboard: layout '%s' variant '%s' options '%s'",
		server->xkb_layout ? server->xkb_layout : "default",
		server->xkb_variant ? server->xkb_variant : "default",
		server->xkb_options ? server->xkb_options : "default");

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	keyboard_notify_modifiers(server, keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

void server_new_input(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, device);
		/* touchpad_tap: apply the config's tap-to-click setting to
		 * libinput pointer devices that support it */
		if (server->touchpad_tap != 0 &&
				wlr_input_device_is_libinput(device)) {
			struct libinput_device *li =
				wlr_libinput_get_device_handle(device);
			if (li != NULL &&
					libinput_device_has_capability(li,
						LIBINPUT_DEVICE_CAP_POINTER)) {
				libinput_device_config_tap_set_enabled(li,
					server->touchpad_tap == 1 ?
						LIBINPUT_CONFIG_TAP_ENABLED :
						LIBINPUT_CONFIG_TAP_DISABLED);
				wlr_log(WLR_INFO, "input: touchpad_tap %s on %s",
					server->touchpad_tap == 1 ? "enabled" : "disabled",
					device->name ? device->name : "(unknown)");
			}
		}
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

// ---------------------------------------------------------------------------
// text_input_v3 (IME): the compositor relays enter/leave to the focused
// surface. The IME client (fcitx, ibus) grabs the keyboard itself and
// commits the resulting strings back via the protocol; wlroots forwards
// them to the focused surface. The compositor never interprets the keys.
// ---------------------------------------------------------------------------

void text_input_handle_new(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, text_input_new);
	/* re-focus the IME on the currently focused surface: a client that
	 * creates its text input after mapping (or after a focus change)
	 * would otherwise stay silent */
	if (server->seat->pointer_state.focused_surface != NULL) {
		text_input_set_focus(server,
			server->seat->pointer_state.focused_surface);
	}
}

/* route the active text inputs to the surface under pointer focus; called
 * on every pointer enter/clear so the IME follows the focused window.
 * a text input may only be entered on a surface owned by the same client
 * (wlroots asserts this), so skip foreign text inputs — e.g. an IME text
 * input must not be entered on an XWayland surface */
void text_input_set_focus(struct guibux_server *server,
		struct wlr_surface *surface) {
	struct wl_client *client = surface != NULL
		? wl_resource_get_client(surface->resource) : NULL;
	struct wlr_text_input_v3 *text_input;
	wl_list_for_each(text_input, &server->text_input_manager->text_inputs,
			link) {
		/* a foreign client owns this text input: it can never take
		 * focus here, but it may still hold an old focus that must
		 * be released before the next matching enter */
		if (text_input->focused_surface != NULL &&
				wl_resource_get_client(text_input->resource) != client) {
			wlr_text_input_v3_send_leave(text_input);
			continue;
		}
		if (surface != NULL && text_input->current_enabled &&
				text_input->focused_surface != surface) {
			if (text_input->focused_surface != NULL) {
				wlr_text_input_v3_send_leave(text_input);
			}
			wlr_text_input_v3_send_enter(text_input, surface);
		} else if (surface == NULL && text_input->focused_surface != NULL) {
			wlr_text_input_v3_send_leave(text_input);
		}
	}
}

// ---------------------------------------------------------------------------
// relative_pointer_v1: raw, unaccelerated pointer deltas for FPS games.
// The manager emits events to every client that requested a relative
// pointer on the seat; the compositor keeps moving the regular cursor in
// parallel (the game locks the cursor via its own grab, not ours).
// ---------------------------------------------------------------------------

void relative_pointer_handle_new(struct wl_listener *listener, void *data) {
	/* nothing to do: wlroots tracks the relative pointers and the
	 * manager emits to all of them on send_relative_motion */
}

void relative_pointer_send_motion(struct guibux_server *server,
		struct wlr_seat *seat, uint32_t time_msec, double delta_x,
		double delta_y, double delta_unaccel_x, double delta_unaccel_y) {
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server->relative_pointer_manager, seat, time_msec, delta_x,
		delta_y, delta_unaccel_x, delta_unaccel_y);
}
