#include "guibuxwm.h"

void reset_cursor_mode(struct guibux_server *server) {
	server->cursor_mode = GUIBUX_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

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

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
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
	int ws = 0;
	if (topbar_workspace_at(server, server->cursor->x, server->cursor->y,
			NULL, &ws) && ws != 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (topbar_network_at(server, server->cursor->x,
			server->cursor->y)) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

void server_cursor_button(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	if (server->launcher.active &&
			event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		launcher_hide(server);
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct guibux_output *o = NULL;
		int ws = 0;
		if (topbar_workspace_at(server, server->cursor->x,
				server->cursor->y, &o, &ws)) {
			/* check network indicator first */
			if (topbar_network_at(server, server->cursor->x,
					server->cursor->y)) {
				spawn_network_info(server);
				return;
			}
			/* check window labels */
			struct guibux_toplevel *win = NULL;
			if (o)
				win = topbar_win_at(o,
					server->cursor->x,
					server->cursor->y);
			if (win) {
				uint32_t dt = event->time_msec - server->last_topbar_click_time;
				if (dt < 300 && server->last_topbar_click_win == win) {
					set_fullscreen(win, !win->is_fullscreen);
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
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		focus_toplevel(toplevel);
	}
}

void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
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
