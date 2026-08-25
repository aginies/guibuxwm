// guibuxwm - a simple Wayland window manager built on wlroots 0.20
//
// Derived from tinywl (wlroots, MIT license).
//
// Features:
//   - xdg-shell toplevels: focus, move, resize, fullscreen
//   - terminal command (config `term`, GUIBUX_TERM env or -t flag,
//     default: gnome-terminal) started by Mod+Return
//   - keyboard layout: -k flag, config `xkb_layout`, GUIBUX_XKB_LAYOUT or
//     XKB_DEFAULT_LAYOUT env (e.g. -k fr); variant and options via config
//   - multi-monitor: new windows open on the output under the cursor,
//     windows move between monitors with Mod+Shift+Left/Right,
//     monitor arrangement via GUIBUX_OUTPUTS="NAME@XxY,NAME@XxY"
//   - topbar per monitor: monitor letter (A, B, C, ...) on the left, date
//     and time on the right (updates every second)
//   - workspaces per monitor (4, numbered 1 2 3 4): Mod+1..4 switch,
//     Mod+Shift+1..4 move a window; workspace numbers shown in the topbar
//     (current highlighted, clickable)
//   - keybindings (Mod = Super), all configurable via the config file:
//       Mod+Return            start a new terminal
//       Mod+q                 close focused window
//       Mod+f                 toggle fullscreen
//       Mod+t                 cycle tile mode (free / split / main+stack)
//       Mod+e                 command box: type a command, Enter runs it
//       Mod+Tab               cycle focus
//       Mod+1..4              switch to workspace 1..4 on the focused monitor
//       Mod+Shift+1..4        move focused window to workspace 1..4
//       Mod+Shift+Left/Right  move window to previous/next monitor
//       Mod+Shift+q           quit
//       Alt+Escape            quit
//       Mod+Alt+Escape        quit
//   - config file: -c flag, GUIBUX_CONFIG env or ~/.config/guibuxwm/config
//     (keybinds, terminal, keyboard layout/variant/options, colors)
//
// Build:
//   meson setup build && ninja -C build
// Run:
//   ./build/guibuxwm

#include "guibuxwm.h"
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#if GUIBUX_HAS_VULKAN
#include <wlr/render/vulkan.h>
#endif
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_primary_selection_v1.h>

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);
	/* SIGUSR1 (guibuxwm-output live re-apply) and SIGHUP (config
	 * reload) are read via signalfd on the main thread; block them
	 * process-wide so every worker thread inherits the blocked mask
	 * and the kernel never delivers them to a thread that would take
	 * the default action (terminate / hang up) */
	sigset_t usr1_mask;
	sigemptyset(&usr1_mask);
	sigaddset(&usr1_mask, SIGUSR1);
	sigaddset(&usr1_mask, SIGHUP);
	sigaddset(&usr1_mask, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &usr1_mask, NULL);
	/* spawned children (terminals, launcher commands) are reaped by a
	 * targeted SIGCHLD handler instead of accumulating as zombies;
	 * SIG_IGN is not an option: it is inherited by Xwayland and breaks
	 * the X server's waitpid() on xkbcomp */
	signal(SIGCHLD, spawn_sigchld_handler);

	char *term_cmd = NULL;
	char *xkb_layout = NULL;
	char *config_path = NULL;
	int c;
	while ((c = getopt(argc, argv, "t:k:c:h")) != -1) {
		switch (c) {
		case 't':
			term_cmd = optarg;
			break;
		case 'k':
			xkb_layout = optarg;
			break;
		case 'c':
			config_path = optarg;
			break;
		default:
			printf("Usage: %s [-t terminal command] [-k keyboard layout] [-c config file]\n", argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-t terminal command] [-k keyboard layout] [-c config file]\n", argv[0]);
		return 1;
	}

	struct guibux_server server = {0};
	server.color_bg = DEFAULT_COLOR_BG;
	server.color_border = DEFAULT_COLOR_BORDER;
	server.color_highlight = DEFAULT_COLOR_HIGHLIGHT;
	server.color_text = DEFAULT_COLOR_TEXT;
	server.color_dim = DEFAULT_COLOR_DIM;
	server.color_topbar_bg = DEFAULT_COLOR_TOPBAR_BG;
	server.color_topbar_text = DEFAULT_COLOR_TOPBAR_TEXT;
	server.topbar_height = DEFAULT_TOPBAR_H;
	server.topbar_font_size = DEFAULT_TOPBAR_FONT_SIZE;
	server.topbar_win_pad = DEFAULT_TOPBAR_WIN_PAD;
	server.topbar_items[0] = TOPBAR_ITEM_NETWORK;
	server.topbar_items[1] = TOPBAR_ITEM_VOLUME;
	server.topbar_items[2] = TOPBAR_ITEM_MIC;
	server.topbar_items[3] = TOPBAR_ITEM_BATTERY;
	server.topbar_items[4] = TOPBAR_ITEM_NOTIFICATIONS;
	server.topbar_items[5] = TOPBAR_ITEM_CLOCK;
	server.topbar_item_count = TOPBAR_ITEMS_MAX;
	server.background_scale = BG_FILL;
	server.screensaver.timeout = 300;
	server.focus_follow_mouse = true;
	server.effects_enabled = true;
	server.effects_duration_ms = 200;
	server.window_open_effect = OPEN_EFFECT_SCALE;
	server.notify_effect_slide = true;
	server.osd_enabled = true;
	server.osd_timeout_ms = 1500;
	server.restore_positions = true;
	server.renderer_name = strdup("auto");
	keybinds_defaults(&server);

	if (config_path == NULL) {
		config_path = getenv("GUIBUX_CONFIG");
	}
	if (config_path == NULL) {
		const char *home = getenv("HOME");
		if (home != NULL) {
			static char default_config[PATH_MAX];
			snprintf(default_config, sizeof(default_config),
				"%s/.config/guibuxwm/config", home);
			config_path = default_config;
		}
	}
	if (config_path != NULL) {
		server.config_path = strdup(config_path);
		load_config(&server, config_path);
	}
	/* the build option can force effects off regardless of config */
	effects_init(&server);
	background_load_images(&server);

	if (server.term_cmd == NULL) {
		const char *env_term = getenv("GUIBUX_TERM");
		server.term_cmd = strdup(env_term ? env_term : "gnome-terminal");
	}
	if (server.xkb_layout == NULL) {
		const char *env_layout = getenv("GUIBUX_XKB_LAYOUT")
			? getenv("GUIBUX_XKB_LAYOUT") : getenv("XKB_DEFAULT_LAYOUT");
		if (env_layout != NULL) {
			server.xkb_layout = strdup(env_layout);
		}
	}
	if (term_cmd != NULL) {
		free(server.term_cmd);
		server.term_cmd = strdup(term_cmd);
	}
	if (xkb_layout != NULL) {
		free(server.xkb_layout);
		server.xkb_layout = strdup(xkb_layout);
	}
	/* config `outputs` wins over the GUIBUX_OUTPUTS env; both default
	 * to auto-arranging every connected monitor. The effective spec is
	 * remembered: outputs_apply falls back to it when the config file
	 * has no `outputs` line */
	const char *outputs_spec = server.outputs_spec;
	if (outputs_spec == NULL) {
		outputs_spec = getenv("GUIBUX_OUTPUTS");
	}
	if (outputs_spec != NULL) {
		server.outputs_env_spec = strdup(outputs_spec);
	}
	parse_output_placements(&server, outputs_spec);
	/* term_cmd is final here (config + env + -t flag): derive the
	 * terminal app_id and load saved window positions */
	restore_derive_terminal_id(&server);
	restore_load(&server);

	server.wl_display = wl_display_create();
	const char *extra_outputs = getenv("GUIBUX_TEST_EXTRA_OUTPUTS");
	if (extra_outputs != NULL) {
		server.backend = wlr_headless_backend_create(
			wl_display_get_event_loop(server.wl_display));
	} else {
		server.backend = wlr_backend_autocreate(
			wl_display_get_event_loop(server.wl_display), NULL);
	}
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/* config `renderer` selects the wlroots render backend; an explicit
	 * WLR_RENDERER env always wins (tests and power users rely on it) */
	if (strcmp(server.renderer_name, "auto") != 0 &&
			getenv("WLR_RENDERER") == NULL) {
		setenv("WLR_RENDERER", server.renderer_name, 0);
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	const char *renderer_type = "pixman";
	if (wlr_renderer_is_gles2(server.renderer)) {
		renderer_type = "gles2";
	}
#if GUIBUX_HAS_VULKAN
	else if (wlr_renderer_is_vk(server.renderer)) {
		renderer_type = "vulkan";
	}
#endif
	wlr_log(WLR_INFO, "renderer: %s", renderer_type);

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	server.screencopy = wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	server.xdg_output_manager = wlr_xdg_output_manager_v1_create(
		server.wl_display, server.output_layout);

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	launcher_init(&server);
	screensaver_init(&server);
	server.screensaver.active = server.screensaver.timeout > 0;

	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
	server.xdg_activation_request.notify = xdg_activation_handle_request;
	wl_signal_add(&server.xdg_activation->events.request_activate,
		&server.xdg_activation_request);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = GUIBUX_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	/* lazy: the Xwayland process only starts when the first X11 client
	 * connects; display_name is available immediately, so DISPLAY setup
	 * below is unaffected. Disabled in tests unless the xwayland test
	 * asks for it: the headless test environment may have no usable X
	 * display and the X server process would outlive the compositor */
	if (getenv("GUIBUX_TEST_XWAYLAND") != NULL ||
	    getenv("GUIBUX_TEST_EXTRA_OUTPUTS") == NULL) {
		server.xwayland = wlr_xwayland_create(server.wl_display,
			server.compositor, true);
	}
	if (server.xwayland != NULL) {
		wlr_xwayland_set_seat(server.xwayland, server.seat);
		wlr_log(WLR_INFO, "xwayland: DISPLAY=%s",
			server.xwayland->display_name);
		server.new_xwayland_surface.notify = server_new_xwayland_surface;
		wl_signal_add(&server.xwayland->events.new_surface,
			&server.new_xwayland_surface);
	} else {
		wlr_log(WLR_ERROR, "failed to create xwayland, X11 apps disabled");
	}
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);
	server.request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	sysinfo_init(&server);
	/* the D-Bus worker may wake the main loop as soon as it starts, so
	 * the pipe must exist before the worker thread does */
	server.notify_pipe[0] = server.notify_pipe[1] = -1;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0,
			server.notify_pipe) == 0) {
		server.notify_pipe_source = wl_event_loop_add_fd(
			wl_display_get_event_loop(server.wl_display),
			server.notify_pipe[0], WL_EVENT_READABLE,
			notify_new_readable, &server);
	}
	notify_init(&server);

	/* SIGUSR1 (sent by the guibuxwm-output tool): re-apply the outputs
	 * config live */
	outputs_apply_init(&server);

	/* SIGHUP: reload the config file live (keybinds, colors, topbar,
	 * backgrounds, outputs, screensaver) */
	{
		sigset_t hup_mask;
		sigemptyset(&hup_mask);
		sigaddset(&hup_mask, SIGHUP);
		int sfd = signalfd(-1, &hup_mask, SFD_NONBLOCK | SFD_CLOEXEC);
		if (sfd < 0) {
			wlr_log(WLR_ERROR, "config: signalfd(SIGHUP) failed: %m");
		} else {
			server.config_signal_source = wl_event_loop_add_fd(
				wl_display_get_event_loop(server.wl_display), sfd,
				WL_EVENT_READABLE, config_signal_readable, &server);
		}
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	/* X11 clients (e.g. PrusaSlicer flatpak) connect to the Xwayland
	 * display started above; spawned clients inherit this DISPLAY */
	if (server.xwayland != NULL) {
		setenv("DISPLAY", server.xwayland->display_name, true);
	}
	/* complete the session env (XDG_SESSION_TYPE, XDG_CURRENT_DESKTOP)
	 * and import DISPLAY/WAYLAND_DISPLAY into the systemd user manager
	 * so xdg-desktop-portal can open URLs in the default browser */
	setup_session_environment(&server);

	if (extra_outputs != NULL) {
		int n = atoi(extra_outputs) + 1;
		for (int i = 0; i < n; i++) {
			wlr_headless_add_output(server.backend, 1280, 720);
		}
	}

	/* a configured output that never showed up (typo'd name, monitor not
	 * connected at start) must not disable the real ones: those were
	 * auto-arranged, so this is a warning, not a failure */
	for (int i = 0; i < server.num_placements; i++) {
		if (!server.placements[i].used && !server.placements[i].disabled) {
			wlr_log(WLR_ERROR, "outputs: '%s' not connected, ignoring",
				server.placements[i].name);
		}
	}

	const char *launcher_test_cmd = getenv("GUIBUX_TEST_LAUNCHER_CMD");
	if (launcher_test_cmd != NULL) {
		server.launcher.test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			launcher_test_run, &server);
		wl_event_source_timer_update(server.launcher.test_timer, 500);
	}

	const char *tile_test_mode = getenv("GUIBUX_TEST_TILE_MODE");
	if (tile_test_mode != NULL) {
		server.tile_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			tile_test_run, &server);
		wl_event_source_timer_update(server.tile_test_timer, 500);
	}

	const char *overview_test = getenv("GUIBUX_TEST_OVERVIEW");
	if (overview_test != NULL) {
		test_seat_add_keyboard(&server);
		server.overview_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			overview_test_run, &server);
		wl_event_source_timer_update(server.overview_test_timer, 2000);
	}

	server.topbar_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server.wl_display),
		topbar_tick, &server);
	wl_event_source_timer_update(server.topbar_timer, 500);

	server.notify_autohide_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server.wl_display),
		notify_autohide_run, &server);

	const char *topbar_test = getenv("GUIBUX_TEST_TOPBAR");
	if (topbar_test != NULL) {
		server.topbar_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			topbar_test_run, &server);
		wl_event_source_timer_update(server.topbar_test_timer, 500);
	}

	const char *audio_test = getenv("GUIBUX_TEST_AUDIO");
	if (audio_test != NULL) {
		/* fire after the first sysinfo audio poll (~5s) so the
		 * indicators have been rendered */
		server.audio_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			audio_test_run, &server);
		wl_event_source_timer_update(server.audio_test_timer, 6500);
	}

	const char *battery_test = getenv("GUIBUX_TEST_BATTERY");
	if (battery_test != NULL) {
		/* fire after the first sysinfo UPower poll (~5s) so the
		 * battery indicator has been rendered */
		server.battery_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			battery_test_run, &server);
		wl_event_source_timer_update(server.battery_test_timer, 6500);
	}

	const char *tooltip_test = getenv("GUIBUX_TEST_TOOLTIP");
	if (tooltip_test != NULL) {
		/* the sysinfo worker seeds a fake battery at startup, so the
		 * indicator is rendered by the first topbar tick (~0.5s) */
		server.tooltip_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			tooltip_test_run, &server);
		wl_event_source_timer_update(server.tooltip_test_timer, 2000);
	}

	const char *osd_test = getenv("GUIBUX_TEST_OSD");
	if (osd_test != NULL) {
		server.osd_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			osd_test_run, &server);
		wl_event_source_timer_update(server.osd_test_timer, 2000);
	}

	const char *power_test = getenv("GUIBUX_TEST_POWER");
	if (power_test != NULL) {
		server.power_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			power_panel_test_run, &server);
		wl_event_source_timer_update(server.power_test_timer, 2000);
	}

	const char *topbar_items_test = getenv("GUIBUX_TEST_TOPBAR_ITEMS");
	if (topbar_items_test != NULL) {
		server.topbar_items_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			topbar_items_panel_test_run, &server);
		wl_event_source_timer_update(server.topbar_items_test_timer, 2000);
	}

	const char *notify_test = getenv("GUIBUX_TEST_NOTIFY");
	if (notify_test != NULL) {
		server.notify_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			notify_test_run, &server);
		wl_event_source_timer_update(server.notify_test_timer, 500);
	}

	const char *scroll_test = getenv("GUIBUX_TEST_SCROLL");
	if (scroll_test != NULL) {
		/* after the first audio poll: the VOL indicator must be
		 * rendered for the scroll hit area to exist */
		server.scroll_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			scroll_test_run, &server);
		wl_event_source_timer_update(server.scroll_test_timer, 6500);
	}

	const char *altdrag_test = getenv("GUIBUX_TEST_ALTDRAG");
	if (altdrag_test != NULL) {
		test_seat_add_keyboard(&server);
		server.altdrag_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			alt_drag_test_run, &server);
		wl_event_source_timer_update(server.altdrag_test_timer, 2000);
	}

	const char *xmondrag_test = getenv("GUIBUX_TEST_XMONDRAG");
	if (xmondrag_test != NULL) {
		test_seat_add_keyboard(&server);
		server.xmondrag_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			xmondrag_test_run, &server);
		wl_event_source_timer_update(server.xmondrag_test_timer, 2000);
	}

	const char *psel_test = getenv("GUIBUX_TEST_PRIMARY_SELECTION");
	if (psel_test != NULL) {
		server.psel_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			psel_test_run, &server);
		wl_event_source_timer_update(server.psel_test_timer, 1500);
	}

	const char *workspace_test = getenv("GUIBUX_TEST_WORKSPACES");
	if (workspace_test != NULL) {
		test_seat_add_keyboard(&server);
		server.workspace_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			workspace_test_run, &server);
		wl_event_source_timer_update(server.workspace_test_timer, 2000);
	}

	const char *outputs_test = getenv("GUIBUX_TEST_OUTPUTS");
	if (outputs_test != NULL) {
		server.outputs_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			outputs_test_run, &server);
		wl_event_source_timer_update(server.outputs_test_timer, 2000);
	}

	const char *outputs_panel_test = getenv("GUIBUX_TEST_OUTPUTS_PANEL");
	if (outputs_panel_test != NULL) {
		server.outputs_panel_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			outputs_panel_test_run, &server);
		wl_event_source_timer_update(server.outputs_panel_test_timer, 2000);
	}

	const char *keybind_test = getenv("GUIBUX_TEST_KEYBIND");
	if (keybind_test != NULL) {
		server.keybind_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			keybind_test_run, &server);
		wl_event_source_timer_update(server.keybind_test_timer, 500);
	}

	const char *resize_test = getenv("GUIBUX_TEST_RESIZE");
	if (resize_test != NULL) {
		server.resize_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			resize_test_run, &server);
		wl_event_source_timer_update(server.resize_test_timer, 3000);
	}

	const char *effects_test = getenv("GUIBUX_TEST_EFFECTS");
	if (effects_test != NULL) {
		server.effects_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			effects_test_run, &server);
		wl_event_source_timer_update(server.effects_test_timer, 1000);
	}

	const char *quit_test = getenv("GUIBUX_TEST_QUIT");
	if (quit_test != NULL) {
		server.quit_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			quit_test_run, &server);
		int delay = atoi(quit_test);
		wl_event_source_timer_update(server.quit_test_timer,
			delay > 0 ? delay : 2000);
	}

	const char *global_topbar_test = getenv("GUIBUX_TEST_GLOBAL_TOPBAR");
	if (global_topbar_test != NULL) {
		server.global_topbar_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			global_topbar_test_run, &server);
		wl_event_source_timer_update(server.global_topbar_test_timer, 4000);
	}

	wlr_log(WLR_INFO, "guibuxwm running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);

	/* remember the final position of every still-mapped window before
	 * the clients (and with them the windows) are destroyed */
	restore_save_all(&server);

	sysinfo_destroy(&server);
	notify_destroy(&server);
	tooltip_destroy(&server);
	preview_destroy(&server);
	osd_destroy(&server);
	power_panel_destroy(&server);
	topbar_items_panel_destroy(&server);
	screensaver_destroy(&server);
	if (server.notify_autohide_timer != NULL) {
		wl_event_source_remove(server.notify_autohide_timer);
	}
	if (server.notify_pipe_source != NULL) {
		wl_event_source_remove(server.notify_pipe_source);
	}
	if (server.outputs_signal_source != NULL) {
		wl_event_source_remove(server.outputs_signal_source);
	}
	if (server.config_signal_source != NULL) {
		wl_event_source_remove(server.config_signal_source);
	}
	if (server.notify_pipe[0] >= 0) {
		close(server.notify_pipe[0]);
		close(server.notify_pipe[1]);
	}

	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	if (server.xwayland != NULL) {
		wl_list_remove(&server.new_xwayland_surface.link);
	}

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);

	wl_list_remove(&server.new_output.link);

	if (server.topbar_timer != NULL) {
		wl_event_source_remove(server.topbar_timer);
	}
	if (server.topbar_test_timer != NULL) {
		wl_event_source_remove(server.topbar_test_timer);
	}
	if (server.audio_test_timer != NULL) {
		wl_event_source_remove(server.audio_test_timer);
	}
	if (server.battery_test_timer != NULL) {
		wl_event_source_remove(server.battery_test_timer);
	}
	if (server.tooltip_test_timer != NULL) {
		wl_event_source_remove(server.tooltip_test_timer);
	}
	if (server.osd_test_timer != NULL) {
		wl_event_source_remove(server.osd_test_timer);
	}
	if (server.power_test_timer != NULL) {
		wl_event_source_remove(server.power_test_timer);
	}
	if (server.topbar_items_test_timer != NULL) {
		wl_event_source_remove(server.topbar_items_test_timer);
	}
	if (server.scroll_test_timer != NULL) {
		wl_event_source_remove(server.scroll_test_timer);
	}
	if (server.altdrag_test_timer != NULL) {
		wl_event_source_remove(server.altdrag_test_timer);
	}
	if (server.xmondrag_test_timer != NULL) {
		wl_event_source_remove(server.xmondrag_test_timer);
	}
	if (server.workspace_test_timer != NULL) {
		wl_event_source_remove(server.workspace_test_timer);
	}
	if (server.outputs_test_timer != NULL) {
		wl_event_source_remove(server.outputs_test_timer);
	}
	if (server.keybind_test_timer != NULL) {
		wl_event_source_remove(server.keybind_test_timer);
	}
	if (server.overview_test_timer != NULL) {
		wl_event_source_remove(server.overview_test_timer);
	}
	if (server.psel_test_timer != NULL) {
		wl_event_source_remove(server.psel_test_timer);
	}
	if (server.resize_test_timer != NULL) {
		wl_event_source_remove(server.resize_test_timer);
	}
	if (server.notify_test_timer != NULL) {
		wl_event_source_remove(server.notify_test_timer);
	}
	if (server.effects_test_timer != NULL) {
		wl_event_source_remove(server.effects_test_timer);
	}
	if (server.quit_test_timer != NULL) {
		wl_event_source_remove(server.quit_test_timer);
	}
	if (server.global_topbar_test_timer != NULL) {
		wl_event_source_remove(server.global_topbar_test_timer);
	}

	launcher_hide(&server);
	launcher_free_commands(&server.launcher);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	if (server.xwayland != NULL) {
		wlr_xwayland_destroy(server.xwayland);
	}
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_output_layout_destroy(server.output_layout);
	wlr_allocator_destroy(server.allocator);
	if (server.launcher.shm_alloc != NULL) {
		wlr_allocator_destroy(server.launcher.shm_alloc);
	}
	if (server.launcher.ft != NULL) {
		FT_Done_FreeType(server.launcher.ft);
	}
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.wl_display);
	restore_free(&server);
	free(server.terminal_app_id);
	free(server.term_cmd);
	free(server.outputs_spec);
	free(server.outputs_env_spec);
	free(server.config_path);
	free(server.renderer_name);
	free(server.xkb_layout);
	free(server.xkb_variant);
	free(server.xkb_options);
	free(server.background_path);
	background_destroy_images(&server);
	return 0;
}
