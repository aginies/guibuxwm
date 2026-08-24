#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <drm_fourcc.h>
#include <time.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>

// width in px of text at the face's current pixel size
// cache: open-addressing hash of the advance width per codepoint for
// the current font size; cp 0 marks an empty slot (NUL is never
// measured). A linear scan here is O(cps^2 * cache) for title
// truncation
#define CHAR_CACHE_SIZE 512
static uint32_t char_cache_cp[CHAR_CACHE_SIZE];
static int char_cache_w[CHAR_CACHE_SIZE];
static int char_width_cache_size = -1;

static void invalidate_char_width_cache(void) {
	memset(char_cache_cp, 0, sizeof(char_cache_cp));
	char_width_cache_size = -1;
}

// decode one UTF-8 codepoint, advance *p past it
uint32_t utf8_next(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp;
	if (s[0] < 0x80) {
		cp = s[0];
		*p += 1;
	} else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
		cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		*p += 2;
	} else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
		cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
			(s[2] & 0x3F);
		*p += 3;
	} else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
		cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
			((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		*p += 4;
	} else {
		cp = '?';
		*p += 1;
	}
	return cp;
}

// copy up to max_cp codepoints of src into dst (NUL-terminated)
void utf8_truncate(const char *src, char *dst, size_t dst_size, int max_cp) {
	int cp = 0;
	size_t n = 0;
	while (src[0] && cp < max_cp) {
		const char *p = src;
		utf8_next(&p);
		size_t len = (size_t)(p - src);
		if (n + len >= dst_size) {
			break;
		}
		memcpy(dst + n, src, len);
		n += len;
		src = p;
		cp++;
	}
	dst[n] = '\0';
}

static int char_advance_width(FT_Face face, uint32_t cp, int font_size) {
	if (font_size != char_width_cache_size) {
		invalidate_char_width_cache();
		char_width_cache_size = font_size;
	}
	int i = (int)((uint32_t)cp * 2654435761u & (CHAR_CACHE_SIZE - 1));
	for (int probe = 0; probe < 8; probe++) {
		int slot = (i + probe) & (CHAR_CACHE_SIZE - 1);
		if (char_cache_cp[slot] == cp) {
			return char_cache_w[slot];
		}
		if (char_cache_cp[slot] == 0) {
			break;
		}
	}
	FT_UInt glyph = FT_Get_Char_Index(face, cp);
	if (glyph == 0 || FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) != 0) {
		return 0;
	}
	int w = face->glyph->advance.x / 64;
	for (int probe = 0; probe < 8; probe++) {
		int slot = (i + probe) & (CHAR_CACHE_SIZE - 1);
		if (char_cache_cp[slot] == 0) {
			char_cache_cp[slot] = cp;
			char_cache_w[slot] = w;
			break;
		}
	}
	return w;
}

int guibux_text_width(FT_Face face, const char *text) {
	int w = 0;
	int font_size = face->size->metrics.height / 64;
	const char *p = text;
	while (*p) {
		w += char_advance_width(face, utf8_next(&p), font_size);
	}
	return w;
}

void set_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0,
		((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}

static void set_color_alpha(cairo_t *cr, uint32_t c, double a) {
	cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0,
		((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, a);
}

void topbar_rounded_rect(cairo_t *cr, double x, double y,
		double w, double h, double r) {
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

int launcher_draw_text_on_surface(cairo_surface_t *cs, FT_Face face,
		const char *text, int x, int baseline, uint32_t color) {
	uint32_t *data = (uint32_t *)cairo_image_surface_get_data(cs);
	int stride = cairo_image_surface_get_stride(cs) / 4;
	int sw = cairo_image_surface_get_width(cs);
	int sh = cairo_image_surface_get_height(cs);
	int cx = x;
	const char *p = text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		FT_UInt glyph = FT_Get_Char_Index(face, cp);
		if (glyph == 0 || FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) != 0) {
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		for (uint32_t row = 0; row < g->bitmap.rows; row++) {
			int py = baseline - (int)g->bitmap_top + (int)row;
			if (py < 0 || py >= sh) {
				continue;
			}
			for (uint32_t col = 0; col < g->bitmap.width; col++) {
				int px = cx + (int)g->bitmap_left + (int)col;
				if (px < 0 || px >= sw) {
					continue;
				}
				uint8_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
				if (a == 0) {
					continue;
				}
				uint32_t *dst = &data[py * stride + px];
				uint8_t r = (color >> 16) & 0xFF;
				uint8_t gg = (color >> 8) & 0xFF;
				uint8_t b = color & 0xFF;
				uint8_t dr = (*dst >> 16) & 0xFF;
				uint8_t dg = (*dst >> 8) & 0xFF;
				uint8_t db = *dst & 0xFF;
				r = (r * a + dr * (255 - a)) / 255;
				gg = (gg * a + dg * (255 - a)) / 255;
				b = (b * a + db * (255 - a)) / 255;
				*dst = (r << 16) | (gg << 8) | b;
			}
		}
		cx += g->advance.x / 64;
	}
	return cx - x;
}

void topbar_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}
	if (!o->topbar_dirty) {
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		/* keep the dirty flag: retry once the output has a real mode */
		return;
	}
	o->topbar_dirty = false;

	/* buffer is in device pixels; the node stays at logical size via
	 * the dest size, so the bar renders sharp on fractional/integer
	 * scaled outputs */
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int w = box.width * scale;
	int h = o->server->topbar_height * scale;

	/* create/resize buffer (also covers outputs that mapped with a
	 * 0x0 box and only got a real mode later) */
	if (o->topbar_buffer == NULL || o->topbar_buffer_w != w ||
			o->topbar_buffer_h != h) {
		wlr_buffer_drop(o->topbar_buffer);
		o->topbar_buffer = NULL;
		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format format = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		o->topbar_buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
			w, h, &format);
		if (o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "topbar: failed to create buffer on %s",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)");
			o->topbar_buffer_w = 0;
			o->topbar_buffer_h = 0;
			return;
		}
		o->topbar_buffer_w = w;
		o->topbar_buffer_h = h;
		if (o->topbar_node == NULL) {
			o->topbar_node = wlr_scene_buffer_create(&server->scene->tree,
				o->topbar_buffer);
			if (o->topbar_node == NULL) {
				wlr_buffer_drop(o->topbar_buffer);
				o->topbar_buffer = NULL;
				o->topbar_buffer_w = 0;
				o->topbar_buffer_h = 0;
				return;
			}
		} else {
			wlr_scene_buffer_set_buffer(o->topbar_node, o->topbar_buffer);
		}
		wlr_scene_buffer_set_dest_size(o->topbar_node,
			box.width, o->server->topbar_height);
		wlr_scene_node_set_position(&o->topbar_node->node, box.x, box.y);
	}

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->topbar_buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "topbar: cannot access buffer data");
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "topbar: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(o->topbar_buffer);
		return;
	}

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_topbar_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_rectangle(cr, 0, h - scale, w, scale);
	cairo_fill(cr);

	int font_px = server->topbar_font_size * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);
	int baseline = o->server->topbar_height / 2 * scale + font_px * 35 / 100;

	/* vertical extent of cells/pills inside the topbar (config: topbar_win_pad) */
	int th = o->server->topbar_height;
	int pad = server->topbar_win_pad;
	if (pad < 0)
		pad = 0;
	if (pad * 2 >= th)
		pad = th / 4;
	int cell_y = pad;
	int cell_h = th - 2 * pad;

	char left[16];
	snprintf(left, sizeof(left), "%c", 'A' + (o->topbar_number - 1));
	launcher_draw_text_on_surface(cs, server->launcher.face, left,
		TOPBAR_PAD * scale, baseline, server->color_topbar_text);

	int cell_w = guibux_text_width(server->launcher.face, "9") / scale + 8;
	o->topbar_ws_cell_w = cell_w;
	int x = TOPBAR_PAD + guibux_text_width(server->launcher.face, left) / scale + 12;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		o->topbar_ws_x[ws] = x;
		char num[8];
		snprintf(num, sizeof(num), "%d", ws);
		if (ws == o->current_workspace) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, x * scale, cell_y * scale,
				cell_w * scale, cell_h * scale);
			cairo_fill(cr);
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_text);
		} else {
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_topbar_text);
		}
		x += cell_w;
	}

	/* vertical separator after workspaces */
	int sep_gap = 12;
	cairo_set_source_rgb(cr,
		((server->color_border >> 16) & 0xFF) / 255.0,
		((server->color_border >> 8) & 0xFF) / 255.0,
		(server->color_border & 0xFF) / 255.0);
	cairo_rectangle(cr, (int)((x + sep_gap / 2) * scale) - (int)(scale),
		cell_y * scale, 2 * scale, cell_h * scale);
	cairo_fill(cr);

	/* render window titles between workspaces and date */
	int win_x = x + sep_gap;
	o->topbar_win_count = 0;
	struct guibux_toplevel *t;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *kb_focus = seat->keyboard_state.focused_surface;
	struct guibux_toplevel *kb_focus_t = NULL;
	if (kb_focus) {
		struct wlr_xdg_toplevel *kb_xdg =
			wlr_xdg_toplevel_try_from_wlr_surface(kb_focus);
		struct wlr_xwayland_surface *kb_xs =
			wlr_xwayland_surface_try_from_wlr_surface(kb_focus);
		wl_list_for_each(t, &server->toplevels, link) {
			if ((kb_xdg && t->xdg_toplevel == kb_xdg) ||
					(kb_xs && t->xsurface == kb_xs)) {
				kb_focus_t = t;
				break;
			}
		}
	}

	struct guibux_toplevel *wins[TOPBAR_WIN_MAX];
	int nwins = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen)
			continue;
		if (toplevel_output_for(t) != o->wlr_output)
			continue;
		if (nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}

	/* put focused first */
	for (int i = 0; i < nwins; i++) {
		if (wins[i] == kb_focus_t) {
			struct guibux_toplevel *f = wins[i];
			memmove(&wins[1], &wins[0], i * sizeof(*wins));
			wins[0] = f;
			break;
		}
	}

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(o->topbar_right, sizeof(o->topbar_right),
		"%a %d %b %Y  %H:%M", &tm);
	o->topbar_minute = now / 60;

	/* snapshot sysinfo (published by the sysinfo worker thread) */
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&o->server->sysinfo, &snap);
	const char *net = snap.net;
	const char *bat = snap.bat;

	/* audio indicators: "VOL 65%" / "VOL MUTE", "MIC 80%" / "MIC MUTE";
	 * empty when no audio is available (hidden) */
	char vol[16], mic[16];
	if (snap.audio_available) {
		if (snap.muted) {
			snprintf(vol, sizeof(vol), "VOL MUTE");
		} else {
			snprintf(vol, sizeof(vol), "VOL %d%%", snap.volume);
		}
		if (snap.mic_muted) {
			snprintf(mic, sizeof(mic), "MIC MUTE");
		} else {
			snprintf(mic, sizeof(mic), "MIC %d%%", snap.mic_volume);
		}
	} else {
		vol[0] = '\0';
		mic[0] = '\0';
	}

	/* battery label: "BAT NN%" */
	char batbuf[40];
	if (bat[0] != '\0') {
		snprintf(batbuf, sizeof(batbuf), "BAT %s", bat);
	} else {
		batbuf[0] = '\0';
	}

	/* remember the rendered sysinfo values: topbar_tick marks the bar
	 * dirty when the worker thread publishes a change, so external
	 * updates (network, volume) show up without other activity */
	snprintf(o->topbar_network, sizeof(o->topbar_network), "%s", net);
	snprintf(o->topbar_battery, sizeof(o->topbar_battery), "%s", bat);
	o->topbar_audio_avail = snap.audio_available;
	o->topbar_vol_pct = snap.volume;
	o->topbar_vol_muted = snap.muted;
	o->topbar_mic_pct = snap.mic_volume;
	o->topbar_mic_muted = snap.mic_muted;

	/* calculate indicator width (logical units; text widths come back
	 * in device pixels at the scaled font size) */
	int ind_w = 0;
	if (net[0] != '\0') {
		ind_w += guibux_text_width(server->launcher.face, net) / scale;
	}
	if (vol[0] != '\0') {
		/* separator between net and audio: 4px + 2px line + 4px */
		if (ind_w > 0) ind_w += 10;
		ind_w += guibux_text_width(server->launcher.face, vol) / scale;
	}
	if (mic[0] != '\0') {
		if (ind_w > 0) ind_w += 8;
		ind_w += guibux_text_width(server->launcher.face, mic) / scale;
	}
	if (batbuf[0] != '\0') {
		/* separator between audio indicators and battery:
		 * 4px + 2px line + 4px */
		if (ind_w > 0) ind_w += 10;
		ind_w += guibux_text_width(server->launcher.face, batbuf) / scale;
	}
	int notif_count = notify_count(&server->notify);
	int notif_w = notify_indicator_width(server->launcher.face, scale,
		notif_count);
	if (notif_count > 0) {
		if (ind_w > 0) ind_w += 8;
		ind_w += notif_w;
	}

	int date_w = guibux_text_width(server->launcher.face, o->topbar_right) / scale;
	int date_x = w / scale - TOPBAR_PAD - date_w;
	int ind_start = date_x - 10 - ind_w;
	int win_end = ind_start - sep_gap;
	if (win_end < win_x)
		win_end = win_x;

	/* vertical separator before indicators */
	if (win_x < win_end - sep_gap) {
		cairo_set_source_rgb(cr,
			((server->color_border >> 16) & 0xFF) / 255.0,
			((server->color_border >> 8) & 0xFF) / 255.0,
			(server->color_border & 0xFF) / 255.0);
		cairo_rectangle(cr, (int)((win_end + sep_gap / 2) * scale) - (int)(scale),
			cell_y * scale, 2 * scale, cell_h * scale);
		cairo_fill(cr);
	}

	/* calculate max width per window to fit all in available space */
	int avail = win_end - win_x;
	int max_w = 30;
	if (nwins > 0) {
		int per = (avail - (nwins - 1) * TOPBAR_WIN_GAP) / nwins;
		if (per < 30)
			per = 30;
		max_w = per;
	}

	int rendered = 0;
	for (int i = 0; i < nwins && win_x < win_end; i++) {
		const char *title = toplevel_get_title(wins[i]) ?
			toplevel_get_title(wins[i]) : "(untitled)";
		char prefix[8];
		snprintf(prefix, sizeof(prefix), "%c%d: ",
			'A' + (o->topbar_number - 1), wins[i]->workspace);
		int pw = guibux_text_width(server->launcher.face, prefix);
		int budget = max_w - 16 - pw;
		if (budget < 0)
			budget = 0;
		char tbuf[64];
		int tw = guibux_text_width(server->launcher.face, title);
		if (tw > budget) {
			int cps = 0;
			const char *p = title;
			while (*p) {
				utf8_next(&p);
				cps++;
			}
			for (int trunc = cps; trunc >= 1; trunc--) {
				utf8_truncate(title, tbuf, sizeof(tbuf), trunc);
				size_t n = strlen(tbuf);
				snprintf(tbuf + n, sizeof(tbuf) - n, "...");
				tw = guibux_text_width(server->launcher.face, tbuf);
				if (tw <= budget) {
					break;
				}
			}
			if (tw > budget) {
				utf8_truncate(title, tbuf, sizeof(tbuf), 1);
				size_t n = strlen(tbuf);
				snprintf(tbuf + n, sizeof(tbuf) - n, "...");
			}
		} else {
			snprintf(tbuf, sizeof(tbuf), "%s", title);
		}
		char buf[72];
		snprintf(buf, sizeof(buf), "%s%s", prefix, tbuf);
		tw = guibux_text_width(server->launcher.face, buf);
		int cell_w = tw + 16;
		if (cell_w > max_w)
			cell_w = max_w;
		o->topbar_wins[rendered] = wins[i];
		snprintf(o->topbar_win_titles[rendered], sizeof(o->topbar_win_titles[rendered]), "%s", buf);
		o->topbar_win_x[rendered] = win_x;
		o->topbar_win_w[rendered] = cell_w;

		if (wins[i] == kb_focus_t) {
			set_color(cr, server->color_highlight);
			topbar_rounded_rect(cr, win_x * scale,
				cell_y * scale,
				cell_w * scale,
				cell_h * scale,
				6 * scale);
			cairo_fill_preserve(cr);
			set_color(cr, server->color_text);
			cairo_set_line_width(cr, 1.0 * scale);
			cairo_stroke(cr);
			launcher_draw_text_on_surface(cs,
				server->launcher.face, buf,
				(win_x + 8) * scale, baseline,
				server->color_text);
		} else {
			set_color_alpha(cr, server->color_topbar_text, 0.22);
			topbar_rounded_rect(cr, win_x * scale,
				cell_y * scale,
				cell_w * scale,
				cell_h * scale,
				6 * scale);
			cairo_fill_preserve(cr);
			set_color_alpha(cr, server->color_topbar_text, 0.50);
			cairo_set_line_width(cr, 1.0 * scale);
			cairo_stroke(cr);
			launcher_draw_text_on_surface(cs,
				server->launcher.face, buf,
				(win_x + 8) * scale, baseline,
				server->color_topbar_text);
		}
		win_x += cell_w + TOPBAR_WIN_GAP;
		rendered++;
	}
	o->topbar_win_count = rendered;

	/* render network + battery indicators */
	int ind_x = date_x - ind_w - 10;

	/* separator between indicators and date */
	if (ind_w > 0) {
		cairo_set_source_rgb(cr,
			((server->color_border >> 16) & 0xFF) / 255.0,
			((server->color_border >> 8) & 0xFF) / 255.0,
			(server->color_border & 0xFF) / 255.0);
		cairo_rectangle(cr, (int)((date_x - 5) * scale) - (int)(scale),
			cell_y * scale, 2 * scale, cell_h * scale);
		cairo_fill(cr);
	}

	o->topbar_net_x = 0;
	o->topbar_net_w = 0;
	if (net[0] != '\0') {
		/* hit area is the net text only: the block also holds the
		 * audio/battery/notification indicators, which have their own */
		o->topbar_net_x = ind_x;
		o->topbar_net_w = guibux_text_width(server->launcher.face,
			net) / scale;
		launcher_draw_text_on_surface(cs, server->launcher.face,
			net,
			ind_x * scale, baseline, server->color_topbar_text);
		ind_x += o->topbar_net_w;
		if (vol[0] != '\0') {
			/* separator between net and audio indicators */
			cairo_set_source_rgb(cr,
				((server->color_border >> 16) & 0xFF) / 255.0,
				((server->color_border >> 8) & 0xFF) / 255.0,
				(server->color_border & 0xFF) / 255.0);
			cairo_rectangle(cr, (int)((ind_x + 5) * scale) - (int)(scale),
				cell_y * scale, 2 * scale, cell_h * scale);
			cairo_fill(cr);
			ind_x += 10;
		} else {
			ind_x += 8;
		}
	}
	o->topbar_vol_x = 0;
	o->topbar_vol_w = 0;
	if (vol[0] != '\0') {
		o->topbar_vol_x = ind_x;
		o->topbar_vol_w = guibux_text_width(server->launcher.face,
			vol) / scale;
		launcher_draw_text_on_surface(cs, server->launcher.face,
			vol,
			ind_x * scale, baseline, server->color_topbar_text);
		ind_x += o->topbar_vol_w;
	}
	o->topbar_mic_x = 0;
	o->topbar_mic_w = 0;
	if (mic[0] != '\0') {
		ind_x += 8;
		o->topbar_mic_x = ind_x;
		o->topbar_mic_w = guibux_text_width(server->launcher.face,
			mic) / scale;
		launcher_draw_text_on_surface(cs, server->launcher.face,
			mic,
			ind_x * scale, baseline, server->color_topbar_text);
		ind_x += o->topbar_mic_w;
	}
	o->topbar_bat_x = 0;
	o->topbar_bat_w = 0;
	if (batbuf[0] != '\0') {
		bool had_prev = net[0] != '\0' || vol[0] != '\0' ||
			mic[0] != '\0';
		if (had_prev) {
			/* separator between audio indicators and battery */
			cairo_set_source_rgb(cr,
				((server->color_border >> 16) & 0xFF) / 255.0,
				((server->color_border >> 8) & 0xFF) / 255.0,
				(server->color_border & 0xFF) / 255.0);
			cairo_rectangle(cr, (int)((ind_x + 5) * scale) - (int)(scale),
				cell_y * scale, 2 * scale, cell_h * scale);
			cairo_fill(cr);
			ind_x += 10;
		}
		o->topbar_bat_x = ind_x;
		o->topbar_bat_w = guibux_text_width(server->launcher.face,
			batbuf) / scale;
		launcher_draw_text_on_surface(cs, server->launcher.face,
			batbuf,
			ind_x * scale, baseline, server->color_topbar_text);
		ind_x += o->topbar_bat_w;
	}

	/* separator between battery and date */
	if (bat[0] != '\0') {
		cairo_set_source_rgb(cr,
			((server->color_border >> 16) & 0xFF) / 255.0,
			((server->color_border >> 8) & 0xFF) / 255.0,
			(server->color_border & 0xFF) / 255.0);
		cairo_rectangle(cr, (int)((ind_x + 4) * scale) - (int)(scale),
			cell_y * scale, 2 * scale, cell_h * scale);
		cairo_fill(cr);
		ind_x += 8;
	}

	/* notification indicator: bell + pending count */
	if (notif_count > 0) {
		bool had_prev = net[0] != '\0' || vol[0] != '\0' ||
			mic[0] != '\0' || bat[0] != '\0';
		if (had_prev) {
			cairo_set_source_rgb(cr,
				((server->color_border >> 16) & 0xFF) / 255.0,
				((server->color_border >> 8) & 0xFF) / 255.0,
				(server->color_border & 0xFF) / 255.0);
			cairo_rectangle(cr, (int)((ind_x + 4) * scale) - (int)(scale),
				cell_y * scale, 2 * scale, cell_h * scale);
			cairo_fill(cr);
			ind_x += 8;
		}
		o->topbar_notif_x = ind_x;
		o->topbar_notif_w = notif_w;
		notify_draw_indicator(cs, cr, server->launcher.face,
			ind_x, baseline, scale, notif_count,
			server->color_topbar_text);
	} else {
		o->topbar_notif_x = 0;
		o->topbar_notif_w = 0;
	}

	launcher_draw_text_on_surface(cs, server->launcher.face, o->topbar_right,
		date_x * scale,
		baseline, server->color_topbar_text);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->topbar_buffer);
	if (o->topbar_node != NULL) {
		wlr_scene_buffer_set_buffer(o->topbar_node, o->topbar_buffer);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

void topbar_mark_dirty(struct guibux_output *o) {
	if (o != NULL) {
		o->topbar_dirty = true;
	}
}

struct guibux_toplevel *topbar_win_at(struct guibux_output *o,
		double lx, double ly) {
	struct guibux_server *server = o->server;
	if (o->topbar_buffer == NULL)
		return NULL;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height)
		return NULL;
	double rel = lx - box.x;
	for (int i = 0; i < o->topbar_win_count; i++) {
		if (rel >= o->topbar_win_x[i] &&
				rel < o->topbar_win_x[i] + o->topbar_win_w[i])
			return o->topbar_wins[i];
	}
	return NULL;
}

bool topbar_workspace_at(struct guibux_server *server, double lx,
		double ly, struct guibux_output **output, int *ws) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_buffer == NULL) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		if (lx < box.x || lx >= box.x + box.width ||
				ly < box.y || ly >= box.y + o->server->topbar_height) {
			continue;
		}
		if (output != NULL) {
			*output = o;
		}
		if (ws != NULL) {
			*ws = 0;
			double rel = lx - box.x;
			for (int i = 1; i <= NUM_WORKSPACES; i++) {
				if (rel >= o->topbar_ws_x[i] &&
						rel < o->topbar_ws_x[i] + o->topbar_ws_cell_w) {
					*ws = i;
					break;
				}
			}
		}
		return true;
	}
	return false;
}

bool topbar_network_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly) {
	if (o == NULL || o->topbar_buffer == NULL || o->topbar_net_w <= 0) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height) {
		return false;
	}
	double rel = lx - box.x;
	if (rel >= o->topbar_net_x &&
			rel < o->topbar_net_x + o->topbar_net_w) {
		return true;
	}
	return false;
}

bool topbar_battery_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly) {
	if (o == NULL || o->topbar_buffer == NULL || o->topbar_bat_w <= 0) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height) {
		return false;
	}
	double rel = lx - box.x;
	if (rel >= o->topbar_bat_x &&
			rel < o->topbar_bat_x + o->topbar_bat_w) {
		return true;
	}
	return false;
}

bool topbar_notif_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly) {
	if (o == NULL || o->topbar_buffer == NULL || o->topbar_notif_w <= 0) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height) {
		return false;
	}
	double rel = lx - box.x;
	if (rel >= o->topbar_notif_x &&
			rel < o->topbar_notif_x + o->topbar_notif_w) {
		return true;
	}
	return false;
}

int topbar_audio_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly) {
	if (o == NULL || o->topbar_buffer == NULL) {
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height) {
		return 0;
	}
	double rel = lx - box.x;
	if (o->topbar_vol_w > 0 &&
			rel >= o->topbar_vol_x && rel < o->topbar_vol_x + o->topbar_vol_w) {
		return 1;
	}
	if (o->topbar_mic_w > 0 &&
			rel >= o->topbar_mic_x && rel < o->topbar_mic_x + o->topbar_mic_w) {
		return 2;
	}
	return 0;
}

void topbar_create(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (server->launcher.shm_alloc == NULL || server->launcher.face == NULL) {
		wlr_log(WLR_ERROR, "topbar: disabled on %s (no allocator or font)",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	/* the buffer is created lazily by topbar_render: the output may
	 * not have a mode yet (0x0 box) and get one later */
	o->topbar_dirty = true;
	topbar_render(o);
}

void topbar_win_remove(struct guibux_output *o,
		struct guibux_toplevel *toplevel) {
	if (o == NULL) {
		return;
	}
	for (int i = 0; i < o->topbar_win_count; i++) {
		if (o->topbar_wins[i] == toplevel) {
			int n = o->topbar_win_count - i - 1;
			memmove(&o->topbar_wins[i], &o->topbar_wins[i + 1],
				n * sizeof(*o->topbar_wins));
			memmove(&o->topbar_win_x[i], &o->topbar_win_x[i + 1],
				n * sizeof(o->topbar_win_x[0]));
			memmove(&o->topbar_win_w[i], &o->topbar_win_w[i + 1],
				n * sizeof(o->topbar_win_w[0]));
			memmove(&o->topbar_win_titles[i], &o->topbar_win_titles[i + 1],
				n * sizeof(o->topbar_win_titles[0]));
			o->topbar_win_count--;
			return;
		}
	}
}

void topbar_destroy(struct guibux_output *o) {
	if (o->topbar_node != NULL) {
		wlr_scene_node_destroy(&o->topbar_node->node);
		o->topbar_node = NULL;
	}
	if (o->topbar_buffer != NULL) {
		wlr_buffer_drop(o->topbar_buffer);
		o->topbar_buffer = NULL;
	}
}

void topbar_renumber(struct guibux_server *server) {
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		/* the number must be set even without a buffer: an output created
		 * before it has a mode (0x0 box) gets its buffer later, and a
		 * missing number would render the labels as '@' until the next
		 * output event */
		if (o == NULL) {
			continue;
		}
		o->topbar_number = i + 1;
		o->topbar_dirty = true;
		if (o->topbar_buffer != NULL) {
			topbar_render(o);
		}
	}
}

int topbar_tick(void *data) {
	struct guibux_server *server = data;
	/* apply volume changes accumulated while the previous pactl child
	 * was still running (relative sets must not run concurrently) */
	volume_flush(server);
	/* the clock is part of the topbar: mark dirty on a minute rollover,
	 * otherwise an idle desktop would show a frozen time */
	time_t minute = time(NULL) / 60;
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	/* a new/dismissed notification changes the indicator on every bar */
	if (notify_consume_dirty(&server->notify)) {
		struct guibux_output *no;
		wl_list_for_each(no, &server->outputs, link) {
			no->topbar_dirty = true;
		}
		if (server->notify_panel.active) {
			notify_panel_render(server);
		}
	}
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_minute != minute) {
			o->topbar_minute = minute;
			o->topbar_dirty = true;
		} else if (strcmp(o->topbar_network, snap.net) != 0 ||
				strcmp(o->topbar_battery, snap.bat) != 0 ||
				o->topbar_audio_avail != snap.audio_available ||
				o->topbar_vol_pct != snap.volume ||
				o->topbar_vol_muted != snap.muted ||
				o->topbar_mic_pct != snap.mic_volume ||
				o->topbar_mic_muted != snap.mic_muted) {
			o->topbar_dirty = true;
		}
		if (o->topbar_dirty) {
			topbar_render(o);
		}
	}
	/* the battery tooltip arms on pointer motion and shows once the
	 * hover delay has elapsed; this tick is the delay checker */
	tooltip_tick(server);
	wl_event_source_timer_update(server->topbar_timer, 500);
	return 0;
}

int topbar_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL no buffer on output %d", i + 1);
			return 0;
		}
		if (o->topbar_number != i + 1) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL number (got %d, want %d)",
				o->topbar_number, i + 1);
			return 0;
		}
		if (strlen(o->topbar_right) < 10) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL time string '%s'",
				o->topbar_right);
			return 0;
		}
	}
	wlr_log(WLR_INFO, "topbar-test: OK (%d outputs)", n);
	return 0;
}

int audio_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "audio-test: FAIL no buffer on output %d", i + 1);
			return 0;
		}
		if (snap.audio_available) {
			if (snap.volume < 0 || snap.volume > 100 ||
					snap.mic_volume < 0 || snap.mic_volume > 100) {
				wlr_log(WLR_ERROR, "audio-test: FAIL bad volumes (%d/%d)",
					snap.volume, snap.mic_volume);
				return 0;
			}
			if (o->topbar_vol_w <= 0 || o->topbar_mic_w <= 0) {
				wlr_log(WLR_ERROR, "audio-test: FAIL indicators not rendered");
				return 0;
			}
		} else {
			if (o->topbar_vol_w != 0 || o->topbar_mic_w != 0) {
				wlr_log(WLR_ERROR, "audio-test: FAIL indicators without audio");
				return 0;
			}
		}
	}
	wlr_log(WLR_INFO, "audio-test: OK (%d outputs, audio %s)", n,
		snap.audio_available ? "on" : "off");
	return 0;
}

int battery_test_run(void *data) {
	struct guibux_server *server = data;
	/* the runner script probes upower for ground truth and passes it in:
	 * "yes" = a battery device exists, "" = it must stay hidden */
	const char *expect = getenv("GUIBUX_TEST_BATTERY_EXPECT");
	bool want = expect != NULL && strcmp(expect, "yes") == 0;
	/* the runner probes `upower -d` for a "time to empty/full" line:
	 * when UPower has an estimate, the poll must carry it (tooltip
	 * shows the remaining time) */
	const char *expect_eta = getenv("GUIBUX_TEST_BATTERY_ETA");
	bool want_eta = expect_eta != NULL && strcmp(expect_eta, "yes") == 0;
	struct guibux_sysinfo_snapshot snap;
	sysinfo_get(&server->sysinfo, &snap);
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "battery-test: FAIL no buffer on output %d", i + 1);
			return 0;
		}
		if (want) {
			if (snap.bat[0] == '\0') {
				wlr_log(WLR_ERROR, "battery-test: FAIL battery not polled");
				return 0;
			}
			/* format must be "NN%" */
			int pct = 0;
			const char *end = snap.bat;
			while (*end >= '0' && *end <= '9') {
				pct = pct * 10 + (*end - '0');
				end++;
			}
			if (*end != '%' || end[1] != '\0' || pct > 100) {
				wlr_log(WLR_ERROR, "battery-test: FAIL bad format '%s'", snap.bat);
				return 0;
			}
			if (strcmp(o->topbar_battery, snap.bat) != 0) {
				wlr_log(WLR_ERROR, "battery-test: FAIL indicator not rendered");
				return 0;
			}
			if (want_eta && snap.bat_eta_sec <= 0) {
				wlr_log(WLR_ERROR, "battery-test: FAIL no time estimate (state %d)",
					snap.bat_state);
				return 0;
			}
		} else {
			if (snap.bat[0] != '\0') {
				wlr_log(WLR_ERROR, "battery-test: FAIL battery without upower '%s'", snap.bat);
				return 0;
			}
		}
	}
	wlr_log(WLR_INFO, "battery-test: OK (%d outputs, battery %s)", n,
		snap.bat[0] != '\0' ? snap.bat : "off");
	return 0;
}
