#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#if GUIBUX_HAS_PAM
#include <security/pam_appl.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <xkbcommon/xkbcommon.h>

#define LOCK_CLOCK_FONT_PX 64
#define LOCK_DATE_FONT_PX 20
#define LOCK_STATUS_FONT_PX 18
#define LOCK_DOT_PX 12
#define LOCK_DOT_GAP 10

static const char *day_names[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *month_names[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December"
};

#if GUIBUX_HAS_PAM
/* PAM conversation: the lock screen owns all user interaction, so the
 * library must never try to prompt on its own (that would deadlock on
 * the main thread). The password is passed through conv_data and
 * returned for PAM_PROMPT_ECHO_OFF; anything else is refused */
static const char *pam_password;
static int pam_conv_cb(int num_msg, const struct pam_message **msg,
		struct pam_response **resp, void *conv_data) {
	const char *password = pam_password;
	if (num_msg == 1 && msg[0] != NULL &&
			(msg[0]->msg_style == PAM_PROMPT_ECHO_OFF ||
			 msg[0]->msg_style == PAM_PROMPT_ECHO_ON) &&
			password != NULL) {
		struct pam_response *resp_arr = calloc(num_msg, sizeof(struct pam_response));
		if (resp_arr == NULL) {
			return PAM_BUF_ERR;
		}
		resp_arr[0].resp = strdup(password);
		if (resp_arr[0].resp == NULL) {
			free(resp_arr);
			return PAM_BUF_ERR;
		}
		*resp = resp_arr;
		return PAM_SUCCESS;
	}
	*resp = NULL;
	return PAM_CONV_ERR;
}

static struct pam_conv pam_conv = { .conv = pam_conv_cb };
#endif

static void lock_clear_password(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	memset(lk->password, 0, sizeof(lk->password));
	lk->password_len = 0;
}

static void lock_render_output(struct guibux_server *server,
		struct guibux_lock_output *lo) {
	if (lo->buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(lo->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(lo->buffer);
		return;
	}

	int w = lo->w * lo->scale;
	int hgt = lo->h * lo->scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, hgt, (int)stride);
	cairo_t *cr = cairo_create(cs);

	/* desktop background, so the lock screen matches the wallpaper */
	cairo_surface_t *img = server->bg_surfaces[lo->o->current_workspace - 1];
	if (img != NULL) {
		double iw = cairo_image_surface_get_width(img);
		double ih = cairo_image_surface_get_height(img);
		if (iw > 0 && ih > 0) {
			double s = w / iw;
			if (hgt / ih < s) {
				s = hgt / ih;
			}
			cairo_translate(cr, (w - iw * s) / 2, (hgt - ih * s) / 2);
			cairo_scale(cr, s, s);
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_identity_matrix(cr);
		}
	}
	/* dim: hides the desktop content, keeps the wallpaper visible */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
	cairo_paint(cr);

	struct guibux_lock *lk = &server->lock;
	FT_Face face = server->launcher.face;

	/* clock */
	char time_str[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
	int clock_px = LOCK_CLOCK_FONT_PX * lo->scale;
	FT_Set_Pixel_Sizes(face, 0, clock_px);
	int tw = guibux_text_width(face, time_str) / lo->scale;
	int mb = hgt / 2 - 40 * lo->scale + clock_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face, time_str,
		(w - tw * lo->scale) / 2, mb, server->color_text);

	/* date */
	char date_str[64];
	snprintf(date_str, sizeof(date_str), "%s, %s %d",
		day_names[tm.tm_wday], month_names[tm.tm_mon], tm.tm_mday);
	int date_px = LOCK_DATE_FONT_PX * lo->scale;
	FT_Set_Pixel_Sizes(face, 0, date_px);
	int dw = guibux_text_width(face, date_str) / lo->scale;
	int db = hgt / 2 + 8 * lo->scale + date_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face, date_str,
		(w - dw * lo->scale) / 2, db, server->color_dim);

	/* password dots */
	if (lk->password_len > 0) {
		int dot = LOCK_DOT_PX * lo->scale;
		int gap = LOCK_DOT_GAP * lo->scale;
		int total = lk->password_len * dot +
			(lk->password_len - 1) * gap;
		int dx = (w - total) / 2;
		int dy = hgt / 2 + 56 * lo->scale;
		set_color(cr, server->color_text);
		for (int i = 0; i < lk->password_len; i++) {
			cairo_arc(cr, dx + dot / 2, dy + dot / 2, dot / 2, 0,
				2 * M_PI);
			cairo_fill(cr);
			dx += dot + gap;
		}
	}

	/* status: wrong password / lockout countdown */
	if (lk->status[0] != '\0') {
		int st_px = LOCK_STATUS_FONT_PX * lo->scale;
		FT_Set_Pixel_Sizes(face, 0, st_px);
		int sw = guibux_text_width(face, lk->status) / lo->scale;
		int sb = hgt / 2 + 100 * lo->scale + st_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, lk->status,
			(w - sw * lo->scale) / 2, sb, server->color_highlight);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(lo->buffer);
	if (lo->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(lo->scene_node, lo->buffer);
	}
	if (lo->o->wlr_output != NULL) {
		wlr_output_schedule_frame(lo->o->wlr_output);
	}
}

static void lock_render_all(struct guibux_server *server) {
	for (int i = 0; i < server->lock.num_outs; i++) {
		lock_render_output(server, &server->lock.outs[i]);
	}
}

static void lock_free_outputs(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	for (int i = 0; i < lk->num_outs; i++) {
		struct guibux_lock_output *lo = &lk->outs[i];
		if (lo->scene_node != NULL) {
			wlr_scene_node_destroy(&lo->scene_node->node);
			lo->scene_node = NULL;
		}
		if (lo->buffer != NULL) {
			wlr_buffer_drop(lo->buffer);
			lo->buffer = NULL;
		}
		lo->o = NULL;
	}
	lk->num_outs = 0;
}

static void lock_topbar_set_visible(struct guibux_server *server, bool vis) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_node != NULL) {
			wlr_scene_node_set_enabled(&o->topbar_node->node, vis);
		}
	}
}

void lock_show(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	if (lk->active) {
		return;
	}

	/* no window may keep focus or a grab while locked */
	end_seat_grabs(server);
	clear_keyboard_focus(server);
	tooltip_hide(server);
	osd_hide(server);
	power_panel_hide(server);
	notify_panel_hide(server);
	if (server->launcher.active) {
		launcher_hide(server);
	}
	if (server->switcher.active) {
		switcher_hide(server);
	}
	if (server->help.active) {
		help_hide(server);
	}
	if (server->overview.active) {
		overview_hide(server);
	}
	if (server->outputs_panel.active) {
		outputs_panel_hide(server);
	}
	if (server->topbar_items_panel.active) {
		topbar_items_panel_hide(server);
	}

	lk->fail_count = 0;
	lk->fail_until_ms = 0;
	lk->status[0] = '\0';
	lock_clear_password(server);

	lk->num_outs = 0;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->disabled || lk->num_outs >= MAX_OUTPUT_PLACEMENTS) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			o->wlr_output, &box);
		int scale = guibux_scale_round(o->wlr_output->scale);
		struct guibux_lock_output *lo = &lk->outs[lk->num_outs++];
		lo->o = o;
		lo->w = box.width;
		lo->h = box.height;
		lo->scale = scale;

		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format format = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		lo->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
			box.width * scale, box.height * scale, &format);
		if (lo->buffer == NULL) {
			lk->num_outs--;
			continue;
		}
		lo->scene_node = wlr_scene_buffer_create(&server->scene->tree,
			lo->buffer);
		wlr_scene_buffer_set_dest_size(lo->scene_node, box.width, box.height);
		wlr_scene_node_set_position(&lo->scene_node->node, box.x, box.y);
		wlr_scene_node_raise_to_top(&lo->scene_node->node);
	}
	if (lk->num_outs == 0) {
		wlr_log(WLR_ERROR, "lock: no output buffers, not locking");
		return;
	}

	/* the lock covers the topbar: no clock/battery/network info may be
	 * visible to whoever is at the screen */
	lock_topbar_set_visible(server, false);

	lk->active = true;
	lk->last_minute = time(NULL) / 60;
	lock_render_all(server);
	wlr_log(WLR_INFO, "lock: screen locked (%d outputs)", lk->num_outs);
}

void lock_hide(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	if (!lk->active) {
		return;
	}
	lk->active = false;
	lock_clear_password(server);
	lk->status[0] = '\0';
	lk->fail_count = 0;
	lk->fail_until_ms = 0;
	lock_free_outputs(server);
	lock_topbar_set_visible(server, true);
	wlr_log(WLR_INFO, "lock: screen unlocked");
}

static void lock_verify(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	if (lk->password_len == 0) {
		return;
	}
	uint32_t now = guibux_now_msec();
	if (lk->fail_until_ms != 0 && now < lk->fail_until_ms) {
		int remain = (int)((lk->fail_until_ms - now) / 1000) + 1;
		snprintf(lk->status, sizeof(lk->status),
			"try again in %ds", remain);
		lock_render_all(server);
		return;
	}

	char pw[LOCK_PASSWORD_MAX];
	memcpy(pw, lk->password, sizeof(pw));
	lock_clear_password(server);

#if GUIBUX_HAS_PAM
	/* the password is passed to the conversation callback through
	 * pam_password; it must be cleared before returning to avoid a
	 * dangling pointer */
	pam_password = pw;

	struct pam_handle *pamh = NULL;
	struct passwd *pwent = getpwuid(getuid());
	const char *user = pwent ? pwent->pw_name : NULL;
	int ret = pam_start("guibuxwm", user, &pam_conv, &pamh);
	if (ret != PAM_SUCCESS) {
		pam_password = NULL;
		memset(pw, 0, sizeof(pw));
		snprintf(lk->status, sizeof(lk->status), "auth unavailable");
		lock_render_all(server);
		return;
	}
	ret = pam_authenticate(pamh, 0);
	pam_end(pamh, ret);
	pam_password = NULL;
	memset(pw, 0, sizeof(pw));

	if (ret == PAM_SUCCESS) {
		lock_hide(server);
		return;
	}
#else
	/* no PAM at build time: fall back to the session lock (the
	 * display-manager / loginctl lock screen takes over) */
	(void)pw;
	pid_t pid = fork();
	if (pid < 0) {
		snprintf(lk->status, sizeof(lk->status), "auth unavailable");
		lock_render_all(server);
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c",
			"loginctl lock-session 2>/dev/null", (void *)NULL);
		_exit(127);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
		lock_hide(server);
		return;
	}
#endif
	lk->fail_count++;
	if (lk->fail_count >= LOCK_MAX_FAILS) {
		lk->fail_until_ms = guibux_now_msec() + LOCK_LOCKOUT_MS;
		snprintf(lk->status, sizeof(lk->status),
			"wrong password, locked for %ds", LOCK_LOCKOUT_MS / 1000);
	} else {
		snprintf(lk->status, sizeof(lk->status), "wrong password");
	}
	wlr_log(WLR_INFO, "lock: authentication failed (%d)", lk->fail_count);
	lock_render_all(server);
}

bool lock_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_lock *lk = &server->lock;
	if (!lk->active) {
		return false;
	}
	/* everything is consumed while locked: no key may reach a client */
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		lock_verify(server);
		return true;
	case XKB_KEY_BackSpace:
		if (lk->password_len > 0) {
			/* drop the last UTF-8 code point (1-4 bytes), not one byte */
			int n = lk->password_len;
			while (n > 0 && ((unsigned char)lk->password[n - 1] & 0xC0) == 0x80) {
				n--;
			}
			n--;
			lk->password[n] = '\0';
			lk->password_len = n;
			if (lk->status[0] != '\0' && lk->fail_until_ms == 0) {
				lk->status[0] = '\0';
			}
			lock_render_all(server);
		}
		return true;
	case XKB_KEY_Escape:
		/* no escape from a lock */
		return true;
	}
	/* xkb resolves the layout (fr/en/...), dead keys and Shift; encode
	 * the resulting code point as UTF-8 so accented passwords survive to
	 * PAM, which compares against the UTF-8 shadow entry. The return
	 * value includes the NUL terminator */
	char utf8[8];
	int n = xkb_keysym_to_utf8(sym, utf8, sizeof(utf8));
	if (n > 1 && n - 1 < (int)sizeof(utf8) &&
			lk->password_len + (n - 1) < (int)sizeof(lk->password)) {
		n--;
		memcpy(lk->password + lk->password_len, utf8, n);
		lk->password_len += n;
		lk->password[lk->password_len] = '\0';
		if (lk->status[0] != '\0' && lk->fail_until_ms == 0) {
			lk->status[0] = '\0';
		}
		lock_render_all(server);
	}
	return true;
}

/* re-render once per minute (clock) and refresh the lockout countdown */
int lock_tick(void *data) {
	struct guibux_server *server = data;
	struct guibux_lock *lk = &server->lock;
	if (!lk->active) {
		return 0;
	}
	time_t minute = time(NULL) / 60;
	if (minute != lk->last_minute) {
		lk->last_minute = minute;
		lock_render_all(server);
	}
	if (lk->fail_until_ms != 0) {
		uint32_t now = guibux_now_msec();
		if (now >= lk->fail_until_ms) {
			lk->fail_until_ms = 0;
			lk->fail_count = 0;
			snprintf(lk->status, sizeof(lk->status), "wrong password");
		} else {
			int remain = (int)((lk->fail_until_ms - now) / 1000) + 1;
			char s[64];
			snprintf(s, sizeof(s), "try again in %ds", remain);
			if (strcmp(s, lk->status) != 0) {
				snprintf(lk->status, sizeof(lk->status), "%s", s);
			}
		}
		lock_render_all(server);
	}
	wl_event_source_timer_update(lk->tick, 1000);
	return 0;
}

void lock_destroy(struct guibux_server *server) {
	struct guibux_lock *lk = &server->lock;
	if (lk->tick != NULL) {
		wl_event_source_remove(lk->tick);
		lk->tick = NULL;
	}
	if (lk->active) {
		lock_hide(server);
	}
}

int lock_test_run(void *data) {
	struct guibux_server *server = data;
	lock_show(server);
	if (!server->lock.active) {
		wlr_log(WLR_ERROR, "lock-test: FAIL not shown");
		return 0;
	}
	if (server->lock.num_outs < 1) {
		wlr_log(WLR_ERROR, "lock-test: FAIL no output buffers");
		return 0;
	}
	if (server->lock.outs[0].buffer == NULL) {
		wlr_log(WLR_ERROR, "lock-test: FAIL no buffer");
		return 0;
	}
	/* typing appends, backspace removes */
	lock_handle_key(server, XKB_KEY_a);
	lock_handle_key(server, XKB_KEY_b);
	if (server->lock.password_len != 2) {
		wlr_log(WLR_ERROR, "lock-test: FAIL password len %d",
			server->lock.password_len);
		return 0;
	}
	lock_handle_key(server, XKB_KEY_BackSpace);
	if (server->lock.password_len != 1) {
		wlr_log(WLR_ERROR, "lock-test: FAIL backspace len %d",
			server->lock.password_len);
		return 0;
	}
	/* a multi-byte UTF-8 char (e-acute, 2 bytes) appends as a unit and
	 * one backspace removes the whole char */
	lock_handle_key(server, XKB_KEY_eacute);
	if (server->lock.password_len != 3) {
		wlr_log(WLR_ERROR, "lock-test: FAIL utf8 len %d (want 3)",
			server->lock.password_len);
		return 0;
	}
	if (memcmp(server->lock.password, "a\xc3\xa9", 3) != 0) {
		wlr_log(WLR_ERROR, "lock-test: FAIL utf8 bytes");
		return 0;
	}
	lock_handle_key(server, XKB_KEY_BackSpace);
	if (server->lock.password_len != 1) {
		wlr_log(WLR_ERROR, "lock-test: FAIL utf8 backspace len %d",
			server->lock.password_len);
		return 0;
	}
	/* Esc must not unlock */
	lock_handle_key(server, XKB_KEY_Escape);
	if (!server->lock.active) {
		wlr_log(WLR_ERROR, "lock-test: FAIL Esc unlocked");
		return 0;
	}
#if GUIBUX_HAS_PAM
	/* PAM auth: wrong password must fail, correct password must unlock */
	const char *test_pw = getenv("GUIBUX_TEST_LOCK_PASSWORD");
	if (test_pw != NULL && test_pw[0] != '\0') {
		/* wrong password */
		const char *wrong = "definitely-wrong-password-12345";
		for (const char *p = wrong; *p; p++) {
			lock_handle_key(server, (xkb_keysym_t)(unsigned char)*p);
		}
		lock_handle_key(server, XKB_KEY_Return);
		if (!server->lock.active) {
			wlr_log(WLR_ERROR, "lock-test: FAIL wrong password unlocked");
			return 0;
		}
		if (server->lock.fail_count != 1) {
			wlr_log(WLR_ERROR, "lock-test: FAIL wrong pw fail_count %d",
				server->lock.fail_count);
			return 0;
		}
		/* correct password */
		for (const char *p = test_pw; *p; p++) {
			lock_handle_key(server, (xkb_keysym_t)(unsigned char)*p);
		}
		lock_handle_key(server, XKB_KEY_Return);
		if (server->lock.active) {
			wlr_log(WLR_ERROR, "lock-test: FAIL correct password did not unlock");
			return 0;
		}
		wlr_log(WLR_INFO, "lock-test: PAM auth OK");
	} else {
		wlr_log(WLR_INFO, "lock-test: no password, skipping PAM auth");
		lock_hide(server);
	}
#else
	wlr_log(WLR_INFO, "lock-test: no PAM, skipping auth");
	lock_hide(server);
#endif
	if (server->lock.active) {
		wlr_log(WLR_ERROR, "lock-test: FAIL not hidden");
		return 0;
	}
	if (server->lock.password_len != 0) {
		wlr_log(WLR_ERROR, "lock-test: FAIL password not cleared");
		return 0;
	}
	wlr_log(WLR_INFO, "lock-test: OK");
	return 0;
}
