#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <stdlib.h>

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
