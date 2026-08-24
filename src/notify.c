#define _GNU_SOURCE
#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/*
 * notify - desktop notifications via org.freedesktop.Notifications (D-Bus)
 *
 * The compositor registers as the session-bus notification daemon (like
 * dunst/mako). A worker thread owns the bus connection and dispatches the
 * method calls (Notify/Close/GetCapabilities/GetServerInformation); the
 * notification list is published under n->lock and the main loop picks up
 * changes through notify_consume_dirty() from topbar_tick.
 *
 * UI:
 *   - topbar indicator: bell + pending count (notify_draw_indicator)
 *   - list panel: opened from the indicator; click a row to dismiss it,
 *     "Clear all" button in the header, Escape closes
 *
 * Degrades gracefully: no session bus, or another daemon already owns the
 * name, leaves the daemon off but the store/indicator/panel still work for
 * internally added notifications (tests).
 */

#define NOTIF_PANEL_W 420
#define NOTIF_PANEL_HEADER_H 32
#define NOTIF_PANEL_ROW_H 44
#define NOTIF_PANEL_PAD 10
#define NOTIF_PANEL_MARGIN 8

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

uint32_t notify_add(struct guibux_notify *n, const char *app_name,
		const char *summary, const char *body, int32_t expire) {
	pthread_mutex_lock(&n->lock);
	if (n->count >= NOTIF_MAX) {
		n->count--;
	}
	memmove(&n->items[1], &n->items[0], n->count * sizeof(n->items[0]));
	struct guibux_notification *it = &n->items[0];
	it->id = n->next_id++;
	snprintf(it->app_name, sizeof(it->app_name), "%s",
		app_name ? app_name : "");
	snprintf(it->summary, sizeof(it->summary), "%s",
		summary ? summary : "");
	snprintf(it->body, sizeof(it->body), "%s", body ? body : "");
	it->expire = expire;
	it->created = time(NULL);
	n->count++;
	n->dirty = true;
	uint32_t id = it->id;
	pthread_mutex_unlock(&n->lock);
	return id;
}

void notify_close(struct guibux_notify *n, uint32_t id) {
	pthread_mutex_lock(&n->lock);
	for (int i = 0; i < n->count; i++) {
		if (n->items[i].id == id) {
			memmove(&n->items[i], &n->items[i + 1],
				(n->count - i - 1) * sizeof(n->items[0]));
			n->count--;
			break;
		}
	}
	n->dirty = true;
	pthread_mutex_unlock(&n->lock);
}

void notify_clear(struct guibux_notify *n) {
	pthread_mutex_lock(&n->lock);
	n->count = 0;
	n->dirty = true;
	pthread_mutex_unlock(&n->lock);
}

/* spec 1.3 replaces_id: update an existing notification in place
 * (no flicker, list position kept). Returns false when the id is
 * unknown, so the caller falls back to adding a new notification */
bool notify_replace(struct guibux_notify *n, uint32_t id,
		const char *app_name, const char *summary, const char *body,
		int32_t expire) {
	pthread_mutex_lock(&n->lock);
	bool found = false;
	for (int i = 0; i < n->count; i++) {
		if (n->items[i].id == id) {
			snprintf(n->items[i].app_name,
				sizeof(n->items[i].app_name), "%s",
				app_name ? app_name : "");
			snprintf(n->items[i].summary,
				sizeof(n->items[i].summary), "%s",
				summary ? summary : "");
			snprintf(n->items[i].body, sizeof(n->items[i].body),
				"%s", body ? body : "");
			n->items[i].expire = expire;
			found = true;
			break;
		}
	}
	n->dirty = true;
	pthread_mutex_unlock(&n->lock);
	return found;
}

int notify_count(struct guibux_notify *n) {
	pthread_mutex_lock(&n->lock);
	int c = n->count;
	pthread_mutex_unlock(&n->lock);
	return c;
}

int notify_snapshot(struct guibux_notify *n, struct guibux_notification *out,
		int max) {
	pthread_mutex_lock(&n->lock);
	int c = n->count < max ? n->count : max;
	memcpy(out, n->items, (size_t)c * sizeof(out[0]));
	pthread_mutex_unlock(&n->lock);
	return c;
}

bool notify_get_by_id(struct guibux_notify *n, uint32_t id,
		struct guibux_notification *out) {
	pthread_mutex_lock(&n->lock);
	bool found = false;
	for (int i = 0; i < n->count; i++) {
		if (n->items[i].id == id) {
			*out = n->items[i];
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&n->lock);
	return found;
}

bool notify_consume_dirty(struct guibux_notify *n) {
	pthread_mutex_lock(&n->lock);
	bool d = n->dirty;
	n->dirty = false;
	pthread_mutex_unlock(&n->lock);
	return d;
}

// ---------------------------------------------------------------------------
// D-Bus daemon (worker thread)
// ---------------------------------------------------------------------------

static DBusHandlerResult notify_message_func(DBusConnection *conn,
		DBusMessage *msg, void *user_data) {
	struct guibux_notify *n = user_data;
	const char *path = dbus_message_get_path(msg);
	if (path == NULL ||
			strcmp(path, "/org/freedesktop/Notifications") != 0) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	const char *member = dbus_message_get_member(msg);
	if (member == NULL) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strcmp(member, "Notify") == 0) {
		/* spec 1.3: app_name (s), replaces_id (u), app_icon (s),
		 * summary (s), body (s), actions (as), hints (a{sv}),
		 * expire_timeout (i)
		 * older spec: same without replaces_id. Both are accepted, the
		 * type of the second argument decides which (Chromium/Electron
		 * apps such as Signal send the 1.3 form; a parser that expects
		 * only the old form stalls on the uint32 and loses all strings) */
		DBusMessageIter iter;
		const char *app_name = "", *summary = "", *body = "";
		uint32_t replaces_id = 0;
		int32_t expire = 0;
		dbus_message_iter_init(msg, &iter);
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&iter, &app_name);
			dbus_message_iter_next(&iter);
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
			dbus_message_iter_get_basic(&iter, &replaces_id);
			dbus_message_iter_next(&iter);
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_next(&iter); /* app_icon */
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&iter, &summary);
			dbus_message_iter_next(&iter);
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&iter, &body);
			dbus_message_iter_next(&iter);
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_next(&iter); /* actions */
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_next(&iter); /* hints */
		}
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&iter, &expire);
		}
		wlr_log(WLR_INFO, "notify: app='%s' summary='%s' body='%s' expire=%d replaces=%u",
			app_name, summary, body, expire, replaces_id);
		uint32_t id;
		if (replaces_id != 0 &&
				notify_replace(n, replaces_id, app_name, summary, body, expire)) {
			id = replaces_id;
		} else {
			id = notify_add(n, app_name, summary, body, expire);
			/* wake the main loop: a brand-new notification pops the
			 * panel (a replace only updates an existing row). A byte
			 * write to the pipe is atomic and thread-safe */
			struct guibux_server *server = wl_container_of(n, server, notify);
			if (server->notify_pipe[1] >= 0) {
				ssize_t rc;
				do {
					rc = write(server->notify_pipe[1], "n", 1);
				} while (rc < 0 && errno == EINTR);
			}
		}
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply != NULL) {
			dbus_message_append_args(reply, DBUS_TYPE_UINT32, &id,
				DBUS_TYPE_INVALID);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "CloseNotification") == 0) {
		uint32_t id = 0;
		DBusMessageIter iter;
		dbus_message_iter_init(msg, &iter);
		if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
			dbus_message_iter_get_basic(&iter, &id);
		}
		notify_close(n, id);
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply != NULL) {
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "GetCapabilities") == 0) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply != NULL) {
			DBusMessageIter iter, sub;
			const char *caps[] = { "body", NULL };
			/* _init_append: a writer iterator; plain _init
			 * creates a reader and open_container asserts */
			dbus_message_iter_init_append(reply, &iter);
			dbus_message_iter_open_container(&iter,
				DBUS_TYPE_ARRAY, "s", &sub);
			for (int i = 0; caps[i] != NULL; i++) {
				dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING,
					&caps[i]);
			}
			dbus_message_iter_close_container(&iter, &sub);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "GetServerInformation") == 0) {
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (reply != NULL) {
		/* spec 1.3: (name, vendor, version, spec_version). Older
		 * clients read the third value as the spec version, so it
		 * must be a valid spec version there, not the project version */
			const char *name = "guibuxwm";
			const char *vendor = "guibuxwm";
			const char *version = "1.3";
			const char *spec_version = "1.3";
			dbus_message_append_args(reply,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_STRING, &vendor,
				DBUS_TYPE_STRING, &version,
				DBUS_TYPE_STRING, &spec_version,
				DBUS_TYPE_INVALID);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable notify_vtable = {
	.unregister_function = NULL,
	.message_function = notify_message_func,
};

static void *notify_worker(void *data) {
	struct guibux_server *server = data;
	struct guibux_notify *n = &server->notify;
	DBusError err = DBUS_ERROR_INIT;

	n->session_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (n->session_bus == NULL) {
		wlr_log(WLR_ERROR, "notify: no session bus: %s",
			dbus_error_is_set(&err) ? err.message : "unknown");
		dbus_error_free(&err);
		n->daemon = false;
	} else {
		int rc = dbus_bus_request_name(n->session_bus,
			"org.freedesktop.Notifications",
			DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
		if (rc == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
			n->daemon = true;
			dbus_connection_register_object_path(n->session_bus,
				"/org/freedesktop/Notifications",
				&notify_vtable, n);
			wlr_log(WLR_INFO,
				"notify: registered as notification daemon");
		} else {
			n->daemon = false;
			wlr_log(WLR_INFO,
				"notify: org.freedesktop.Notifications owned by "
				"another daemon, indicator only");
			if (dbus_error_is_set(&err)) {
				dbus_error_free(&err);
			}
		}
	}

	while (true) {
		if (n->session_bus != NULL) {
			/* blocks up to 100ms for incoming method calls */
			dbus_connection_read_write_dispatch(n->session_bus, 100);
		} else {
			/* no bus: sleep so the loop does not spin */
			pthread_mutex_lock(&n->lock);
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&n->wake, &n->lock, &ts);
			pthread_mutex_unlock(&n->lock);
		}
		pthread_mutex_lock(&n->lock);
		bool running = n->worker_running;
		pthread_mutex_unlock(&n->lock);
		if (!running) {
			break;
		}
	}

	if (n->session_bus != NULL) {
		if (n->daemon) {
			dbus_connection_unregister_object_path(n->session_bus,
				"/org/freedesktop/Notifications");
		}
		/* dbus_bus_get returns the shared connection: unref only,
		 * closing it aborts (libdbus) */
		dbus_connection_unref(n->session_bus);
		n->session_bus = NULL;
	}
	return NULL;
}

void notify_init(struct guibux_server *server) {
	struct guibux_notify *n = &server->notify;
	n->session_bus = NULL;
	n->daemon = false;
	n->next_id = 1;
	n->count = 0;
	n->dirty = false;
	n->worker = 0;

	pthread_mutex_init(&n->lock, NULL);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&n->wake, &ca);
	pthread_condattr_destroy(&ca);
	n->worker_running = true;
	if (pthread_create(&n->worker, NULL, notify_worker, server) != 0) {
		wlr_log(WLR_ERROR, "notify: failed to start worker thread");
		n->worker_running = false;
		n->worker = 0;
	}
}

void notify_destroy(struct guibux_server *server) {
	struct guibux_notify *n = &server->notify;
	notify_panel_hide(server);
	if (n->worker) {
		pthread_mutex_lock(&n->lock);
		n->worker_running = false;
		pthread_cond_signal(&n->wake);
		pthread_mutex_unlock(&n->lock);
		pthread_join(n->worker, NULL);
		n->worker = 0;
	}
	pthread_cond_destroy(&n->wake);
	pthread_mutex_destroy(&n->lock);
}

// ---------------------------------------------------------------------------
// Topbar indicator
// ---------------------------------------------------------------------------

/* small bell centered at (x, cy), total size s, in device pixels */
static void draw_bell(cairo_t *cr, double x, double cy, double s,
		uint32_t color) {
	set_color(cr, color);
	double r = s * 0.30;
	double top = cy - s * 0.42;
	double dome_cy = top + r;
	double body_bottom = cy + s * 0.28;
	double flare = s * 0.46;

	cairo_new_path(cr);
	cairo_arc(cr, x, dome_cy, r, M_PI, 2 * M_PI);
	cairo_line_to(cr, x + r, body_bottom - s * 0.12);
	cairo_line_to(cr, x + flare, body_bottom);
	cairo_line_to(cr, x - flare, body_bottom);
	cairo_line_to(cr, x - r, body_bottom - s * 0.12);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_arc(cr, x, body_bottom + s * 0.10, s * 0.09, 0, 2 * M_PI);
	cairo_fill(cr);
}

/* indicator layout (logical px): 8 pad | 12 bell | 6 gap | count | 8 pad */
#define NOTIF_IND_PAD 8
#define NOTIF_IND_BELL 12
#define NOTIF_IND_GAP 6

int notify_indicator_width(FT_Face face, int scale, int count) {
	if (count <= 0) {
		return 0;
	}
	char cnt[16];
	snprintf(cnt, sizeof(cnt), "%d", count);
	return 2 * NOTIF_IND_PAD + NOTIF_IND_BELL + NOTIF_IND_GAP +
		guibux_text_width(face, cnt) / scale;
}

void notify_draw_indicator(cairo_surface_t *cs, cairo_t *cr, FT_Face face,
		int x, int baseline, int scale, int count, uint32_t color) {
	if (count <= 0) {
		return;
	}
	int font_px = face->size->metrics.height / 64;
	double cy = baseline - font_px * 35 / 100.0;
	draw_bell(cr, (x + NOTIF_IND_PAD + NOTIF_IND_BELL / 2) * scale,
		cy, NOTIF_IND_BELL * scale, color);
	char cnt[16];
	snprintf(cnt, sizeof(cnt), "%d", count);
	launcher_draw_text_on_surface(cs, face, cnt,
		(x + NOTIF_IND_PAD + NOTIF_IND_BELL + NOTIF_IND_GAP) * scale,
		baseline, color);
}

// ---------------------------------------------------------------------------
// List panel
// ---------------------------------------------------------------------------

/* truncate a UTF-8 string so its rendered width fits max_w (device px),
 * appending "..." when cut */
static void truncate_to_width(FT_Face face, const char *src, char *dst,
		size_t dst_size, int max_w) {
	if (guibux_text_width(face, src) <= max_w) {
		snprintf(dst, dst_size, "%s", src);
		return;
	}
	int cps = 0;
	const char *p = src;
	while (*p) {
		utf8_next(&p);
		cps++;
	}
	for (int t = cps; t >= 1; t--) {
		utf8_truncate(src, dst, dst_size, t);
		size_t n = strlen(dst);
		snprintf(dst + n, dst_size - n, "...");
		if (guibux_text_width(face, dst) <= max_w) {
			return;
		}
	}
	utf8_truncate(src, dst, dst_size, 1);
	size_t n = strlen(dst);
	snprintf(dst + n, dst_size - n, "...");
}

void notify_panel_render(struct guibux_server *server) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (p->buffer == NULL || server->launcher.face == NULL) {
		return;
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(p->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(p->buffer);
		return;
	}

	int w = p->box_w * p->box_scale;
	int hgt = p->box_h * p->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, hgt, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, p->box_scale);
	cairo_rectangle(cr, p->box_scale / 2.0, p->box_scale / 2.0,
		w - p->box_scale, hgt - p->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int sc = p->box_scale;
	int font_px = server->topbar_font_size * sc;
	int small_px = (server->topbar_font_size * 3 / 4) * sc;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	int pad = NOTIF_PANEL_PAD * sc;
	int header_h = NOTIF_PANEL_HEADER_H * sc;

	/* header: title left, "Clear all" button right */
	char title[32];
	snprintf(title, sizeof(title), "Notifications (%d)",
		notify_count(&server->notify));
	int tb = pad + header_h / 2 + font_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face, title, pad, tb,
		server->color_text);

	const char *clear_label = "Clear all";
	/* device px: the text width comes back scaled at the panel font size */
	int cw = guibux_text_width(face, clear_label) + 16 * sc;
	int ch = 22 * sc;
	int cx = w - pad - cw;
	int cy = pad + (header_h - ch) / 2;
	p->clear_x = cx / sc;
	p->clear_y = cy / sc;
	p->clear_w = cw / sc;
	p->clear_h = ch / sc;
	set_color(cr, server->color_highlight);
	topbar_rounded_rect(cr, cx, cy, cw, ch, 5 * sc);
	cairo_fill(cr);
	int cb = cy + ch / 2 + font_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face, clear_label, cx + 8 * sc, cb,
		server->color_text);

	/* separator under the header */
	set_color(cr, server->color_border);
	cairo_rectangle(cr, 0, pad + header_h, w, sc);
	cairo_fill(cr);

	/* rows: summary (text) + "app: body" (dim), newest first */
	struct guibux_notification items[NOTIF_PANEL_MAX];
	int n = notify_snapshot(&server->notify, items, p->box_rows);
	int row_h = NOTIF_PANEL_ROW_H * sc;
	int y = pad + header_h + sc;
	p->num_rows = 0;
	if (n == 0) {
		int mb = y + row_h / 2 + font_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, "No notifications", pad, mb,
			server->color_dim);
	}
	for (int i = 0; i < n; i++) {
		int ry = y + i * row_h;
		p->row_y[i] = ry / sc;
		p->row_ids[i] = items[i].id;
		p->num_rows = i + 1;

		char sbuf[128];
		truncate_to_width(face, items[i].summary, sbuf, sizeof(sbuf),
			w - 2 * pad);
		int sb = ry + 16 * sc + font_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, sbuf, pad, sb,
			server->color_text);

		char lbuf[320];
		if (items[i].app_name[0] != '\0' && items[i].body[0] != '\0') {
			int len = snprintf(lbuf, sizeof(lbuf), "%s",
				items[i].app_name);
			if (len > 0 && (size_t)len < sizeof(lbuf))
				snprintf(lbuf + len, sizeof(lbuf) - len, ": %s",
					items[i].body);
		} else if (items[i].body[0] != '\0') {
			snprintf(lbuf, sizeof(lbuf), "%s", items[i].body);
		} else {
			snprintf(lbuf, sizeof(lbuf), "%s", items[i].app_name);
		}
		char lbuf2[320];
		FT_Set_Pixel_Sizes(face, 0, small_px);
		truncate_to_width(face, lbuf, lbuf2, sizeof(lbuf2), w - 2 * pad);
		int lb = ry + 34 * sc + small_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, lbuf2, pad, lb,
			server->color_dim);
		FT_Set_Pixel_Sizes(face, 0, font_px);

		if (i + 1 < n) {
			set_color(cr, server->color_border);
			cairo_rectangle(cr, pad, ry + row_h - sc / 2, w - 2 * pad, sc);
			cairo_fill(cr);
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(p->buffer);
	if (p->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(p->scene_node, p->buffer);
	}
	if (p->output != NULL) {
		wlr_output_schedule_frame(p->output);
	}
}

void notify_panel_free_node(struct guibux_server *server);

void notify_panel_show(struct guibux_server *server,
		struct wlr_output *output) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (p->active) {
		return;
	}
	tooltip_hide(server);
	/* a slide-out may still be in flight: cancel it and start fresh */
	if (p->hiding) {
		effects_cancel_node(server, &p->scene_node->node);
		notify_panel_free_node(server);
	}
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = output->scale > 1 ? (int)output->scale : 1;

	int count = notify_count(&server->notify);
	int rows = count < 1 ? 1 : count;
	if (rows > NOTIF_PANEL_MAX) {
		rows = NOTIF_PANEL_MAX;
	}
	int avail = eh - server->topbar_height - 12;
	int fit = (avail - 2 * NOTIF_PANEL_PAD - NOTIF_PANEL_HEADER_H) /
		NOTIF_PANEL_ROW_H;
	if (fit < rows) {
		rows = fit;
	}
	if (rows < 1) {
		rows = 1;
	}
	p->box_rows = rows;

	int bw = NOTIF_PANEL_W;
	if (bw > ew - 20) {
		bw = ew - 20;
	}
	p->box_w = bw;
	p->box_h = 2 * NOTIF_PANEL_PAD + NOTIF_PANEL_HEADER_H +
		rows * NOTIF_PANEL_ROW_H;
	p->box_scale = scale;
	p->box_x = ew - bw - NOTIF_PANEL_MARGIN;
	p->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	p->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, p->box_h * scale, &format);
	if (p->buffer == NULL) {
		return;
	}
	p->scene_node = wlr_scene_buffer_create(&server->scene->tree, p->buffer);
	if (p->scene_node == NULL) {
		wlr_buffer_drop(p->buffer);
		p->buffer = NULL;
		return;
	}
	wlr_scene_buffer_set_dest_size(p->scene_node, bw, p->box_h);
	wlr_scene_node_set_position(&p->scene_node->node,
		box.x + p->box_x, box.y + server->topbar_height);
	wlr_scene_node_raise_to_top(&p->scene_node->node);
	topbar_raise_all(server);

	p->active = true;
	notify_panel_render(server);
	effects_notify_show(server, output);
}

void notify_panel_free_node(struct guibux_server *server) {
	struct guibux_notif_panel *p = &server->notify_panel;
	p->hiding = false;
	p->output = NULL;
	if (p->scene_node != NULL) {
		wlr_scene_node_destroy(&p->scene_node->node);
		p->scene_node = NULL;
	}
	if (p->buffer != NULL) {
		wlr_buffer_drop(p->buffer);
		p->buffer = NULL;
	}
}

/* completion of the slide-out animation */
void notify_panel_hide_done(void *data) {
	struct guibux_server *server = data;
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!p->hiding) {
		return;
	}
	notify_panel_free_node(server);
}

void notify_panel_hide(struct guibux_server *server) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!p->active) {
		return;
	}
	notify_autohide_cancel(server);
	p->active = false;
	p->num_rows = 0;
	if (effects_notify_hide(server)) {
		p->hiding = true;
		return;
	}
	notify_panel_free_node(server);
}

// ---------------------------------------------------------------------------
// Auto-hide: a new notification pops the panel like an indicator click;
// it closes after a delay unless the user keeps interacting with it
// ---------------------------------------------------------------------------

#define NOTIF_AUTOHIDE_MS 2000

void notify_autohide_start(struct guibux_server *server) {
	if (server->notify_autohide_timer != NULL) {
		wl_event_source_timer_update(server->notify_autohide_timer,
			NOTIF_AUTOHIDE_MS);
	}
}

void notify_autohide_cancel(struct guibux_server *server) {
	if (server->notify_autohide_timer != NULL) {
		wl_event_source_timer_update(server->notify_autohide_timer, 0);
	}
}

int notify_autohide_run(void *data) {
	struct guibux_server *server = data;
	if (server->notify_panel.active) {
		notify_panel_hide(server);
	}
	return 0;
}

/* main-loop side of the worker's new-notification wake-up: show the panel
 * on the output under the cursor (or refresh it if already open) and arm
 * the auto-hide */
int notify_new_readable(int fd, uint32_t mask, void *data) {
	struct guibux_server *server = data;
	struct guibux_notif_panel *p = &server->notify_panel;
	/* drain the pipe: several notifications may have piled up */
	char buf[64];
	while (read(fd, buf, sizeof buf) > 0) {
	}
	if (notify_count(&server->notify) == 0) {
		return 0;
	}
	if (p->active) {
		notify_panel_render(server);
	} else {
		struct wlr_output *out = output_at_cursor(server);
		if (out == NULL) {
			return 0;
		}
		notify_panel_show(server, out);
	}
	notify_autohide_start(server);
	return 0;
}

bool notify_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!p->active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		notify_panel_hide(server);
		return true;
	case XKB_KEY_Delete:
		notify_clear(&server->notify);
		if (notify_count(&server->notify) == 0) {
			notify_panel_hide(server);
		} else {
			notify_panel_render(server);
		}
		return true;
	default:
		break;
	}
	/* modal: swallow the rest so keys do not leak to focused windows */
	return true;
}

static bool panel_box_at(struct guibux_server *server, double lx, double ly,
		double *rx, double *ry) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!p->active || p->output == NULL) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, p->output, &box);
	/* the panel is right-aligned: its left edge is box_x, not the
	 * output origin (hit tests against box.x missed the visible panel) */
	if (lx < box.x + p->box_x || lx >= box.x + p->box_x + p->box_w ||
			ly < box.y + server->topbar_height ||
			ly >= box.y + server->topbar_height + p->box_h) {
		return false;
	}
	if (rx) {
		*rx = lx - (box.x + p->box_x);
	}
	if (ry) {
		*ry = ly - (box.y + server->topbar_height);
	}
	return true;
}

bool notify_panel_contains(struct guibux_server *server, double lx, double ly) {
	return panel_box_at(server, lx, ly, NULL, NULL);
}

uint32_t notify_panel_row_at(struct guibux_server *server, double lx, double ly) {
	struct guibux_notif_panel *p = &server->notify_panel;
	double rx, ry;
	if (!panel_box_at(server, lx, ly, &rx, &ry)) {
		return 0;
	}
	for (int i = 0; i < p->num_rows; i++) {
		if (ry >= p->row_y[i] && ry < p->row_y[i] + NOTIF_PANEL_ROW_H) {
			return p->row_ids[i];
		}
	}
	return 0;
}

bool notify_panel_clear_at(struct guibux_server *server, double lx, double ly) {
	struct guibux_notif_panel *p = &server->notify_panel;
	double rx, ry;
	if (!panel_box_at(server, lx, ly, &rx, &ry)) {
		return false;
	}
	return rx >= p->clear_x && rx < p->clear_x + p->clear_w &&
		ry >= p->clear_y && ry < p->clear_y + p->clear_h;
}

// ---------------------------------------------------------------------------
// Test hook
// ---------------------------------------------------------------------------

/* send a Notify call over the session bus (blocking) and return the id
 * the daemon assigned; 0 on failure. spec13 selects the spec 1.3
 * signature (with replaces_id as second argument) */
static uint32_t test_send_notify(DBusConnection *c, bool spec13,
		uint32_t replaces_id, const char *summary, const char *body) {
	DBusMessage *m = dbus_message_new_method_call(
		"org.freedesktop.Notifications",
		"/org/freedesktop/Notifications",
		"org.freedesktop.Notifications", "Notify");
	if (m == NULL) {
		return 0;
	}
	const char *app = "guibux-dbus-test";
	const char *icon = "";
	int32_t expire = 0;
	uint32_t rid = replaces_id;
	if (spec13) {
		dbus_message_append_args(m,
			DBUS_TYPE_STRING, &app,
			DBUS_TYPE_UINT32, &rid,
			DBUS_TYPE_STRING, &icon,
			DBUS_TYPE_STRING, &summary,
			DBUS_TYPE_STRING, &body,
			DBUS_TYPE_INVALID);
	} else {
		dbus_message_append_args(m,
			DBUS_TYPE_STRING, &app,
			DBUS_TYPE_STRING, &icon,
			DBUS_TYPE_STRING, &summary,
			DBUS_TYPE_STRING, &body,
			DBUS_TYPE_INVALID);
	}
	DBusMessageIter iter, sub;
	dbus_message_iter_init_append(m, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &sub);
	dbus_message_iter_close_container(&iter, &sub);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub);
	dbus_message_iter_close_container(&iter, &sub);
	dbus_message_append_args(m, DBUS_TYPE_INT32, &expire, DBUS_TYPE_INVALID);
	DBusError err = DBUS_ERROR_INIT;
	DBusMessage *reply =
		dbus_connection_send_with_reply_and_block(c, m, 2000, &err);
	dbus_message_unref(m);
	uint32_t id = 0;
	if (reply != NULL) {
		DBusMessageIter ri;
		dbus_message_iter_init(reply, &ri);
		if (dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_UINT32) {
			dbus_message_iter_get_basic(&ri, &id);
		}
		dbus_message_unref(reply);
	} else if (dbus_error_is_set(&err)) {
		wlr_log(WLR_ERROR, "notify-test: dbus error: %s", err.message);
		dbus_error_free(&err);
	}
	return id;
}

static int64_t notify_test_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int notify_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_notify *n = &server->notify;
	static bool seeded = false;
	if (!seeded) {
		notify_add(n, "guibuxwm-test", "Test notification",
			"Body text for the notify test", 0);
		notify_add(n, "guibuxwm-test", "Second notification",
			"Second body for the notify test", 0);
		seeded = true;
		struct guibux_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			o->topbar_dirty = true;
			topbar_render(o);
		}
	}
	/* D-Bus round-trip: send Notify over the session bus in both
	 * signatures and verify the strings survive the parse (regression:
	 * the spec 1.3 signature with replaces_id used to stall the parser
	 * and produce empty rows). Skipped when there is no session bus or
	 * another daemon owns the name */
	static bool dbus_checked = false;
	static int64_t dbus_done_ms = 0;
	static bool dbus_skipped = false;
	static bool autohide_checked = false;
	static bool autohidden_checked = false;
	static bool autohide_done = false;
	if (!dbus_checked) {
		dbus_checked = true;
		if (!n->daemon) {
			wlr_log(WLR_INFO,
				"notify-test: SKIP dbus round-trip (not the daemon)");
		} else {
			DBusError derr = DBUS_ERROR_INIT;
			/* _get_private: dbus_bus_get returns the cached connection,
			 * which is the worker thread's connection here; two threads
			 * on one libdbus connection is undefined (the reply would be
			 * dispatched on the worker and the blocking call below would
			 * time out) */
			DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION,
				&derr);
			if (c == NULL) {
				wlr_log(WLR_INFO,
					"notify-test: SKIP dbus round-trip (no session bus)");
			} else {
				struct guibux_notification it;
				uint32_t id1 = test_send_notify(c, false, 0,
					"DBUS-OLD-SUMMARY", "DBUS-OLD-BODY");
				if (id1 == 0 || !notify_get_by_id(n, id1, &it) ||
						strcmp(it.app_name, "guibux-dbus-test") != 0 ||
						strcmp(it.summary, "DBUS-OLD-SUMMARY") != 0 ||
						strcmp(it.body, "DBUS-OLD-BODY") != 0) {
					wlr_log(WLR_ERROR,
						"notify-test: FAIL old-signature parse (id=%u)",
						id1);
					return 0;
				}
				uint32_t id2 = test_send_notify(c, true, 0,
					"DBUS-NEW-SUMMARY", "DBUS-NEW-BODY");
				if (id2 == 0 || id2 == id1 ||
						!notify_get_by_id(n, id2, &it) ||
						strcmp(it.summary, "DBUS-NEW-SUMMARY") != 0 ||
						strcmp(it.body, "DBUS-NEW-BODY") != 0) {
					wlr_log(WLR_ERROR,
						"notify-test: FAIL spec 1.3 parse (id=%u)", id2);
					return 0;
				}
				uint32_t id3 = test_send_notify(c, true, id2,
					"DBUS-REPLACED", "DBUS-REPLACED-BODY");
				if (id3 != id2 || !notify_get_by_id(n, id2, &it) ||
						strcmp(it.summary, "DBUS-REPLACED") != 0 ||
						strcmp(it.body, "DBUS-REPLACED-BODY") != 0) {
					wlr_log(WLR_ERROR,
						"notify-test: FAIL replaces_id (got=%u want=%u)",
						id3, id2);
					return 0;
				}
				wlr_log(WLR_INFO,
					"notify-test: dbus round-trip OK (old=%u new=%u replaced=%u)",
					id1, id2, id3);
				dbus_done_ms = notify_test_now_ms();
				dbus_connection_close(c);
				dbus_connection_unref(c);
			}
		}
	}
	/* auto-show + auto-hide: the D-Bus notification above must have popped
	 * the panel (worker -> pipe -> main loop), and the auto-hide must close
	 * it again after the delay. The pipe wake-up is only processed once this
	 * timer callback returns, so the first check happens on the next tick */
	if (dbus_checked && dbus_done_ms == 0 && !dbus_skipped) {
		/* round-trip was skipped: nothing to auto-show/hide */
		dbus_skipped = true;
		autohide_done = true;
	}
	if (dbus_done_ms > 0) {
		int64_t dt = notify_test_now_ms() - dbus_done_ms;
		if (dt == 0) {
			/* same tick as the round-trip: the pipe is not processed yet */
		} else if (!autohide_checked) {
			if (!server->notify_panel.active) {
				wlr_log(WLR_ERROR,
					"notify-test: FAIL panel not auto-shown on new notification");
				return 0;
			}
			autohide_checked = true;
			wlr_log(WLR_INFO, "notify-test: auto-show OK");
		} else if (!autohidden_checked && dt < 1500) {
			/* well inside the 2s window: the panel must stay open */
			if (!server->notify_panel.active) {
				wlr_log(WLR_ERROR,
					"notify-test: FAIL panel closed before the auto-hide delay");
				return 0;
			}
		} else if (!autohidden_checked && dt >= 3000) {
			/* safely past the 2s delay: the panel must be gone */
			if (server->notify_panel.active) {
				wlr_log(WLR_ERROR,
					"notify-test: FAIL panel not auto-hidden after the delay");
				return 0;
			}
			autohidden_checked = true;
			autohide_done = true;
			wlr_log(WLR_INFO, "notify-test: auto-hide OK");
		}
		/* 1500 <= dt < 3000: race window around the 2s timer, no check */
	}
	if (notify_count(n) < 1) {
		wlr_log(WLR_ERROR, "notify-test: FAIL no notifications (count=%d)",
			notify_count(n));
		return 0;
	}
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int nn = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < nn; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "notify-test: FAIL no buffer on output %d",
				i + 1);
			return 0;
		}
		if (o->topbar_notif_w <= 0) {
			wlr_log(WLR_ERROR,
				"notify-test: FAIL indicator not rendered on output %d",
				i + 1);
			return 0;
		}
	}
	/* panel: hit areas must match the visible (right-aligned) position,
	 * and the rows must actually render text pixels */
	static bool panel_checked = false;
	if (!panel_checked && nn > 0 && autohide_done) {
		panel_checked = true;
		/* the auto-show may have popped the panel on another output */
		notify_panel_hide(server);
		notify_panel_show(server, sorted[0]);
		struct guibux_notif_panel *p = &server->notify_panel;
		if (!p->active || p->buffer == NULL || p->num_rows < 2) {
			wlr_log(WLR_ERROR,
				"notify-test: FAIL panel not shown (active=%d rows=%d)",
				p->active, p->num_rows);
			return 0;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, sorted[0], &box);
		double left = box.x + p->box_x;
		double top = box.y + server->topbar_height;
		uint32_t rid = notify_panel_row_at(server,
			left + p->box_w / 2.0,
			top + p->row_y[0] + NOTIF_PANEL_ROW_H / 2.0);
		if (rid != p->row_ids[0]) {
			wlr_log(WLR_ERROR,
				"notify-test: FAIL row hit (got %u want %u)", rid,
				p->row_ids[0]);
			return 0;
		}
		if (!notify_panel_clear_at(server,
				left + p->clear_x + p->clear_w / 2.0,
				top + p->clear_y + p->clear_h / 2.0)) {
			wlr_log(WLR_ERROR, "notify-test: FAIL clear button hit");
			return 0;
		}
		void *data;
		uint32_t format;
		size_t stride;
		if (wlr_buffer_begin_data_ptr_access(p->buffer,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format,
				&stride) &&
				format == DRM_FORMAT_XRGB8888) {
			uint32_t *px = (uint32_t *)data;
			int sstride = (int)stride / 4;
			int bw = p->box_w * p->box_scale;
			int bh = p->box_h * p->box_scale;
			int sc = p->box_scale;
			int top_px = (NOTIF_PANEL_PAD + NOTIF_PANEL_HEADER_H) * sc + sc;
			int text_px = 0;
			for (int py = top_px; py < bh; py++) {
				for (int qx = 0; qx < bw; qx++) {
					uint32_t c = px[py * sstride + qx];
					if (c != server->color_bg &&
							c != server->color_border) {
						text_px++;
					}
				}
			}
			wlr_buffer_end_data_ptr_access(p->buffer);
			if (text_px == 0) {
				wlr_log(WLR_ERROR,
					"notify-test: FAIL row text not rendered");
				return 0;
			}
		} else {
			wlr_log(WLR_ERROR, "notify-test: FAIL cannot read panel buffer");
			return 0;
		}
		notify_panel_hide(server);
	}
	/* clicking the bell must open the panel: the net hit area used to
	 * span the whole indicator block and swallowed the bell click */
	static bool click_checked = false;
	if (!click_checked && nn > 0 && autohide_done) {
		click_checked = true;
		struct guibux_output *o = guibux_output_for(server, sorted[0]);
		if (o != NULL && o->topbar_notif_w > 0) {
			server->cursor->x = boxes[0].x + o->topbar_notif_x +
				o->topbar_notif_w / 2.0;
			server->cursor->y = boxes[0].y + server->topbar_height / 2.0;
			server->cursor_topbar_output = o;
			struct wlr_pointer_button_event bevent = {
				.button = 272, /* BTN_LEFT */
				.state = WL_POINTER_BUTTON_STATE_PRESSED,
			};
			server_cursor_button(&server->cursor_button, &bevent);
			if (!server->notify_panel.active) {
				wlr_log(WLR_ERROR,
					"notify-test: FAIL bell click did not open panel");
				return 0;
			}
			notify_panel_hide(server);
		}
	}
	/* all checks done: log OK once and stop the timer. Until then keep
	 * ticking — the auto-show/auto-hide sequence spans several ticks */
	static bool ok_logged = false;
	if (autohide_done && panel_checked && click_checked) {
		if (!ok_logged) {
			ok_logged = true;
			wlr_log(WLR_INFO,
				"notify-test: OK (%d outputs, %d notifications)",
				nn, notify_count(n));
		}
		return 0;
	}
	wl_event_source_timer_update(server->notify_test_timer, 500);
	return 0;
}
