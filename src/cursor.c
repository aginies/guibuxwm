#include "guibuxwm.h"

void reset_cursor_mode(struct guibux_server *server) {
	server->cursor_mode = GUIBUX_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	double dx = server->cursor->x - server->grab_x;
	double dy = server->cursor->y - server->grab_y;
	wlr_scene_node_set_position(&toplevel->scene_tree->node, dx, dy);
	if (toplevel_is_xwayland(toplevel)) {
		/* motion events arrive at the pointer's polling rate (up to
		 * 1000/s); only notify the X11 app when the integer position
		 * actually changed */
		int nx = (int)dx, ny = (int)dy;
		if (nx != toplevel->xsurface->x || ny != toplevel->xsurface->y) {
			struct wlr_box geo;
			toplevel_get_geometry(toplevel, &geo);
			wlr_xwayland_surface_configure(toplevel->xsurface,
				nx, ny, geo.width, geo.height);
		}
	}
}

static void process_cursor_resize(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	struct wlr_box geo_box;
	toplevel_get_geometry(toplevel, &geo_box);

	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = geo_box.x;
	int new_right = geo_box.x + geo_box.width;
	int new_top = geo_box.y;
	int new_bottom = geo_box.y + geo_box.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	toplevel_get_geometry(toplevel, &geo_box);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box.x, new_top - geo_box.y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	toplevel_set_size(toplevel, new_width, new_height);
}

void process_cursor_motion(struct guibux_server *server, uint32_t time) {
	if (server->cursor_mode == GUIBUX_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == GUIBUX_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	struct guibux_output *o = NULL;
	int ws = 0;
	bool in_topbar = topbar_workspace_at(server, server->cursor->x, server->cursor->y,
		&o, &ws);
	if (in_topbar && ws != 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (in_topbar && topbar_network_at(server, o, server->cursor->x,
			server->cursor->y)) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	server->cursor_topbar_output = o;
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		if (server->focus_follow_mouse && toplevel &&
		    toplevel != server->last_ffm_toplevel &&
		    !server->launcher.active &&
		    !server->switcher.active &&
		    !server->overview.active &&
		    !server->help.active) {
			focus_toplevel(toplevel);
			server->last_ffm_toplevel = toplevel;
		}
	} else {
		wlr_seat_pointer_clear_focus(seat);
		server->last_ffm_toplevel = NULL;
	}
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	screensaver_notify_activity(server);
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	screensaver_notify_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	screensaver_notify_activity(server);
	if (server->launcher.active &&
			event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		server->button_consumed = event->button;
		launcher_hide(server);
		return;
	}
	if (server->overview.active) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			overview_click(server, server->cursor->x, server->cursor->y);
		} else {
			reset_cursor_mode(server);
		}
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct guibux_output *o = server->cursor_topbar_output;
		int ws = 0;
		if (o && topbar_workspace_at(server, server->cursor->x,
				server->cursor->y, NULL, &ws)) {
			/* check network indicator first */
			if (topbar_network_at(server, o, server->cursor->x,
					server->cursor->y)) {
				spawn_network_info(server);
				return;
			}
			/* check window labels */
			struct guibux_toplevel *win = NULL;
			win = topbar_win_at(o,
				server->cursor->x,
				server->cursor->y);
			if (win) {
				if (win->workspace != o->current_workspace) {
					switch_workspace(o, win->workspace);
				}
				uint32_t dt = event->time_msec - server->last_topbar_click_time;
				if (dt < 300 && server->last_topbar_click_win == win) {
					set_fullscreen(win, !win->is_fullscreen, NULL);
				} else {
					focus_toplevel(win);
				}
				server->last_topbar_click_time = event->time_msec;
				server->last_topbar_click_win = win;
				return;
			}
			if (ws != 0) {
				switch_workspace(o, ws);
			}
			return;
		}
	}
	/* X11 windows have no titlebar: Mod+drag moves them */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (toplevel != NULL && toplevel->managed &&
				toplevel_is_xwayland(toplevel) && kb != NULL &&
				(kb->modifiers.depressed & WLR_MODIFIER_LOGO)) {
			focus_toplevel(toplevel);
			if (toplevel->is_fullscreen) {
				set_fullscreen(toplevel, false, NULL);
			}
			server->button_consumed = event->button;
			begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
			return;
		}
	}
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* a press consumed by the WM (Mod+drag, launcher) was never
		 * forwarded; don't send the client a release for it */
		if (server->button_consumed != event->button) {
			wlr_seat_pointer_notify_button(server->seat,
				event->time_msec, event->button, event->state);
		}
		server->button_consumed = 0;
		reset_cursor_mode(server);
	} else {
		wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		focus_toplevel(toplevel);
		server->last_ffm_toplevel = NULL;
	}
}
void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	screensaver_notify_activity(server);
	wlr_seat_pointer_notify_axis(server->seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}
