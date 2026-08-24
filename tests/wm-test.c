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
		if (!topbar_lists(dst, t) || topbar_lists(src, t)) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL topbar lists after drag (dst %s, src %s)",
				topbar_lists(dst, t) ? "yes" : "no",
				topbar_lists(src, t) ? "yes" : "no");
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
		if (!topbar_lists(src, t) || topbar_lists(dst, t)) {
			wlr_log(WLR_ERROR, "xmondrag-test: FAIL topbar lists after resize (src %s, dst %s)",
				topbar_lists(src, t) ? "yes" : "no",
				topbar_lists(dst, t) ? "yes" : "no");
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
