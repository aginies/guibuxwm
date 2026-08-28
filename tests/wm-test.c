#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <stdlib.h>
#include <time.h>

void test_seat_add_keyboard(struct guibux_server *server) {
	if (wlr_seat_get_keyboard(server->seat) != NULL) {
		return;
	}
	struct wlr_keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	wl_signal_init(&kb->base.events.destroy);
	wl_signal_init(&kb->events.key);
	wl_signal_init(&kb->events.modifiers);
	wl_signal_init(&kb->events.keymap);
	wl_signal_init(&kb->events.repeat_info);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (map != NULL && wlr_keyboard_set_keymap(kb, map)) {
		wlr_seat_set_keyboard(server->seat, kb);
	}
	if (map != NULL) {
		xkb_keymap_unref(map);
	}
	xkb_context_unref(ctx);
}

int workspace_test_run(void *data) {
	struct guibux_server *server = data;
	int ws = atoi(getenv("GUIBUX_TEST_WORKSPACES"));
	if (ws < 1 || ws > NUM_WORKSPACES) {
		ws = 2;
	}
	struct guibux_toplevel *t;
	int n_outputs = 0, n_toplevels = 0;

	wl_list_for_each(t, &server->toplevels, link) {
		n_toplevels++;
	}
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		n_outputs++;
		if (o->current_workspace != 1) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL initial workspace "
				"on %s (got %d, want 1)",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)",
				o->current_workspace);
			return 0;
		}
	}
	if (n_outputs == 0) {
		wlr_log(WLR_ERROR, "workspace-test: FAIL no outputs");
		return 0;
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel hidden "
				"before any switch");
			return 0;
		}
	}

	wl_list_for_each(o, &server->outputs, link) {
		switch_workspace(o, ws);
		if (o->current_workspace != ws) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL switch on %s "
				"(got %d, want %d)",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)",
				o->current_workspace, ws);
			return 0;
		}
	}
	/* a transition animation may still be sliding the old windows out:
	 * settle it before asserting the visibility state */
	effects_flush(server);
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel visible "
				"on non-current workspace");
			return 0;
		}
	}

	struct guibux_toplevel *mover = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		mover = t;
		break;
	}
	if (mover != NULL) {
		move_toplevel_to_workspace(mover, ws);
		if (mover->workspace != ws || !mover->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL move to current ws "
				"(workspace=%d, enabled=%d, want %d/1)",
				mover->workspace, mover->scene_tree->node.enabled, ws);
			return 0;
		}
		/* the focus border must follow the window's new workspace color */
		if (server->overview.ws_colors_enabled &&
				ws >= 1 && ws <= NUM_WORKSPACES &&
				server->overview.ws_colors[ws - 1] != 0) {
			if (mover->border_node == NULL || !mover->border_node->node.enabled) {
				wlr_log(WLR_ERROR, "workspace-test: FAIL border not shown "
					"on focused window after move (node=%p, enabled=%d)",
					(void *)mover->border_node,
					mover->border_node ? mover->border_node->node.enabled : -1);
				return 0;
			}
			if (mover->border_color != server->overview.ws_colors[ws - 1]) {
				wlr_log(WLR_ERROR, "workspace-test: FAIL border color after move "
					"(got 0x%08x, want 0x%08x)",
					mover->border_color, server->overview.ws_colors[ws - 1]);
				return 0;
			}
		}
		move_toplevel_to_workspace(mover, 1);
		if (mover->workspace != 1 || mover->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL move away "
				"(workspace=%d, enabled=%d, want 1/0)",
				mover->workspace, mover->scene_tree->node.enabled);
			return 0;
		}
	}

	wl_list_for_each(o, &server->outputs, link) {
		switch_workspace(o, 1);
	}
	effects_flush(server);
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel still "
				"hidden after switch back");
			return 0;
		}
	}
	wlr_log(WLR_INFO, "workspace-test: OK (%d outputs, ws %d, %d toplevels)",
		n_outputs, ws, n_toplevels);
	return 0;
}

int tile_test_run(void *data) {
	struct guibux_server *server = data;
	int mode = atoi(getenv("GUIBUX_TEST_TILE_MODE"));
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		o->tile_mode = mode;
		o->tile_modes[o->current_workspace] = mode;
		retile_output(o);
		wlr_log(WLR_INFO, "tile-test: mode %d on %s", mode,
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
	}
	return 0;
}

/* clean exit, same call as the quit keybind (GUIBUX_ACT_QUIT) */
int quit_test_run(void *data) {
	struct guibux_server *server = data;
	wlr_log(WLR_INFO, "quit-test: terminating display");
	wl_display_terminate(server->wl_display);
	return 0;
}

/* position the cursor at the center of workspace row `ws` of output `o` */
static void cursor_over_row(struct guibux_server *server,
		struct guibux_output *o, int ws) {
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	int area_y = box.y + server->topbar_height;
	int area_h = box.height - server->topbar_height;
	int row_h = area_h / NUM_WORKSPACES;
	server->cursor->x = box.x + box.width / 2.0;
	server->cursor->y = area_y + (ws - 1) * row_h + row_h / 2.0;
}

static struct guibux_output *other_output(struct guibux_server *server,
		struct guibux_output *skip) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o != skip) {
			return o;
		}
	}
	return NULL;
}

/* Global topbar pill list: with two outputs and one window on each, every
 * bar must list both windows (own-monitor first, other-monitor after the
 * separator). The mini-map (decorative, own-monitor only) is drawn
 * separately under the ws cells. */
int global_topbar_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_output *o;
	int n_outputs = 0;
	wl_list_for_each(o, &server->outputs, link) {
		n_outputs++;
	}
	if (n_outputs < 2) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL need two outputs (GUIBUX_TEST_EXTRA_OUTPUTS=1)");
		return 0;
	}
	/* collect the toplevels and their outputs */
	struct guibux_toplevel *ts[8];
	int n_t = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->managed && n_t < 8) {
			ts[n_t++] = t;
		}
	}
	if (n_t < 2) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL need two toplevels (got %d)", n_t);
		return 0;
	}
	/* ensure one window per output: move the second to the other output
	 * if both landed on the same one */
	struct guibux_output *o1 = guibux_output_for(server,
		toplevel_output_for(ts[0]));
	struct guibux_output *o2 = guibux_output_for(server,
		toplevel_output_for(ts[1]));
	if (o1 == o2) {
		struct guibux_output *alt = other_output(server, o1);
		if (alt == NULL) {
			wlr_log(WLR_ERROR, "global-topbar-test: FAIL no second output");
			return 0;
		}
		move_toplevel_to_output(ts[1], alt->wlr_output);
		o2 = alt;
	}
	/* render both bars and verify the global pill list. The map handler
	 * must have already marked both bars dirty; the explicit set here
	 * would mask a regression, so verify the flag first */
	if (!o1->topbar_dirty || !o2->topbar_dirty) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL bars not marked dirty on map (o1=%d o2=%d)",
			o1->topbar_dirty, o2->topbar_dirty);
		return 0;
	}
	topbar_render(o1);
	topbar_render(o2);
	/* o1's bar: ts[0] (own) must come before ts[1] (other) */
	int idx0 = -1, idx1 = -1;
	for (int i = 0; i < o1->topbar_win_count; i++) {
		if (o1->topbar_wins[i] == ts[0]) idx0 = i;
		if (o1->topbar_wins[i] == ts[1]) idx1 = i;
	}
	if (idx0 < 0 || idx1 < 0 || idx0 >= idx1) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL o1 list order (own idx %d, other idx %d, count %d)",
			idx0, idx1, o1->topbar_win_count);
		return 0;
	}
	/* o2's bar: ts[1] (own) must come before ts[0] (other) */
	idx0 = -1; idx1 = -1;
	for (int i = 0; i < o2->topbar_win_count; i++) {
		if (o2->topbar_wins[i] == ts[0]) idx0 = i;
		if (o2->topbar_wins[i] == ts[1]) idx1 = i;
	}
	if (idx0 < 0 || idx1 < 0 || idx1 >= idx0) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL o2 list order (own idx %d, other idx %d, count %d)",
			idx1, idx0, o2->topbar_win_count);
		return 0;
	}
	/* cross-monitor entries must carry the window's own monitor letter */
	char want0 = 'A' + (o1->topbar_number - 1);
	char want1 = 'A' + (o2->topbar_number - 1);
	int ti1 = -1, ti0 = -1;
	for (int i = 0; i < o1->topbar_win_count; i++)
		if (o1->topbar_wins[i] == ts[1]) ti1 = i;
	for (int i = 0; i < o2->topbar_win_count; i++)
		if (o2->topbar_wins[i] == ts[0]) ti0 = i;
	if (ti1 < 0 || ti0 < 0 ||
			o1->topbar_win_titles[ti1][0] != want1 ||
			o2->topbar_win_titles[ti0][0] != want0) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL cross-monitor prefix (o1 ts1: '%s', o2 ts0: '%s')",
			ti1 >= 0 ? o1->topbar_win_titles[ti1] : "?",
			ti0 >= 0 ? o2->topbar_win_titles[ti0] : "?");
		return 0;
	}
	/* regression: closing a window must mark every bar dirty and the
	 * window must be gone from the global list after render */
	xdg_toplevel_unmap(&ts[0]->unmap, NULL);
	if (!o1->topbar_dirty || !o2->topbar_dirty) {
		wlr_log(WLR_ERROR, "global-topbar-test: FAIL bars not marked dirty on unmap (o1=%d o2=%d)",
			o1->topbar_dirty, o2->topbar_dirty);
		return 0;
	}
	topbar_render(o1);
	topbar_render(o2);
	for (int i = 0; i < o1->topbar_win_count; i++) {
		if (o1->topbar_wins[i] == ts[0]) {
			wlr_log(WLR_ERROR, "global-topbar-test: FAIL o1 still lists unmapped window (count %d)",
				o1->topbar_win_count);
			return 0;
		}
	}
	for (int i = 0; i < o2->topbar_win_count; i++) {
		if (o2->topbar_wins[i] == ts[0]) {
			wlr_log(WLR_ERROR, "global-topbar-test: FAIL o2 still lists unmapped window (count %d)",
				o2->topbar_win_count);
			return 0;
		}
	}
	wlr_log(WLR_INFO, "global-topbar-test: OK (%d outputs, %d toplevels, both bars list both, own first)",
		n_outputs, n_t);
	return 0;
}

int overview_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_toplevel *t;
	int n_outputs = 0, n_toplevels = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		n_toplevels++;
	}
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		n_outputs++;
	}
	if (n_outputs == 0 || n_toplevels == 0) {
		wlr_log(WLR_ERROR, "overview-test: FAIL no outputs/toplevels (%d/%d)",
			n_outputs, n_toplevels);
		return 0;
	}

	double ox[64], oy[64];
	int n = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (n >= 64) {
			break;
		}
		ox[n] = t->scene_tree->node.x;
		oy[n] = t->scene_tree->node.y;
		n++;
	}

	overview_show(server);
	if (!server->overview.active) {
		wlr_log(WLR_ERROR, "overview-test: FAIL not active after show");
		return 0;
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "overview-test: FAIL window not enabled in overview");
			return 0;
		}
		struct guibux_output *oo = guibux_output_for(server,
			toplevel_output_for(t));
		if (oo == NULL) {
			wlr_log(WLR_ERROR, "overview-test: FAIL window has no output in overview");
			return 0;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			oo->wlr_output, &box);
		double x = t->scene_tree->node.x;
		double y = t->scene_tree->node.y;
		int area_y = box.y + server->topbar_height;
		int area_h = box.height - server->topbar_height;
		int row_h = area_h / NUM_WORKSPACES;
		bool on_row = false;
		for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
			if (y == (double)(area_y + (ws - 1) * row_h)) {
				on_row = true;
				break;
			}
		}
		if (x < box.x + OVERVIEW_WS_COL_W || x >= box.x + box.width ||
				!on_row) {
			wlr_log(WLR_ERROR, "overview-test: FAIL window (%.0f,%.0f) "
				"not on a workspace row of its output", x, y);
			return 0;
		}
	}
	wl_list_for_each(o, &server->outputs, link) {
		if (!o->overview_ws_col_node) {
			wlr_log(WLR_ERROR, "overview-test: FAIL no workspace column "
				"on %s", o->wlr_output->name ? o->wlr_output->name : "(unknown)");
			return 0;
		}
	}

	overview_hide(server);
	if (server->overview.active) {
		wlr_log(WLR_ERROR, "overview-test: FAIL still active after hide");
		return 0;
	}
	wl_list_for_each(o, &server->outputs, link) {
		if (o->overview_ws_col_node || o->overview_ws_col_buf) {
			wlr_log(WLR_ERROR, "overview-test: FAIL workspace column "
				"not destroyed on %s",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)");
			return 0;
		}
	}
	int i = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (i >= 64) {
			break;
		}
		if (t->scene_tree->node.x != ox[i] || t->scene_tree->node.y != oy[i]) {
			wlr_log(WLR_ERROR, "overview-test: FAIL geometry not restored "
				"(%d,%d) != (%.0f,%.0f)",
				t->scene_tree->node.x, t->scene_tree->node.y, ox[i], oy[i]);
			return 0;
		}
		i++;
	}

	// --- drag & drop: move a window to another workspace (GNOME-style) ---
	overview_show(server);
	if (!server->overview.active) {
		wlr_log(WLR_ERROR, "overview-test: FAIL not active for drag");
		return 0;
	}
	struct guibux_toplevel *dw = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		dw = t;
		break;
	}
	if (dw == NULL) {
		wlr_log(WLR_ERROR, "overview-test: FAIL no window to drag");
		return 0;
	}
	struct guibux_output *dwo = guibux_output_for(server,
		toplevel_output_for(dw));
	/* drag to workspace 2 on the window's own output */
	int want_ws = 2;
	if (dwo != NULL) {
		cursor_over_row(server, dwo, want_ws);
	}
	server->overview.drag_toplevel = dw;
	server->overview.drag_active = true;
	overview_update_hover(server);
	if (server->overview.hover_ws != want_ws ||
			server->overview.hover_output !=
				(dwo != NULL ? dwo->wlr_output : NULL)) {
		wlr_log(WLR_ERROR, "overview-test: FAIL hover ws (got %d, want %d)",
			server->overview.hover_ws, want_ws);
		return 0;
	}
	overview_button_release(server);
	if (server->overview.hover_ws != 0) {
		wlr_log(WLR_ERROR, "overview-test: FAIL hover not cleared after drop");
		return 0;
	}
	if (dw->workspace != want_ws) {
		wlr_log(WLR_ERROR, "overview-test: FAIL drag ws (got %d, want %d)",
			dw->workspace, want_ws);
		return 0;
	}
	if (!server->overview.active) {
		wlr_log(WLR_ERROR, "overview-test: FAIL overview closed after drag");
		return 0;
	}
	/* cross-monitor drag if a second output exists */
	struct guibux_output *o2 = other_output(server, dwo);
	if (o2 != NULL) {
		int want_ws2 = 3;
		cursor_over_row(server, o2, want_ws2);
		server->overview.drag_toplevel = dw;
		server->overview.drag_active = true;
		overview_button_release(server);
		if (dw->workspace != want_ws2) {
			wlr_log(WLR_ERROR, "overview-test: FAIL cross-monitor drag ws "
				"(got %d, want %d)", dw->workspace, want_ws2);
			return 0;
		}
		if (toplevel_output_for(dw) != o2->wlr_output) {
			wlr_log(WLR_ERROR, "overview-test: FAIL cross-monitor drag "
				"output");
			return 0;
		}
	}
	overview_hide(server);
	if (server->overview.active) {
		wlr_log(WLR_ERROR, "overview-test: FAIL still active after drag hide");
		return 0;
	}
	struct wlr_output *final_out = o2 != NULL ? o2->wlr_output
		: (dwo != NULL ? dwo->wlr_output : NULL);
	if (final_out != NULL && toplevel_output_for(dw) != final_out) {
		wlr_log(WLR_ERROR, "overview-test: FAIL window not on final output "
			"after drag hide");
		return 0;
	}
	wlr_log(WLR_INFO, "overview-test: OK (%d outputs, %d toplevels)",
		n_outputs, n_toplevels);
	return 0;
}

int keybind_test_run(void *data) {
	struct guibux_server *server = data;
	const char *key = getenv("GUIBUX_TEST_KEYBIND");
	if (key == NULL) {
		return 0;
	}
	xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL unknown key '%s'", key);
		return 0;
	}
	bool handled = handle_keybinding(server, sym, WLR_MODIFIER_LOGO);
	if (!handled) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL Mod+%s not in keybind table", key);
		return 0;
	}
	if (!server->launcher.active) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL launcher not active after Mod+%s", key);
		return 0;
	}
	launcher_hide(server);
	wlr_log(WLR_INFO, "keybind-test: OK (Mod+%s opened the launcher)", key);
	return 0;
}

int psel_test_run(void *data) {
	struct guibux_server *server = data;
	if (wl_list_empty(&server->toplevels)) {
		wlr_log(WLR_ERROR, "psel-test: FAIL no toplevel mapped");
		return 0;
	}
	if (!server->psel_test_enter_sent) {
		wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER);
		wlr_log(WLR_INFO, "psel-test: pointer capability set");
		server->psel_test_enter_sent = true;
		wl_event_source_timer_update(server->psel_test_timer, 1000);
		return 0;
	}
	struct guibux_toplevel *t = wl_container_of(
		server->toplevels.next, t, link);
	struct wlr_surface *surface = toplevel_get_surface(t);
	wlr_seat_pointer_notify_enter(server->seat, surface, 0.0, 0.0);
	wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	wlr_log(WLR_INFO, "psel-test: enter sent");
	return 0;
}

/* scroll over the topbar VOL indicator: the published volume must move
 * by one step (scroll up = up) and the system volume is restored after */
int scroll_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_buffer == NULL || o->topbar_vol_w <= 0) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);

		struct guibux_sysinfo_snapshot before, after;
		sysinfo_get(&server->sysinfo, &before);
		int step = 1;
		double delta = -1;
		if (before.volume >= 150) {
			step = -1;
			delta = 1;
		}

		server->cursor->x = box.x + o->topbar_vol_x + o->topbar_vol_w / 2.0;
		server->cursor->y = box.y + server->topbar_height / 2.0;
		server->cursor_topbar_output = o;
		struct wlr_pointer_axis_event event = {
			.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
			.delta = delta,
			.delta_discrete = (int32_t)delta,
		};
		server_cursor_axis(&server->cursor_axis, &event);

		sysinfo_get(&server->sysinfo, &after);
		/* restore the system volume before reporting: relative, so it
		 * nets to zero even if the two pactl children run in either
		 * order */
		volume_change(server, false, -step);
		if (after.volume != before.volume + step) {
			wlr_log(WLR_ERROR, "scroll-test: FAIL volume (got %d, want %d)",
				after.volume, before.volume + step);
			return 0;
		}
		wlr_log(WLR_INFO, "scroll-test: OK (scroll %s = volume %s, %d -> %d)",
			delta < 0 ? "up" : "down", step > 0 ? "up" : "down",
			before.volume, after.volume);
		return 0;
	}
	wlr_log(WLR_INFO, "scroll-test: SKIP no audio indicator rendered");
	return 0;
}

/* alt+left-drag must start a move, follow the cursor, and leave the
 * window at the drop position (GNOME-style) */
int alt_drag_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_toplevel *t = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->managed) {
			break;
		}
	}
	if (t == NULL) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL no managed toplevel");
		return 0;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb == NULL) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL no keyboard");
		return 0;
	}

	struct wlr_box geo;
	toplevel_get_geometry(t, &geo);
	double ox = t->scene_tree->node.x;
	double oy = t->scene_tree->node.y;
	double cx = ox + geo.width / 2.0;
	double cy = oy + geo.height / 2.0;

	server->cursor->x = cx;
	server->cursor->y = cy;
	process_cursor_motion(server, 1);
	kb->modifiers.depressed |= WLR_MODIFIER_ALT;

	struct wlr_pointer_button_event press = {
		.time_msec = 1,
		.button = 272, /* BTN_LEFT */
		.state = WL_POINTER_BUTTON_STATE_PRESSED,
	};
	server_cursor_button(&server->cursor_button, &press);
	if (server->cursor_mode != GUIBUX_CURSOR_MOVE ||
			server->grabbed_toplevel != t) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL drag not started (mode %d, grab %s)",
			server->cursor_mode,
			server->grabbed_toplevel == t ? "yes" : "no");
		return 0;
	}

	server->cursor->x = cx + 100;
	server->cursor->y = cy + 50;
	process_cursor_motion(server, 2);
	if (t->scene_tree->node.x != ox + 100 ||
			t->scene_tree->node.y != oy + 50) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL window not moved (got %d,%d, want %d,%d)",
			t->scene_tree->node.x, t->scene_tree->node.y,
			(int)(ox + 100), (int)(oy + 50));
		return 0;
	}

	kb->modifiers.depressed &= ~WLR_MODIFIER_ALT;
	struct wlr_pointer_button_event release = {
		.time_msec = 3,
		.button = 272,
		.state = WL_POINTER_BUTTON_STATE_RELEASED,
	};
	server_cursor_button(&server->cursor_button, &release);
	if (server->cursor_mode != GUIBUX_CURSOR_PASSTHROUGH ||
			server->grabbed_toplevel != NULL) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL drag not ended (mode %d)",
			server->cursor_mode);
		return 0;
	}
	if (t->scene_tree->node.x != ox + 100 ||
			t->scene_tree->node.y != oy + 50) {
		wlr_log(WLR_ERROR, "altdrag-test: FAIL window not kept at drop position (got %d,%d)",
			t->scene_tree->node.x, t->scene_tree->node.y);
		return 0;
	}
	wlr_log(WLR_INFO, "altdrag-test: OK (moved %d,%d -> %d,%d, stayed)",
		(int)ox, (int)oy, t->scene_tree->node.x, t->scene_tree->node.y);
	return 0;
}

/* Dragging a window onto another monitor must reassign its stored
 * output: the original bar must drop it, the new bar must list it.
 * Regression: the release handler asked toplevel_output_for(), which
 * prefers the stored output, so the "new" output was always the old
 * one and the window never left the original monitor. Phase 2 covers
 * a resize that drags the window center back across the boundary. */
static struct {
	int phase;
	int w0;
	struct guibux_output *src, *dst;
} xmondrag_test_state;

static bool topbar_lists(struct guibux_output *o, struct guibux_toplevel *t) {
	for (int i = 0; i < o->topbar_win_count; i++) {
		if (o->topbar_wins[i] == t) {
			return true;
		}
	}
	return false;
}

static void xmondrag_test_next(struct guibux_server *server) {
	wl_event_source_timer_update(server->xmondrag_test_timer, 400);
}

int xmondrag_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_output *o;
	int n_outputs = 0;
	wl_list_for_each(o, &server->outputs, link) {
		n_outputs++;
	}
	if (n_outputs < 2) {
		wlr_log(WLR_ERROR, "xmondrag-test: FAIL need two outputs (GUIBUX_TEST_EXTRA_OUTPUTS=1)");
		return 0;
	}
	struct guibux_toplevel *t = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->managed) {
			break;
		}
	}
	if (t == NULL) {
		wlr_log(WLR_ERROR, "xmondrag-test: FAIL no managed toplevel");
		return 0;
	}

	switch (xmondrag_test_state.phase) {
	case 0: {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (kb == NULL) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL no keyboard");
			return 0;
		}
		xmondrag_test_state.src = guibux_output_for(server,
			toplevel_output_for(t));
		xmondrag_test_state.dst = other_output(server,
			xmondrag_test_state.src);
		if (xmondrag_test_state.src == NULL ||
				xmondrag_test_state.dst == NULL) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL no src/dst output");
			return 0;
		}
		struct guibux_output *src = xmondrag_test_state.src;
		struct guibux_output *dst = xmondrag_test_state.dst;

		/* alt+drag the window center onto the other monitor */
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		xmondrag_test_state.w0 = geo.width;
		double cx = t->scene_tree->node.x + geo.width / 2.0;
		double cy = t->scene_tree->node.y + geo.height / 2.0;
		server->cursor->x = cx;
		server->cursor->y = cy;
		process_cursor_motion(server, 1);
		kb->modifiers.depressed |= WLR_MODIFIER_ALT;
		struct wlr_pointer_button_event press = {
			.time_msec = 1,
			.button = 272, /* BTN_LEFT */
			.state = WL_POINTER_BUTTON_STATE_PRESSED,
		};
		server_cursor_button(&server->cursor_button, &press);
		if (server->cursor_mode != GUIBUX_CURSOR_MOVE ||
				server->grabbed_toplevel != t) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL drag not started (mode %d, grab %s)",
				server->cursor_mode,
				server->grabbed_toplevel == t ? "yes" : "no");
			return 0;
		}
		struct wlr_box dbox;
		wlr_output_layout_get_box(server->output_layout, dst->wlr_output, &dbox);
		server->cursor->x = dbox.x + dbox.width / 2.0;
		server->cursor->y = dbox.y + dbox.height / 2.0;
		process_cursor_motion(server, 2);
		kb->modifiers.depressed &= ~WLR_MODIFIER_ALT;
		struct wlr_pointer_button_event release = {
			.time_msec = 3,
			.button = 272,
			.state = WL_POINTER_BUTTON_STATE_RELEASED,
		};
		server_cursor_button(&server->cursor_button, &release);
		if (t->output != dst) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL output not updated after drag");
			return 0;
		}
		src->topbar_dirty = true;
		dst->topbar_dirty = true;
		topbar_render(src);
		topbar_render(dst);
		/* the topbar pill list is global (own + other monitors): both
		 * bars list the window; the window's own output must be the dst */
		if (!topbar_lists(dst, t) || !topbar_lists(src, t) ||
				t->output != dst) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL topbar lists after drag (dst %s, src %s, output %s)",
				topbar_lists(dst, t) ? "yes" : "no",
				topbar_lists(src, t) ? "yes" : "no",
				t->output == dst ? "dst" : "other");
			return 0;
		}

		/* start the cross-boundary resize: drag the left edge until the
		 * center lands on the src monitor; the release comes in phase 1,
		 * once the client has committed the new geometry */
		struct wlr_box sbox;
		wlr_output_layout_get_box(server->output_layout, src->wlr_output, &sbox);
		double left = t->scene_tree->node.x + geo.x;
		double right = left + geo.width;
		double want_left = 2.0 * (sbox.x + sbox.width / 2.0) - right;
		server->cursor->x = left;
		server->cursor->y = cy;
		begin_interactive(t, GUIBUX_CURSOR_RESIZE, WLR_EDGE_LEFT);
		server->cursor->x = want_left;
		process_cursor_motion(server, 4);
		xmondrag_test_state.phase = 1;
		xmondrag_test_next(server);
		return 0;
	}
	case 1: {
		struct guibux_output *src = xmondrag_test_state.src;
		struct guibux_output *dst = xmondrag_test_state.dst;
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		double center = t->scene_tree->node.x + geo.width / 2.0;
		struct wlr_box sbox;
		wlr_output_layout_get_box(server->output_layout, src->wlr_output, &sbox);
		/* the client must have committed the resized geometry before the
		 * release, or the position lookup sees a stale box */
		if (geo.width < 2.0 * xmondrag_test_state.w0 ||
				center < sbox.x || center >= sbox.x + sbox.width) {
			xmondrag_test_next(server);
			return 0;
		}
		struct wlr_pointer_button_event release = {
			.time_msec = 5,
			.button = 272,
			.state = WL_POINTER_BUTTON_STATE_RELEASED,
		};
		server_cursor_button(&server->cursor_button, &release);
		if (t->output != src) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL output not updated after resize");
			return 0;
		}
		src->topbar_dirty = true;
		dst->topbar_dirty = true;
		topbar_render(src);
		topbar_render(dst);
		/* the topbar pill list is global: both bars list the window; the
		 * window's own output must be back on the src */
		if (!topbar_lists(src, t) || !topbar_lists(dst, t) ||
				t->output != src) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL topbar lists after resize (src %s, dst %s, output %s)",
				topbar_lists(src, t) ? "yes" : "no",
				topbar_lists(dst, t) ? "yes" : "no",
				t->output == src ? "src" : "other");
			return 0;
		}
		wlr_log(WLR_INFO, "xmondrag-test: OK (drag + resize across outputs)");
		return 0;
	}
	}
	return 0;
}

/* Interactive resize of a window that is NOT at the layout origin must
 * resize in place: right/bottom drags keep the node position, left/top
 * drags move the node with the dragged edge. The regression this guards
 * mixed client-relative geometry with absolute cursor coordinates and
 * teleported the window to (0,0) (top-left of the first monitor).
 * One drag per phase (400ms apart) so the client can process the
 * configure and update its window geometry in between, like a real
 * client (GTK) does. */
static struct {
	double ox, oy;
	int w, h;
	int phase;
} resize_test_state;

static bool near_d(double a, double b) {
	return a >= b - 1.0 && a <= b + 1.0;
}

static bool check_committed_size(struct wlr_surface *s, int w, int h,
		const char *what) {
	if (s->current.width != w || s->current.height != h) {
		wlr_log(WLR_ERROR, "resize-test: FAIL size %s (got %dx%d, want %dx%d)",
			what, s->current.width, s->current.height, w, h);
		return false;
	}
	return true;
}

/* grab the given edge, drag it by (move_dx, move_dy), check the node
 * lands at (want_x, want_y), release */
static bool resize_drag(struct guibux_server *server,
		struct guibux_toplevel *t, uint32_t edges,
		double move_dx, double move_dy,
		double want_x, double want_y, const char *what) {
	struct wlr_box geo;
	toplevel_get_geometry(t, &geo);
	double gx, gy;
	if (edges & WLR_EDGE_LEFT) {
		gx = 0;
	} else if (edges & WLR_EDGE_RIGHT) {
		gx = geo.width;
	} else {
		gx = geo.width / 2.0;
	}
	if (edges & WLR_EDGE_TOP) {
		gy = 0;
	} else if (edges & WLR_EDGE_BOTTOM) {
		gy = geo.height;
	} else {
		gy = geo.height / 2.0;
	}
	server->cursor->x = t->scene_tree->node.x + geo.x + gx;
	server->cursor->y = t->scene_tree->node.y + geo.y + gy;
	begin_interactive(t, GUIBUX_CURSOR_RESIZE, edges);
	server->cursor->x += move_dx;
	server->cursor->y += move_dy;
	process_cursor_motion(server, 1);
	if (!near_d(t->scene_tree->node.x, want_x) ||
			!near_d(t->scene_tree->node.y, want_y)) {
		wlr_log(WLR_ERROR, "resize-test: FAIL %s (got %.0f,%.0f, want %.0f,%.0f)",
			what, (double)t->scene_tree->node.x, (double)t->scene_tree->node.y,
			want_x, want_y);
		return false;
	}
	reset_cursor_mode(server);
	return true;
}

static void resize_test_next(struct guibux_server *server) {
	wl_event_source_timer_update(server->resize_test_timer, 400);
}

int resize_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_toplevel *t = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->managed && t->xdg_toplevel != NULL) {
			break;
		}
	}
	if (t == NULL) {
		wlr_log(WLR_ERROR, "resize-test: FAIL no managed xdg toplevel");
		return 0;
	}
	struct wlr_surface *s = t->xdg_toplevel->base->surface;
	int w = resize_test_state.w, h = resize_test_state.h;
	double ox = resize_test_state.ox, oy = resize_test_state.oy;

	switch (resize_test_state.phase) {
	case 0: {
		struct guibux_output *cur = guibux_output_for(server,
			toplevel_output_for(t));
		struct guibux_output *o2 = other_output(server, cur);
		if (o2 == NULL) {
			wlr_log(WLR_ERROR, "resize-test: FAIL need two outputs (GUIBUX_TEST_EXTRA_OUTPUTS=1)");
			return 0;
		}
		move_toplevel_to_output(t, o2->wlr_output);
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		ox = t->scene_tree->node.x;
		oy = t->scene_tree->node.y;
		if (ox <= 1.0 || oy <= 1.0) {
			wlr_log(WLR_ERROR, "resize-test: FAIL window at layout origin (%.0f,%.0f)",
				ox, oy);
			return 0;
		}
		resize_test_state.ox = ox;
		resize_test_state.oy = oy;
		resize_test_state.w = geo.width;
		resize_test_state.h = geo.height;
		if (!resize_drag(server, t, WLR_EDGE_RIGHT, 200, 0, ox, oy,
				"right edge")) {
			return 0;
		}
		resize_test_state.phase = 1;
		resize_test_next(server);
		return 0;
	}
	case 1:
		if (!check_committed_size(s, w + 200, h, "after right edge")) {
			return 0;
		}
		if (!resize_drag(server, t, WLR_EDGE_LEFT, 100, 0,
				ox + 100, oy, "left edge")) {
			return 0;
		}
		resize_test_state.phase = 2;
		resize_test_next(server);
		return 0;
	case 2:
		if (!check_committed_size(s, w + 100, h, "after left edge")) {
			return 0;
		}
		if (!resize_drag(server, t, WLR_EDGE_TOP, 0, 80,
				ox + 100, oy + 80, "top edge")) {
			return 0;
		}
		resize_test_state.phase = 3;
		resize_test_next(server);
		return 0;
	case 3:
		if (!check_committed_size(s, w + 100, h - 80, "after top edge")) {
			return 0;
		}
		if (!resize_drag(server, t, WLR_EDGE_BOTTOM, 0, 60,
				ox + 100, oy + 80, "bottom edge")) {
			return 0;
		}
		resize_test_state.phase = 4;
		resize_test_next(server);
		return 0;
	case 4:
		if (!check_committed_size(s, w + 100, h - 20, "final")) {
			return 0;
		}
		if (!near_d(t->scene_tree->node.x, ox + 100) ||
				!near_d(t->scene_tree->node.y, oy + 80)) {
			wlr_log(WLR_ERROR, "resize-test: FAIL final position (got %.0f,%.0f, want %.0f,%.0f)",
				(double)t->scene_tree->node.x, (double)t->scene_tree->node.y,
				ox + 100, oy + 80);
			return 0;
		}
		wlr_log(WLR_INFO, "resize-test: OK (4 edges on second output, final %dx%d at %.0f,%.0f)",
			s->current.width, s->current.height,
			(double)t->scene_tree->node.x, (double)t->scene_tree->node.y);
		return 0;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Effects test: close (frozen-buffer shrink + animated retile) and open
// (scale-in). The client (effects-test) maps A+B, destroys A when the
// compositor sends a close request, and maps C late. The phases verify
// each animation in flight and at rest.
// ---------------------------------------------------------------------------

static struct {
	int phase;
	int64_t start_ms;         /* overall start (global timeout) */
	int64_t phase_start_ms;   /* current polling phase start (per-phase timeout) */
	struct guibux_output *o;
	struct guibux_toplevel *b;
	struct guibux_toplevel *c;
	struct wlr_buffer *a_buf;   /* A's last buffer: the close snapshot holds it */
	int b_x1;   /* B's cell x after A's close */
	bool saw_in_flight;       /* at least one animation observed mid-way */
	bool saw_snap;            /* the close snapshot node was observed */
} effects_test_state;

static int64_t effects_test_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void effects_test_next(struct guibux_server *server, int ms) {
	wl_event_source_timer_update(server->effects_test_timer, ms);
}

int effects_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_toplevel *t;

	/* global safety timeout: a stuck polling phase ends up here */
	if (effects_test_state.start_ms > 0 &&
			effects_test_now() - effects_test_state.start_ms > 15000) {
		wlr_log(WLR_ERROR, "effects-test: FAIL timeout (phase %d)",
			effects_test_state.phase);
		return 0;
	}

	switch (effects_test_state.phase) {
	case 0: {
		/* setup: 2 mapped windows, split tiling, slow animations, close one */
		int n = 0;
		struct guibux_toplevel *first = NULL;
		wl_list_for_each(t, &server->toplevels, link) {
			if (first == NULL) {
				first = t;
			}
			n++;
		}
		/* the output the toplevels are on: xwayland warps the wl cursor
		 * to the X pointer position, so the first output in the list is
		 * not necessarily the one the windows landed on */
		struct guibux_output *o = first != NULL
			? guibux_output_for(server, toplevel_output_for(first)) : NULL;
		if (n < 2 || o == NULL) {
			wlr_log(WLR_ERROR, "effects-test: FAIL need 2 toplevels (got %d)", n);
			return 0;
		}
		if (!server->effects_enabled || server->effects_duration_ms <= 0) {
			wlr_log(WLR_ERROR, "effects-test: FAIL effects disabled");
			return 0;
		}
		/* slow the animations down so the in-flight states are reliably
		 * observable by the polling checks that follow */
		server->effects_duration_ms = 500;
		for (int i = 1; i <= NUM_WORKSPACES; i++) o->tile_modes[i] = GUIBUX_TILE_SPLIT;
		retile_output(o);
		effects_test_state.o = o;
		struct guibux_toplevel *a = NULL;
		wl_list_for_each(t, &server->toplevels, link) {
			if (toplevel_output_for(t) != o->wlr_output) {
				continue;
			}
			if (a == NULL) {
				a = t;
			} else {
				effects_test_state.b = t;
				break;
			}
		}
		if (a == NULL || effects_test_state.b == NULL) {
			wlr_log(WLR_ERROR, "effects-test: FAIL need 2 toplevels on one output");
			return 0;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		effects_test_state.b_x1 = box.x;
		struct wlr_scene_buffer *ab = toplevel_inner_buffer(a);
		effects_test_state.a_buf = ab != NULL ? ab->buffer : NULL;
		effects_test_state.start_ms = effects_test_now();
		toplevel_close(a);   /* the client destroys A: snapshot + animated retile */
		effects_test_state.phase = 1;
		effects_test_next(server, 20);
		return 0;
	}
	case 1: {
		/* poll: B settles into the freed cell at full height; the close
		 * snapshot (a root-tree scene buffer holding A's last buffer)
		 * appears during the animation and is destroyed when it ends */
		struct guibux_toplevel *b = effects_test_state.b;
		struct guibux_output *o = effects_test_state.o;
		if (b->scene_tree == NULL) {
			effects_test_next(server, 20);
			return 0;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		bool settled = (b->scene_tree->node.x == effects_test_state.b_x1 &&
			b->scene_tree->node.y == box.y + server->topbar_height);
		bool snap = false;
		struct wlr_scene_node *child;
		wl_list_for_each(child, &server->scene->tree.children, link) {
			if (child->type == WLR_SCENE_NODE_BUFFER &&
					wlr_scene_buffer_from_node(child)->buffer ==
					effects_test_state.a_buf) {
				snap = true;
				break;
			}
		}
		if (snap) {
			effects_test_state.saw_snap = true;
			effects_test_state.saw_in_flight = true;
		}
		if (!settled) {
			effects_test_state.saw_in_flight = true;
			effects_test_next(server, 20);
			return 0;
		}
		struct wlr_box geo;
		toplevel_get_geometry(b, &geo);
		if (geo.height != box.height - server->topbar_height) {
			effects_test_next(server, 20);
			return 0;
		}
		if (!effects_test_state.saw_snap || snap) {
			effects_test_next(server, 20);
			return 0;
		}
		effects_test_state.phase = 2;
		effects_test_next(server, 20);
		return 0;
	}
	case 2: {
		/* poll: the late-mapped window C shows up with buffer content */
		wl_list_for_each(t, &server->toplevels, link) {
			if (t == effects_test_state.b ||
					toplevel_output_for(t) != effects_test_state.o->wlr_output) {
				continue;
			}
			struct wlr_scene_buffer *sb = toplevel_inner_buffer(t);
			if (sb != NULL && sb->buffer != NULL) {
				effects_test_state.c = t;
				break;
			}
		}
		if (effects_test_state.c == NULL) {
			effects_test_next(server, 20);
			return 0;
		}
		effects_test_state.phase = 3;
		effects_test_next(server, 20);
		return 0;
	}
	case 3: {
		/* poll: C's open scale settles at its natural size */
		struct guibux_toplevel *c = effects_test_state.c;
		struct wlr_scene_buffer *sb = toplevel_inner_buffer(c);
		struct wlr_scene_surface *ss =
			sb != NULL ? wlr_scene_surface_try_from_buffer(sb) : NULL;
		int nw = ss != NULL ? ss->surface->current.width : 0;
		if (sb == NULL || nw <= 0) {
			effects_test_next(server, 20);
			return 0;
		}
		if (sb->dst_width < nw) {
			effects_test_state.saw_in_flight = true;
			effects_test_next(server, 20);
			return 0;
		}
		effects_test_state.phase = 4;
		effects_test_next(server, 20);
		return 0;
	}
	case 4: {
		if (!effects_test_state.saw_in_flight) {
			wlr_log(WLR_ERROR, "effects-test: FAIL no animation observed in flight "
				"(effects instant or disabled)");
			return 0;
		}
		wlr_log(WLR_INFO, "effects-test: OK (close snapshot+retile, open scale)");
		return 0;
	}
	}
	return 0;
}

int outputs_test_run(void *data) {
	struct guibux_server *server = data;
	const char *mode = getenv("GUIBUX_TEST_OUTPUTS");
	if (mode == NULL) {
		mode = "bogus";
	}
	struct wlr_output *outs[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, outs, boxes, 16);

	if (!strcmp(mode, "bogus")) {
		/* a name that matches nothing must not kill the session: the
		 * placements are dropped and both headless outputs stay alive,
		 * auto-arranged */
		if (server->num_placements != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL bogus: placements "
				"not cleared (n=%d)", server->num_placements);
			return 0;
		}
		if (n != 2) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL bogus: expected 2 "
				"live outputs, got %d", n);
			return 0;
		}
		if (boxes[0].x == boxes[1].x && boxes[0].y == boxes[1].y) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL bogus: outputs not "
				"arranged (both at %d,%d)", boxes[0].x, boxes[0].y);
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK bogus (%d outputs auto-arranged)", n);
		return 0;
	}

	if (!strcmp(mode, "placement")) {
		if (n != 2) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL placement: expected 2 outputs, got %d", n);
			return 0;
		}
		if (boxes[0].x != 0 || boxes[0].y != 0 ||
				boxes[1].x != 1280 || boxes[1].y != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL placement: got (%d,%d)/(%d,%d), want (0,0)/(1280,0)",
				boxes[0].x, boxes[0].y, boxes[1].x, boxes[1].y);
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK placement (manual boxes applied)");
		return 0;
	}

	if (!strcmp(mode, "off")) {
		if (n != 1) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL off: expected 1 live output, got %d", n);
			return 0;
		}
		if (outs[0]->name == NULL || strcmp(outs[0]->name, "HEADLESS-1") != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL off: expected HEADLESS-1, got %s",
				outs[0]->name ? outs[0]->name : "(unknown)");
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK off (@off disabled the second output)");
		return 0;
	}

	if (!strcmp(mode, "unplug")) {
		if (n != 2) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: expected 2 outputs, got %d", n);
			return 0;
		}
		struct guibux_output *o1 = guibux_output_for(server, outs[0]);
		struct guibux_output *o2 = guibux_output_for(server, outs[1]);
		if (o1 == NULL || o2 == NULL) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: no guibux outputs");
			return 0;
		}
		struct guibux_toplevel *t = NULL;
		wl_list_for_each(t, &server->toplevels, link) {
			break;
		}
		if (t == NULL) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: no mapped toplevel");
			return 0;
		}
		/* move the window to the second output and focus it, then
		 * simulate that output being unplugged */
		move_toplevel_to_output(t, outs[1]);
		focus_toplevel(t, true);
		struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
		output_rehome_toplevels(server, o2, o1);
		if (t->output != o1) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: window not rehomed "
				"(output=%p, want %p)", (void *)t->output, (void *)o1);
			return 0;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, outs[0], &box);
		int32_t nx = t->scene_tree->node.x;
		int32_t ny = t->scene_tree->node.y;
		if (nx < box.x || ny < box.y ||
				nx >= box.x + box.width || ny >= box.y + box.height) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: window at %d,%d "
				"outside %d,%d %dx%d", nx, ny, box.x, box.y, box.width, box.height);
			return 0;
		}
		if (server->seat->keyboard_state.focused_surface != focused) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL unplug: focus lost after rehome");
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK unplug (window rehomed, focus kept)");
		return 0;
	}

	if (!strcmp(mode, "autofallback")) {
		/* the config named no connected output: the placements must be
		 * dropped and both outputs auto-arranged */
		if (server->num_placements != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL autofallback: placements "
				"not cleared (n=%d)", server->num_placements);
			return 0;
		}
		if (n != 2) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL autofallback: expected 2 "
				"live outputs, got %d", n);
			return 0;
		}
		if (boxes[0].x == boxes[1].x && boxes[0].y == boxes[1].y) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL autofallback: outputs not "
				"arranged (both at %d,%d)", boxes[0].x, boxes[0].y);
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK autofallback (%d outputs auto-arranged)", n);
		return 0;
	}

	if (!strcmp(mode, "apply")) {
		/* live re-apply: the runner started us with a config file
		 * (GUIBUX_CONFIG) placing both outputs; rewrite it, call
		 * outputs_apply and verify the layout follows. Disabling must
		 * keep the output object alive (re-enable without replug) */
		if (server->config_path == NULL) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: no config path");
			return 0;
		}
		FILE *f = fopen(server->config_path, "w");
		if (f == NULL) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: cannot write config");
			return 0;
		}
		fprintf(f, "outputs = HEADLESS-1@0x0,HEADLESS-2@off\n");
		fclose(f);
		outputs_apply(server);
		n = outputs_sorted_by_x(server, outs, boxes, 16);
		if (n != 1 || strcmp(outs[0]->name, "HEADLESS-1") != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: after disable "
				"expected 1 live output HEADLESS-1, got %d", n);
			return 0;
		}
		struct guibux_output *o2 = NULL;
		struct guibux_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (o->wlr_output->name != NULL &&
					strcmp(o->wlr_output->name, "HEADLESS-2") == 0) {
				o2 = o;
			}
		}
		if (o2 == NULL || !o2->disabled) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: HEADLESS-2 not "
				"kept alive and disabled");
			return 0;
		}
		/* re-enable at 1280x0, then move it below HEADLESS-1 */
		f = fopen(server->config_path, "w");
		fprintf(f, "outputs = HEADLESS-1@0x0,HEADLESS-2@1280x0\n");
		fclose(f);
		outputs_apply(server);
		n = outputs_sorted_by_x(server, outs, boxes, 16);
		if (n != 2 || boxes[0].x != 0 || boxes[0].y != 0 ||
				boxes[1].x != 1280 || boxes[1].y != 0) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: after re-enable "
				"got n=%d (%d,%d)/(%d,%d), want (0,0)/(1280,0)",
				n, boxes[0].x, boxes[0].y, boxes[1].x, boxes[1].y);
			return 0;
		}
		f = fopen(server->config_path, "w");
		fprintf(f, "outputs = HEADLESS-1@0x0,HEADLESS-2@0x720\n");
		fclose(f);
		outputs_apply(server);
		n = outputs_sorted_by_x(server, outs, boxes, 16);
		/* both at x=0: check the boxes by name, not sort order */
		struct wlr_box b1 = {0}, b2 = {0};
		struct guibux_output *oo;
		wl_list_for_each(oo, &server->outputs, link) {
			if (oo->wlr_output->name != NULL &&
					strcmp(oo->wlr_output->name, "HEADLESS-1") == 0) {
				wlr_output_layout_get_box(server->output_layout,
					oo->wlr_output, &b1);
			} else if (oo->wlr_output->name != NULL &&
					strcmp(oo->wlr_output->name, "HEADLESS-2") == 0) {
				wlr_output_layout_get_box(server->output_layout,
					oo->wlr_output, &b2);
			}
		}
		if (n != 2 || b1.x != 0 || b1.y != 0 || b2.x != 0 || b2.y != 720) {
			wlr_log(WLR_ERROR, "outputs-test: FAIL apply: after move "
				"got n=%d H1(%d,%d) H2(%d,%d), want H1(0,0) H2(0,720)",
				n, b1.x, b1.y, b2.x, b2.y);
			return 0;
		}
		wlr_log(WLR_INFO, "outputs-test: OK apply (disable, re-enable, move)");
		return 0;
	}

	wlr_log(WLR_ERROR, "outputs-test: FAIL unknown mode '%s'", mode);
	return 0;
}

/* the runner starts us with a config placing both headless outputs; the
 * panel must show both, and each key must move the layout + config */
static void panel_box_by_name(struct guibux_server *server,
		const char *name, struct wlr_box *box) {
	box->x = box->y = box->width = box->height = 0;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->name != NULL &&
				strcmp(o->wlr_output->name, name) == 0) {
			wlr_output_layout_get_box(server->output_layout,
				o->wlr_output, box);
			return;
		}
	}
}

static void panel_boxes_by_name(struct guibux_server *server,
		struct wlr_box *b1, struct wlr_box *b2) {
	panel_box_by_name(server, "HEADLESS-1", b1);
	panel_box_by_name(server, "HEADLESS-2", b2);
}

/* three connected outputs: A(HEADLESS-1, rotated 90, effective 720 wide)
 * B(HEADLESS-2) C(HEADLESS-3) in a row. Selecting B and pressing Left
 * must yield the order B A C, repacked contiguously from the row's left
 * edge by each monitor's effective (rotation-aware) width: B -> 0,
 * A -> 1280, C -> 2000. Right moves B back to A B C: A -> 0, B -> 720,
 * C -> 2000 */
static int outputs_panel_test_three(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	struct wlr_box b1, b2, b3;
	char spec[2048];

	outputs_panel_show(server);
	if (!p->active || p->num_entries != 3) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL show3 (active=%d, "
			"entries=%d, want 3)", p->active, p->num_entries);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK show3 (3 entries)");

	/* select B (HEADLESS-2), move it left of A */
	outputs_panel_handle_key(server, XKB_KEY_Down);
	outputs_panel_handle_key(server, XKB_KEY_Left);
	panel_box_by_name(server, "HEADLESS-1", &b1);
	panel_box_by_name(server, "HEADLESS-2", &b2);
	panel_box_by_name(server, "HEADLESS-3", &b3);
	if (b2.x != 0 || b2.y != 0 || b1.x != 1280 || b1.y != 0 ||
			b3.x != 2000 || b3.y != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move3-left: "
			"A(%d,%d) B(%d,%d) C(%d,%d), want B(0,0) A(1280,0) C(2000,0)",
			b1.x, b1.y, b2.x, b2.y, b3.x, b3.y);
		return 0;
	}
	if (p->selected != 0 || strcmp(p->entries[0].name, "HEADLESS-2") != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move3-left: list "
			"order not swapped (selected=%d, entry=%s)", p->selected,
			p->entries[p->selected].name);
		return 0;
	}
	if (!outputs_config_read(server->config_path, spec, sizeof(spec)) ||
			strstr(spec, "HEADLESS-2@0x0,HEADLESS-1@1280x0:90,"
			"HEADLESS-3@2000x0") == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move3-left: config "
			"not repacked ('%s')", spec);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK move3-left (B A C, repacked "
		"by effective widths)");

	/* move B back right: A B C, A keeps its rotated 720 width */
	outputs_panel_handle_key(server, XKB_KEY_Right);
	panel_box_by_name(server, "HEADLESS-1", &b1);
	panel_box_by_name(server, "HEADLESS-2", &b2);
	panel_box_by_name(server, "HEADLESS-3", &b3);
	if (b1.x != 0 || b1.y != 0 || b2.x != 720 || b2.y != 0 ||
			b3.x != 2000 || b3.y != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move3-right: "
			"A(%d,%d) B(%d,%d) C(%d,%d), want A(0,0) B(720,0) C(2000,0)",
			b1.x, b1.y, b2.x, b2.y, b3.x, b3.y);
		return 0;
	}
	if (p->selected != 1 || strcmp(p->entries[1].name, "HEADLESS-2") != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move3-right: list "
			"order not swapped back (selected=%d, entry=%s)", p->selected,
			p->entries[p->selected].name);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK move3-right (A B C restored)");

	outputs_panel_handle_key(server, XKB_KEY_Escape);
	if (p->active) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL close3 (still active)");
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK3 (3-monitor reflow, close)");
	return 0;
}

int outputs_panel_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_outputs_panel *p = &server->outputs_panel;
	struct wlr_output *outs[16];
	struct wlr_box boxes[16];
	int n;

	if (server->config_path == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL: no config path");
		return 0;
	}
	int nconn = 0;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->name != NULL) {
			nconn++;
		}
	}
	if (nconn >= 3) {
		return outputs_panel_test_three(server);
	}
	outputs_panel_show(server);
	if (!p->active) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL show (active=%d)",
			p->active);
		return 0;
	}
	if (p->num_entries != 2) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL show: expected 2 "
			"entries (both connected outputs), got %d", p->num_entries);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK show (2 entries)");

	/* Right: HEADLESS-1 (0x0) swaps with the monitor to its right
	 * (HEADLESS-2 at 1280x0): H1 -> 1280, H2 -> 0; the list order
	 * swaps with it */
	outputs_panel_handle_key(server, XKB_KEY_Right);
	n = outputs_sorted_by_x(server, outs, boxes, 16);
	struct wlr_box b1, b2;
	panel_boxes_by_name(server, &b1, &b2);
	if (n != 2 || b1.x != 1280 || b1.y != 0 || b2.x != 0 || b2.y != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move-right: n=%d "
			"H1(%d,%d) H2(%d,%d), want H1(1280,0) H2(0,0)",
			n, b1.x, b1.y, b2.x, b2.y);
		return 0;
	}
	if (p->selected != 1 || strcmp(p->entries[1].name, "HEADLESS-1") != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move-right: list "
			"order not swapped (selected=%d, entry=%s)", p->selected,
			p->entries[p->selected].name);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK move-right (H1 -> 1280x0, "
		"H2 -> 0x0, list swapped)");

	/* Left: HEADLESS-1 (now at 1280) swaps back with the monitor to
	 * its left (HEADLESS-2 at 0) */
	outputs_panel_handle_key(server, XKB_KEY_Left);
	n = outputs_sorted_by_x(server, outs, boxes, 16);
	panel_boxes_by_name(server, &b1, &b2);
	if (n != 2 || b1.x != 0 || b1.y != 0 || b2.x != 1280 || b2.y != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move-left: n=%d "
			"H1(%d,%d) H2(%d,%d), want H1(0,0) H2(1280,0)",
			n, b1.x, b1.y, b2.x, b2.y);
		return 0;
	}
	/* Left again: HEADLESS-1 is leftmost, no monitor to its left: no-op */
	outputs_panel_handle_key(server, XKB_KEY_Left);
	panel_boxes_by_name(server, &b1, &b2);
	if (b1.x != 0 || b1.y != 0 ||
			strstr(p->status, "left") == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL move-left edge: "
			"H1(%d,%d) status '%s', want no-op + 'left'",
			b1.x, b1.y, p->status);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK move-left (H1 -> 0x0, "
		"leftmost edge no-op)");

	/* rotation-aware reflow: rotate HEADLESS-2 to 90 (effective
	 * 720x1280), then move it left of HEADLESS-1: the row is repacked
	 * from its left edge by effective widths, H2 -> 0x0, H1 -> 720x0 */
	outputs_panel_handle_key(server, XKB_KEY_Down);
	outputs_panel_handle_key(server, XKB_KEY_r);
	outputs_panel_handle_key(server, XKB_KEY_r);
	panel_boxes_by_name(server, &b1, &b2);
	if (b2.width != 720 || b2.height != 1280) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL reflow: H2 box "
			"%dx%d, want 720x1280 after 90 rotation", b2.width, b2.height);
		return 0;
	}
	outputs_panel_handle_key(server, XKB_KEY_Left);
	n = outputs_sorted_by_x(server, outs, boxes, 16);
	panel_boxes_by_name(server, &b1, &b2);
	if (n != 2 || b2.x != 0 || b2.y != 0 || b1.x != 720 || b1.y != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL reflow: n=%d "
			"H1(%d,%d) H2(%d,%d), want H2(0,0) H1(720,0)",
			n, b1.x, b1.y, b2.x, b2.y);
		return 0;
	}
	if (p->selected != 0 || strcmp(p->entries[0].name, "HEADLESS-2") != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL reflow: list order "
			"not swapped (selected=%d, entry=%s)", p->selected,
			p->entries[p->selected].name);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK reflow (rotated H2 left of "
		"H1, row repacked by effective widths)");

	/* the topbar follows the moved outputs and is resized for the
	 * rotation: node at the box origin, buffer at the box width */
	{
		struct guibux_output *go;
		wl_list_for_each(go, &server->outputs, link) {
			if (go->wlr_output->name == NULL) {
				continue;
			}
			struct wlr_box ob;
			wlr_output_layout_get_box(server->output_layout,
				go->wlr_output, &ob);
			int s = go->wlr_output->scale > 1 ?
				(int)go->wlr_output->scale : 1;
			if (go->topbar_node != NULL &&
					(go->topbar_node->node.x != ob.x ||
					 go->topbar_node->node.y != ob.y)) {
				wlr_log(WLR_ERROR, "outputs-panel-test: FAIL "
					"topbar-follow: %s topbar at (%d,%d), box at (%d,%d)",
					go->wlr_output->name, go->topbar_node->node.x,
					go->topbar_node->node.y, ob.x, ob.y);
				return 0;
			}
			if (go->topbar_buffer != NULL &&
					go->topbar_buffer_w != ob.width * s) {
				wlr_log(WLR_ERROR, "outputs-panel-test: FAIL "
					"topbar-size: %s topbar buffer %dpx, box %dpx "
					"(scale %d)", go->wlr_output->name,
					go->topbar_buffer_w, ob.width, s);
				return 0;
			}
			if (go->bg_node != NULL &&
					(go->bg_node->node.x != ob.x ||
					 go->bg_node->node.y != ob.y)) {
				wlr_log(WLR_ERROR, "outputs-panel-test: FAIL bg-follow: "
					"%s background at (%d,%d), box at (%d,%d)",
					go->wlr_output->name, go->bg_node->node.x,
					go->bg_node->node.y, ob.x, ob.y);
				return 0;
			}
		}
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK topbar (follows moves, "
		"resized for rotation)");

	/* m: cycle the mode. Headless outputs have an empty mode list, so
	 * the panel must handle it gracefully: no crash, panel stays active */
	outputs_panel_handle_key(server, XKB_KEY_m);
	if (!p->active) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL mode: panel closed");
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK mode (cycled, status '%s')",
		p->status);

	/* mode round-trip: seed a WxH section into the config, re-open the
	 * panel, and verify the section survives an edit */
	FILE *f = fopen(server->config_path, "w");
	if (f == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL mode: cannot write "
			"config");
		return 0;
	}
	fprintf(f, "outputs = HEADLESS-1@0x0:1920x1080,HEADLESS-2@1280x0\n");
	fclose(f);
	outputs_panel_hide(server);
	outputs_panel_show(server);
	if (!p->active || p->num_entries != 2) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL mode: re-show "
			"(active=%d, entries=%d)", p->active, p->num_entries);
		return 0;
	}
	if (p->entries[0].mode_w != 1920 || p->entries[0].mode_h != 1080) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL mode: WxH section "
			"not parsed (%dx%d)", p->entries[0].mode_w,
			p->entries[0].mode_h);
		return 0;
	}
	/* r: transform -1 -> normal: the commit must keep the mode section */
	outputs_panel_handle_key(server, XKB_KEY_r);
	char spec[2048];
	if (!outputs_config_read(server->config_path, spec, sizeof(spec)) ||
			strstr(spec, "HEADLESS-1@0x0:1920x1080:normal") == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL mode: section lost "
			"on edit ('%s')", spec);
		return 0;
	}
	/* r: -> 90, live transform follows */
	outputs_panel_handle_key(server, XKB_KEY_r);
	if (!outputs_config_read(server->config_path, spec, sizeof(spec)) ||
			strstr(spec, "HEADLESS-1@0x0:1920x1080:90") == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL transform: config "
			"lacks 1920x1080:90 ('%s')", spec);
		return 0;
	}
	struct wlr_output *w1 = NULL;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->name != NULL &&
				strcmp(o->wlr_output->name, "HEADLESS-1") == 0) {
			w1 = o->wlr_output;
		}
	}
	if (w1 == NULL || (int)w1->transform != 1) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL transform: "
			"HEADLESS-1 transform=%d, want 1 (90)",
			w1 != NULL ? (int)w1->transform : -1);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK mode+transform (section "
		"kept, 90 applied)");

	/* d: disable HEADLESS-1: one live output left, config has @off */
	outputs_panel_handle_key(server, XKB_KEY_d);
	n = outputs_sorted_by_x(server, outs, boxes, 16);
	if (n != 1 || outs[0]->name == NULL ||
			strcmp(outs[0]->name, "HEADLESS-2") != 0) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL disable: n=%d "
			"first=%s, want 1 (HEADLESS-2)",
			n, n > 0 && outs[0]->name != NULL ? outs[0]->name : "(none)");
		return 0;
	}
	if (!outputs_config_read(server->config_path, spec, sizeof(spec)) ||
			strstr(spec, "HEADLESS-1@off") == NULL) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL disable: config "
			"lacks HEADLESS-1@off ('%s')", spec);
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK disable (@off persisted)");

	/* Down: selection moves to HEADLESS-2; Escape: close */
	outputs_panel_handle_key(server, XKB_KEY_Down);
	if (p->selected != 1) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL select: "
			"selected=%d, want 1", p->selected);
		return 0;
	}
	outputs_panel_handle_key(server, XKB_KEY_Escape);
	if (p->active) {
		wlr_log(WLR_ERROR, "outputs-panel-test: FAIL close (still active)");
		return 0;
	}
	wlr_log(WLR_INFO, "outputs-panel-test: OK (show, move, mode, "
		"transform, disable, close)");
	return 0;
}
