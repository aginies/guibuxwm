#include "guibuxwm.h"

/* pointer travel before an overview press becomes a drag */
#define OVERVIEW_DRAG_THRESHOLD 5

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
	/* grab_geobox is the window box in absolute layout coordinates
	 * (captured in begin_interactive); border_x/border_y below are
	 * absolute too, so all math stays in one coordinate space */
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

	struct wlr_box geo_box;
	toplevel_get_geometry(toplevel, &geo_box);
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box.x, new_top - geo_box.y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	toplevel_set_size(toplevel, new_width, new_height);
}

void process_cursor_motion(struct guibux_server *server, uint32_t time) {
	/* an overview press becomes a drag once the pointer moves far
	 * enough; the window then follows the cursor until release */
	if (server->overview.active && server->overview.drag_toplevel != NULL &&
			!server->overview.drag_active) {
		double dx = server->cursor->x - server->overview.drag_press_x;
		double dy = server->cursor->y - server->overview.drag_press_y;
		if (dx * dx + dy * dy >=
				OVERVIEW_DRAG_THRESHOLD * OVERVIEW_DRAG_THRESHOLD) {
			struct guibux_toplevel *t = server->overview.drag_toplevel;
			server->overview.drag_active = true;
			begin_interactive(t, GUIBUX_CURSOR_MOVE, 0);
			/* keep the dragged window above the dim rect */
			wlr_scene_node_raise_to_top(&t->scene_tree->node);
			topbar_raise_all(server);
		}
	}
	if (server->overview.active) {
		/* highlight the workspace column cell the dragged window
		 * would be dropped on (no-op when not dragging) */
		overview_update_hover(server);
	}
	if (server->cursor_mode == GUIBUX_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == GUIBUX_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}
	/* the auto-hidden panel must not close while the user reads it */
	if (server->notify_panel.active &&
			notify_panel_contains(server, server->cursor->x,
				server->cursor->y)) {
		notify_autohide_start(server);
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
	} else if (in_topbar && topbar_audio_at(server, o, server->cursor->x,
			server->cursor->y) != 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (in_topbar && topbar_battery_at(server, o, server->cursor->x,
			server->cursor->y)) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (in_topbar && topbar_network_at(server, o, server->cursor->x,
			server->cursor->y)) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (in_topbar && topbar_notif_at(server, o, server->cursor->x,
			server->cursor->y)) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (in_topbar && topbar_win_at(o, server->cursor->x,
			server->cursor->y) != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (server->notify_panel.active &&
			(notify_panel_row_at(server, server->cursor->x,
				server->cursor->y) != 0 ||
			 notify_panel_clear_at(server, server->cursor->x,
				server->cursor->y))) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (server->power_panel.active &&
			power_panel_action_at(server, server->cursor->x,
				server->cursor->y) >= 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (server->topbar_items_panel.active &&
			topbar_items_panel_row_at(server, server->cursor->x,
				server->cursor->y) >= 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	server->cursor_topbar_output = o;
	/* battery indicator tooltip: arm/disarm the hover on every move */
	tooltip_update_hover(server, time);
	/* window preview: arm/disarm the hover on every move */
	preview_update_hover(server, time);
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
		if (server->focus_follow_mouse && toplevel &&
		    toplevel != server->last_ffm_toplevel &&
		    !server->launcher.active &&
		    !server->switcher.active &&
		    !server->overview.active &&
		    !server->help.active &&
		    !server->notify_panel.active) {
			focus_toplevel(toplevel, false);
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
			struct guibux_toplevel *t = overview_window_at(server,
				server->cursor->x, server->cursor->y);
			if (t != NULL) {
				/* potential drag: the window is only grabbed
				 * once the pointer moves past a threshold */
				server->overview.drag_toplevel = t;
				server->overview.drag_active = false;
				server->overview.drag_press_x = server->cursor->x;
				server->overview.drag_press_y = server->cursor->y;
			} else {
				overview_click_empty(server, server->cursor->x,
					server->cursor->y);
			}
		} else {
			overview_button_release(server);
		}
		return;
	}
	if (server->power_panel.active) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			int idx = power_panel_action_at(server, server->cursor->x,
				server->cursor->y);
			if (idx >= 0) {
				power_panel_select(server, idx);
			} else {
				power_panel_hide(server);
			}
		}
		server->button_consumed = event->button;
		return;
	}
	if (server->topbar_items_panel.active) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			int idx = topbar_items_panel_row_at(server, server->cursor->x,
				server->cursor->y);
			if (idx >= 0) {
				topbar_items_panel_toggle(server, idx);
			} else {
				topbar_items_panel_hide(server);
			}
		}
		server->button_consumed = event->button;
		return;
	}
	if (server->notify_panel.active) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (notify_panel_clear_at(server, server->cursor->x,
					server->cursor->y)) {
				notify_clear(&server->notify);
				if (notify_count(&server->notify) == 0) {
					notify_panel_hide(server);
				} else {
					notify_panel_render(server);
					/* the panel stays open: give it a fresh delay */
					notify_autohide_start(server);
				}
			} else {
				uint32_t id = notify_panel_row_at(server,
					server->cursor->x, server->cursor->y);
				if (id != 0) {
					/* click a row: focus the window that sent the
					 * notification (best-effort app match), dismiss
					 * the notification and close the panel */
					struct guibux_notification item;
					struct guibux_toplevel *win = NULL;
					if (notify_get_by_id(&server->notify, id, &item)) {
						win = toplevel_for_app(server, item.app_name);
					}
					if (win != NULL) {
						struct guibux_output *wo = guibux_output_for(
							server, toplevel_output_for(win));
						if (wo != NULL &&
								win->workspace != wo->current_workspace) {
							switch_workspace(wo, win->workspace);
						}
						focus_toplevel(win, true);
					}
					notify_close(&server->notify, id);
					notify_panel_hide(server);
				} else {
					notify_panel_hide(server);
				}
			}
		}
		server->button_consumed = event->button;
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct guibux_output *o = server->cursor_topbar_output;
		int ws = 0;
		if (o && topbar_workspace_at(server, server->cursor->x,
				server->cursor->y, NULL, &ws)) {
			/* check audio indicators first: left click toggles the
			 * mute, right click opens the mixer (pavucontrol) */
			int audio = topbar_audio_at(server, o, server->cursor->x,
					server->cursor->y);
			if (audio != 0) {
				/* Wayland button codes are Linux input codes:
				 * BTN_LEFT = 272, BTN_RIGHT = 273 */
				if (event->button == 272) {
					volume_toggle_mute(server, audio == 2);
				} else if (event->button == 273) {
					spawn_mixer(server);
				}
				return;
			}
			/* check network indicator */
			if (topbar_network_at(server, o, server->cursor->x,
					server->cursor->y)) {
				if (event->button == 273) {
					spawn_network_info(server);
				}
				return;
			}
			/* check notification indicator: only while there are
			 * unread notifications (the cell is reserved but empty
			 * otherwise) */
			if (notify_count(&server->notify) > 0 &&
					topbar_notif_at(server, o, server->cursor->x,
						server->cursor->y)) {
				notify_panel_show(server, o->wlr_output);
				return;
			}
			/* check window labels */
			struct guibux_toplevel *win = NULL;
			win = topbar_win_at(o,
				server->cursor->x,
				server->cursor->y);
			if (win) {
				/* the list is global (own + other monitors): switch to
				 * the window's own monitor's workspace when it differs
				 * from the clicked bar's monitor */
				struct wlr_output *win_out =
					toplevel_output_for(win);
				struct guibux_output *win_o = win_out != NULL
					? guibux_output_for(server, win_out) : NULL;
				if (win_o != NULL && win_o != o) {
					if (win->workspace != win_o->current_workspace) {
						switch_workspace(win_o, win->workspace);
					}
				} else if (win->workspace != o->current_workspace) {
					switch_workspace(o, win->workspace);
				}
				uint32_t dt = event->time_msec - server->last_topbar_click_time;
				if (dt < 300 && server->last_topbar_click_win == win) {
					set_fullscreen(win, !win->is_fullscreen, NULL);
				} else {
					focus_toplevel(win, true);
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
	/* X11 windows have no titlebar: Mod+drag moves them.
	 * Alt+left-drag moves any managed window (GNOME-style); Ctrl held
	 * means AltGr on many layouts, not a plain Alt. The switcher
	 * (Alt+Tab) is still up while Alt is held: dismiss it first */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		bool alt_drag = toplevel != NULL && toplevel->managed &&
			kb != NULL && event->button == 272 && /* BTN_LEFT */
			(kb->modifiers.depressed & WLR_MODIFIER_ALT) &&
			!(kb->modifiers.depressed & WLR_MODIFIER_CTRL);
		bool mod_drag = toplevel != NULL && toplevel->managed &&
			toplevel_is_xwayland(toplevel) && kb != NULL &&
			(kb->modifiers.depressed & WLR_MODIFIER_LOGO);
		if (alt_drag || mod_drag) {
			if (server->switcher.active) {
				switcher_hide(server);
			}
			focus_toplevel(toplevel, true);
			if (toplevel->is_fullscreen) {
				set_fullscreen(toplevel, false, NULL);
			}
			server->button_consumed = event->button;
			begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
			return;
		}
	}
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if ((server->cursor_mode == GUIBUX_CURSOR_MOVE ||
				server->cursor_mode == GUIBUX_CURSOR_RESIZE) &&
				server->grabbed_toplevel != NULL) {
			struct guibux_toplevel *t = server->grabbed_toplevel;
			/* toplevel_output_for() would return the stored output,
			 * which is exactly what may have just changed: ask for
			 * the position instead; NULL (center over no output)
			 * keeps the window on its current output */
			struct wlr_output *new_wlr = toplevel_output_at_position(t);
			struct guibux_output *new_o = guibux_output_for(server, new_wlr);
			if (new_o != NULL && t->output != new_o) {
				struct guibux_output *old_o = t->output;
				t->output = new_o;
				t->workspace = new_o->current_workspace;
				/* the window list changed on both bars */
				topbar_mark_dirty(old_o);
				topbar_mark_dirty(new_o);
				if (new_o->tile_modes[new_o->current_workspace] != GUIBUX_TILE_FREE) {
					retile_output(new_o);
				}
				if (old_o != NULL &&
						old_o->tile_modes[old_o->current_workspace] != GUIBUX_TILE_FREE) {
					retile_output(old_o);
				}
			}
		}
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
		focus_toplevel(toplevel, true);
		server->last_ffm_toplevel = NULL;
	}
}
void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	screensaver_notify_activity(server);
	/* scroll over a topbar audio indicator adjusts that volume
	 * instead of forwarding the event to a client */
	struct guibux_output *o = server->cursor_topbar_output;
	int audio = o ? topbar_audio_at(server, o, server->cursor->x,
		server->cursor->y) : 0;
	if (audio != 0) {
		int step = event->delta_discrete != 0 ? event->delta_discrete :
			(event->delta > 0 ? 1 : -1);
		/* positive delta = scroll down; negative = scroll up = volume up */
		volume_change(server, audio == 2, step > 0 ? -1 : 1);
		return;
	}
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
