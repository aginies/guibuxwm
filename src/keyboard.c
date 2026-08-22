#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <stdlib.h>
#include <unistd.h>

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
	wlr_log(WLR_INFO, "spawned terminal (%s) pid %d", server->term_cmd, pid);
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
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		}
		break;
	case GUIBUX_ACT_FULLSCREEN:
		if (toplevel != NULL) {
			set_fullscreen(toplevel, !toplevel->is_fullscreen);
		}
		break;
	case GUIBUX_ACT_TILE:
		if (toplevel != NULL) {
			struct guibux_output *o = guibux_output_for(server,
				toplevel_output_for(toplevel));
			if (o != NULL) {
				o->tile_mode = (o->tile_mode + 1) % 3;
				retile_output(o);
				wlr_log(WLR_INFO, "tile mode on %s: %s",
					o->wlr_output->name ? o->wlr_output->name : "(unknown)",
					o->tile_mode == GUIBUX_TILE_FREE ? "free"
					: o->tile_mode == GUIBUX_TILE_SPLIT ? "split"
					: "main+stack");
			}
		}
		break;
	case GUIBUX_ACT_LAUNCHER:
		launcher_show(server);
		break;
	case GUIBUX_ACT_FOCUS_NEXT: {
		struct guibux_toplevel *next = NULL;
		struct guibux_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t == toplevel) {
				continue;
			}
			if (toplevel_visible(t)) {
				next = t;
				break;
			}
		}
		if (next != NULL) {
			focus_toplevel(next);
		}
		break;
	}
	case GUIBUX_ACT_QUIT:
		wl_display_terminate(server->wl_display);
		break;
	case GUIBUX_ACT_SWITCH_WS: {
		struct wlr_output *out = toplevel != NULL
			? toplevel_output_for(toplevel) : output_at_cursor(server);
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
	}
}

// ---------------------------------------------------------------------------
// Keybind table
// ---------------------------------------------------------------------------

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
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Tab, GUIBUX_ACT_FOCUS_NEXT, 0);
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
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_q,
		GUIBUX_ACT_QUIT, 0);
}

bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym,
		uint32_t modifiers) {
	uint32_t mods = modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT |
		WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL);
	struct guibux_toplevel *toplevel = wl_list_empty(&server->toplevels) ? NULL :
		wl_container_of(server->toplevels.next, toplevel, link);
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

void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct guibux_server *server = keyboard->server;
	struct wlr_seat *seat = server->seat;

	struct wlr_keyboard_key_event *event = data;
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (server->launcher.active) {
				handled = launcher_handle_key(server, syms[i]);
			} else {
				handled = handle_keybinding(server, syms[i], modifiers);
			}
			if (handled) {
				break;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
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

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
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
