// guibuxwm - a simple Wayland window manager built on wlroots 0.20
//
// Derived from tinywl (wlroots, MIT license).
//
// Features:
//   - xdg-shell toplevels: focus, move, resize, fullscreen
//   - starts a terminal at launch (config `term`, GUIBUX_TERM env or -t
//     flag, default: gnome-terminal)
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
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_primary_selection_v1.h>

extern void spawn_terminal(struct guibux_server *server);

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);

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
	server.background_scale = BG_FILL;
	server.screensaver.timeout = 300;
	server.focus_follow_mouse = true;
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
		load_config(&server, config_path);
	}
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
	parse_output_placements(&server);

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

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);

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
	 * below is unaffected */
	server.xwayland = wlr_xwayland_create(server.wl_display,
		server.compositor, true);
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

	if (extra_outputs != NULL) {
		int n = atoi(extra_outputs) + 1;
		for (int i = 0; i < n; i++) {
			wlr_headless_add_output(server.backend, 1280, 720);
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

	server.sysinfo_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server.wl_display),
		sysinfo_tick, &server);
	wl_event_source_timer_update(server.sysinfo_timer, 5000);

	const char *topbar_test = getenv("GUIBUX_TEST_TOPBAR");
	if (topbar_test != NULL) {
		server.topbar_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			topbar_test_run, &server);
		wl_event_source_timer_update(server.topbar_test_timer, 500);
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

	const char *keybind_test = getenv("GUIBUX_TEST_KEYBIND");
	if (keybind_test != NULL) {
		server.keybind_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			keybind_test_run, &server);
		wl_event_source_timer_update(server.keybind_test_timer, 500);
	}

	spawn_terminal(&server);

	wlr_log(WLR_INFO, "guibuxwm running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);

	sysinfo_destroy(&server);

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
	if (server.sysinfo_timer != NULL) {
		wl_event_source_remove(server.sysinfo_timer);
	}
	if (server.topbar_test_timer != NULL) {
		wl_event_source_remove(server.topbar_test_timer);
	}
	if (server.workspace_test_timer != NULL) {
		wl_event_source_remove(server.workspace_test_timer);
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
	free(server.term_cmd);
	free(server.xkb_layout);
	free(server.xkb_variant);
	free(server.xkb_options);
	free(server.background_path);
	background_destroy_images(&server);
	return 0;
}
