#include "guibuxwm.h"

static int screensaver_timer_cb(void *data);
static void handle_inhibit_new(struct wl_listener *listener, void *data);

void screensaver_init(struct guibux_server *server) {
	struct guibux_screensaver *ss = &server->screensaver;
	ss->server = server;
	ss->dpms_off = false;

	ss->idle_inhibit = wlr_idle_inhibit_v1_create(server->wl_display);
	if (ss->idle_inhibit) {
		wl_list_init(&ss->idle_inhibit->inhibitors);
		ss->inhibit_new.notify = handle_inhibit_new;
		wl_signal_add(&ss->idle_inhibit->events.new_inhibitor, &ss->inhibit_new);
	}

	ss->idle_notify = wlr_idle_notifier_v1_create(server->wl_display);

	ss->timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->wl_display),
		screensaver_timer_cb, server);
}

void screensaver_destroy(struct guibux_server *server) {
	struct guibux_screensaver *ss = &server->screensaver;
	if (ss->timer) {
		wl_event_source_remove(ss->timer);
		ss->timer = NULL;
	}
}

void screensaver_set_timeout(struct guibux_screensaver *ss, int seconds) {
	ss->timeout = seconds;
	if (seconds == 0) {
		ss->active = false;
		ss->dpms_off = false;
		return;
	}
	if (!ss->dpms_off) {
		ss->active = true;
	}
}

static bool is_inhibited(struct guibux_screensaver *ss) {
	if (!ss->idle_inhibit) {
		return false;
	}
	struct wlr_idle_inhibitor_v1 *inh;
	wl_list_for_each(inh, &ss->idle_inhibit->inhibitors, link) {
		if (inh->surface && inh->surface->mapped) {
			return true;
		}
	}
	return false;
}

static bool ui_active(struct guibux_server *server) {
	return server->launcher.active ||
		server->switcher.active ||
		server->help.active;
}

void screensaver_notify_activity(struct guibux_server *server) {
	struct guibux_screensaver *ss = &server->screensaver;

	if (ss->idle_notify) {
		wlr_idle_notifier_v1_notify_activity(ss->idle_notify, server->seat);
	}

	if (ss->dpms_off) {
		screensaver_turn_on(server);
		ss->dpms_off = false;
		ss->active = true;
		wl_event_source_timer_update(ss->timer, ss->timeout * 1000);
		return;
	}

	if (ss->timeout <= 0 || !ss->active || is_inhibited(ss) || ui_active(server)) {
		return;
	}

	wl_event_source_timer_update(ss->timer, ss->timeout * 1000);
}

static void set_outputs_power(struct guibux_server *server, bool on) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		wlr_output_commit_state(o->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
}

void screensaver_turn_off(struct guibux_server *server) {
	struct guibux_screensaver *ss = &server->screensaver;
	ss->dpms_off = true;
	ss->active = false;
	wlr_log(WLR_INFO, "screensaver: DPMS off");
	screensaver_update_inhibited(server);
	set_outputs_power(server, false);
}

void screensaver_turn_on(struct guibux_server *server) {
	wlr_log(WLR_INFO, "screensaver: DPMS on");
	set_outputs_power(server, true);
	screensaver_update_inhibited(server);
}

void screensaver_update_inhibited(struct guibux_server *server) {
	struct guibux_screensaver *ss = &server->screensaver;
	if (!ss->idle_notify) {
		return;
	}
	wlr_idle_notifier_v1_set_inhibited(ss->idle_notify, is_inhibited(ss));
}

static int screensaver_timer_cb(void *data) {
	struct guibux_server *server = data;
	struct guibux_screensaver *ss = &server->screensaver;

	if (!ss->active || is_inhibited(ss) || ui_active(server)) {
		return 0;
	}

	screensaver_turn_off(server);
	return 0;
}

static void handle_inhibit_new(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(listener, server, screensaver.inhibit_new);
	(void)data;

	struct guibux_screensaver *ss = &server->screensaver;
	if (ss->dpms_off) {
		screensaver_turn_on(server);
		ss->dpms_off = false;
		ss->active = true;
		wl_event_source_timer_update(ss->timer, ss->timeout * 1000);
	} else {
		screensaver_update_inhibited(server);
	}
}
