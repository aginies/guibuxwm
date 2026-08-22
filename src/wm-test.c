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
	struct wlr_surface *surface = t->xdg_toplevel->base->surface;
	wlr_seat_pointer_notify_enter(server->seat, surface, 0.0, 0.0);
	wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, NULL);
	wlr_log(WLR_INFO, "psel-test: enter sent");
	return 0;
}
