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

/* per-render context for the right-side indicator items: the enabled
 * set + order comes from the config `topbar_items` key */
#define TOPBAR_ITEM_PAD 4
/* indicator icon size in logical px (font-relative, set at render) and
 * the gap between icon and its text label */
#define TOPBAR_ICON_GAP 4

/* icon-theme name for the current state of an indicator item; NULL when
 * the item has no icon (caller renders text-only) */
static const char *topbar_volume_icon(int pct, bool muted) {
	if (muted)
		return "audio-volume-muted";
	if (pct >= 67)
		return "audio-volume-high";
	if (pct >= 34)
		return "audio-volume-medium";
	if (pct > 0)
		return "audio-volume-low";
	return "audio-volume-muted";
}

static const char *topbar_mic_icon(int pct, bool muted) {
	if (muted)
		return "microphone-sensitivity-muted";
	if (pct >= 67)
		return "microphone-sensitivity-high";
	if (pct >= 34)
		return "microphone-sensitivity-medium";
	return "microphone-sensitivity-low";
}

static const char *topbar_battery_icon(int pct, int state) {
	/* state: 0=unknown 1=charging 2=discharging 3=empty 4=full;
	 * Adwaita symbolic: battery-level-N[-charging|-plugged-in|-charged] */
	static char name[48];
	int level = (pct / 10) * 10;
	if (level < 0) level = 0;
	if (level > 100) level = 100;
	if (state == 1) {
		if (level >= 100)
			return "battery-level-100-charged";
		snprintf(name, sizeof(name), "battery-level-%d-charging", level);
		return name;
	}
	if (state == 4) {
		return "battery-level-100-plugged-in";
	}
	snprintf(name, sizeof(name), "battery-level-%d", level);
	return name;
}

static const char *topbar_network_icon(bool is_wifi) {
	return is_wifi ? "network-wireless" : "network-wired";
}

struct topbar_items {
	struct guibux_server *server;
	struct guibux_output *o;
	cairo_t *cr;
	cairo_surface_t *cs;
	int scale;
	int baseline;
	int cell_y;
	int cell_h;
	int icon_size;
	struct guibux_sysinfo_snapshot snap;
	char vol[16];
	char mic[16];
	char batbuf[40];
	int notif_count;
	int ind_w;
	int ind_x;
	int date_x;
	bool clock_enabled;
};

static bool topbar_item_enabled(struct guibux_server *server, int id) {
	for (int i = 0; i < server->topbar_item_count; i++) {
		if (server->topbar_items[i] == id) {
			return true;
		}
	}
	return false;
}

static void topbar_items_prepare(struct topbar_items *ctx) {
	struct guibux_server *server = ctx->server;
	struct guibux_output *o = ctx->o;
	sysinfo_get(&server->sysinfo, &ctx->snap);
	const char *net = ctx->snap.net;
	const char *bat = ctx->snap.bat;

	/* audio indicators: "65%" / "80%" with the icon showing state;
	 * "VOL MUTE" / "MIC MUTE" text when the icon is missing from the
	 * theme; empty when no audio (hidden) */
	if (ctx->snap.audio_available) {
		const char *vp = resolve_icon(topbar_volume_icon(ctx->snap.volume, ctx->snap.muted));
		if (vp != NULL) {
			if (ctx->snap.muted) {
				snprintf(ctx->vol, sizeof(ctx->vol), " ");
			} else {
				snprintf(ctx->vol, sizeof(ctx->vol), "%d%%", ctx->snap.volume);
			}
		} else {
			if (ctx->snap.muted) {
				snprintf(ctx->vol, sizeof(ctx->vol), "VOL MUTE");
			} else {
				snprintf(ctx->vol, sizeof(ctx->vol), "VOL %d%%", ctx->snap.volume);
			}
		}
		const char *mp = resolve_icon(topbar_mic_icon(ctx->snap.mic_volume, ctx->snap.mic_muted));
		if (mp != NULL) {
			if (ctx->snap.mic_muted) {
				snprintf(ctx->mic, sizeof(ctx->mic), " ");
			} else {
				snprintf(ctx->mic, sizeof(ctx->mic), "%d%%", ctx->snap.mic_volume);
			}
		} else {
			if (ctx->snap.mic_muted) {
				snprintf(ctx->mic, sizeof(ctx->mic), "MIC MUTE");
			} else {
				snprintf(ctx->mic, sizeof(ctx->mic), "MIC %d%%", ctx->snap.mic_volume);
			}
		}
	} else {
		ctx->vol[0] = '\0';
		ctx->mic[0] = '\0';
	}

	/* battery label: "BAT NN%"; remember the parsed pct + state for
	 * the color-state render and the test hooks */
	if (bat[0] != '\0') {
		snprintf(ctx->batbuf, sizeof(ctx->batbuf), "BAT %s", bat);
		int pct = 0;
		const char *p = bat;
		while (*p >= '0' && *p <= '9') {
			pct = pct * 10 + (*p - '0');
			p++;
		}
		o->topbar_bat_pct = pct;
		o->topbar_bat_state = ctx->snap.bat_state;
	} else {
		ctx->batbuf[0] = '\0';
		o->topbar_bat_pct = -1;
		o->topbar_bat_state = 0;
	}

	ctx->notif_count = notify_count(&server->notify);

	/* remember the rendered sysinfo values: topbar_tick marks the bar
	 * dirty when the worker thread publishes a change, so external
	 * updates (network, volume) show up without other activity */
	snprintf(o->topbar_network, sizeof(o->topbar_network), "%s", net);
	snprintf(o->topbar_battery, sizeof(o->topbar_battery), "%s", bat);
	o->topbar_audio_avail = ctx->snap.audio_available;
	o->topbar_vol_pct = ctx->snap.volume;
	o->topbar_vol_muted = ctx->snap.muted;
	o->topbar_mic_pct = ctx->snap.mic_volume;
	o->topbar_mic_muted = ctx->snap.mic_muted;
}

/* 3px vertical separator with a vertical alpha gradient (transparent at
 * both ends, solid in the middle) so it reads as a soft divider instead
 * of a hard line; x/y/h in logical units */
static void topbar_draw_separator(cairo_t *cr, int x, int y, int h,
		int scale, uint32_t color) {
	cairo_pattern_t *grad = cairo_pattern_create_linear(
		0, y * scale, 0, (y + h) * scale);
	cairo_pattern_add_color_stop_rgba(grad, 0.0,
		((color >> 16) & 0xFF) / 255.0,
		((color >> 8) & 0xFF) / 255.0,
		(color & 0xFF) / 255.0, 0.0);
	cairo_pattern_add_color_stop_rgba(grad, 0.5,
		((color >> 16) & 0xFF) / 255.0,
		((color >> 8) & 0xFF) / 255.0,
		(color & 0xFF) / 255.0, 1.0);
	cairo_pattern_add_color_stop_rgba(grad, 1.0,
		((color >> 16) & 0xFF) / 255.0,
		((color >> 8) & 0xFF) / 255.0,
		(color & 0xFF) / 255.0, 0.0);
	cairo_set_source(cr, grad);
	cairo_rectangle(cr, x * scale, y * scale, 3 * scale, h * scale);
	cairo_fill(cr);
	cairo_pattern_destroy(grad);
}

static void topbar_item_draw_separator(struct topbar_items *ctx, int x, int gap) {
	topbar_draw_separator(ctx->cr, x + gap / 2, ctx->cell_y, ctx->cell_h,
		ctx->scale, ctx->server->color_border);
}

/* width of one indicator item in logical px: icon + gap + text, or
 * text alone when the icon is missing from the theme; the render path
 * must use the same value so hit rects and layout stay in sync */
static int topbar_item_width(struct topbar_items *ctx, const char *icon,
		const char *text) {
	int w = guibux_text_width(ctx->server->launcher.face, text) / ctx->scale;
	if (icon != NULL) {
		if (resolve_icon(icon) != NULL) {
			w += ctx->icon_size + TOPBAR_ICON_GAP;
		}
	}
	return w;
}

/* draw icon (when resolvable) + text label at x (logical); returns the
 * drawn width in logical px */
static int topbar_item_draw(struct topbar_items *ctx, const char *icon,
		const char *text, int x, uint32_t color) {
	int cx = x;
	if (icon != NULL) {
		int iw = topbar_icon_draw(ctx->cr, &ctx->server->launcher,
			icon, x + ctx->icon_size / 2,
			(ctx->cell_y + ctx->cell_h) / 2, ctx->icon_size,
			ctx->scale, color);
		if (iw > 0) {
			cx += iw + TOPBAR_ICON_GAP;
		}
	}
	launcher_draw_text_on_surface(ctx->cs, ctx->server->launcher.face,
		text, cx * ctx->scale, ctx->baseline, color);
	return (cx - x) + guibux_text_width(ctx->server->launcher.face,
		text) / ctx->scale;
}

/* plan + draw one indicator at the current ctx->ind_x; returns the
 * advanced x (unchanged when the item is disabled or has no data).
 * Each item gets TOPBAR_ITEM_PAD on both sides so text does not touch
 * the separators */
static int topbar_item_render(struct topbar_items *ctx, int id) {
	struct guibux_server *server = ctx->server;
	struct guibux_output *o = ctx->o;
	if (!topbar_item_enabled(server, id)) {
		return ctx->ind_x;
	}
	int x = ctx->ind_x + TOPBAR_ITEM_PAD;
	switch (id) {
		case TOPBAR_ITEM_NETWORK: {
			o->topbar_net_count = 0;
			for (int i = 0; i < ctx->snap.net_iface_count; i++) {
				const char *lbl = ctx->snap.net_ifaces[i].label;
				if (lbl[0] == '\0') {
					continue;
				}
				if (o->topbar_net_count > 0) {
					x += 8;
				}
				int iw = topbar_item_draw(ctx,
					topbar_network_icon(ctx->snap.net_ifaces[i].is_wifi),
					lbl, x, server->color_topbar_text);
				o->topbar_net_x[o->topbar_net_count] = x;
				o->topbar_net_w[o->topbar_net_count] = iw;
				x += iw;
				o->topbar_net_count++;
			}
			break;
		}
	case TOPBAR_ITEM_VOLUME:
		o->topbar_vol_x = 0;
		o->topbar_vol_w = 0;
		if (ctx->vol[0] != '\0') {
			o->topbar_vol_x = x;
			o->topbar_vol_w = topbar_item_draw(ctx,
				topbar_volume_icon(ctx->snap.volume, ctx->snap.muted),
				ctx->vol, x, server->color_topbar_text);
			x += o->topbar_vol_w;
		}
		break;
	case TOPBAR_ITEM_MIC:
		o->topbar_mic_x = 0;
		o->topbar_mic_w = 0;
		if (ctx->mic[0] != '\0') {
			o->topbar_mic_x = x;
			o->topbar_mic_w = topbar_item_draw(ctx,
				topbar_mic_icon(ctx->snap.mic_volume, ctx->snap.mic_muted),
				ctx->mic, x, server->color_topbar_text);
			x += o->topbar_mic_w;
		}
		break;
	case TOPBAR_ITEM_BATTERY:
		o->topbar_bat_x = 0;
		o->topbar_bat_w = 0;
		if (ctx->batbuf[0] != '\0') {
			o->topbar_bat_x = x;
			/* color states: low charge red, medium orange, else the
			 * normal topbar text color; charging stays normal */
			uint32_t bc = server->color_topbar_text;
			if (o->topbar_bat_state != 1) {
				if (o->topbar_bat_pct <= 20) {
					bc = 0xef4444;
				} else if (o->topbar_bat_pct <= 50) {
					bc = 0xf59e0b;
				}
			}
			o->topbar_bat_w = topbar_item_draw(ctx,
				topbar_battery_icon(o->topbar_bat_pct, o->topbar_bat_state),
				ctx->batbuf, x, bc);
			x += o->topbar_bat_w;
		}
		break;
	case TOPBAR_ITEM_NOTIFICATIONS:
		o->topbar_notif_x = 0;
		o->topbar_notif_w = 0;
		/* the cell is always reserved (fixed width) so the clock and
		 * the items to its right never shift as the count changes; the
		 * bell + count only draw while there are unread notifications */
		o->topbar_notif_w = notify_indicator_width(
			server->launcher.face, ctx->scale, ctx->notif_count);
		if (ctx->notif_count > 0) {
			o->topbar_notif_x = x;
			notify_draw_indicator(ctx->cs, ctx->cr, server->launcher.face,
				&server->launcher,
				x, ctx->baseline, ctx->scale, ctx->notif_count,
				server->color_topbar_text);
		}
		x += o->topbar_notif_w;
		break;
	case TOPBAR_ITEM_CLOCK:
		break;
	}
	/* no trailing pad when a separator follows: the separator's own
	 * margins (6+7) provide the spacing, so the item keeps a symmetric
	 * gap on both sides instead of pad + margin on the right */
	bool next_real = false;
	for (int j = 0; j < server->topbar_item_count; j++) {
		if (server->topbar_items[j] == id) {
			for (int k = j + 1; k < server->topbar_item_count; k++) {
				if (server->topbar_items[k] != TOPBAR_ITEM_CLOCK) {
					next_real = true;
				}
			}
			break;
		}
	}
	ctx->ind_x = x + (next_real ? 0 : TOPBAR_ITEM_PAD);
	return ctx->ind_x;
}

/* focus-only fast path: the window list and kb focus are the only
 * things that changed, so recompute them and redraw just the
 * window-pill region (occupancy dots never change on a focus event).
 * The indicator/clock layout is reused from the last full render */
static void topbar_render_focus(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (!o->topbar_focus_dirty || o->topbar_buffer == NULL) {
		return;
	}
	if (o->topbar_win_region_x <= 0) {
		/* no full render yet: the cached layout does not exist */
		o->topbar_focus_dirty = false;
		return;
	}
	o->topbar_focus_dirty = false;

	int scale = guibux_scale_round(o->wlr_output->scale);
	int w = o->topbar_buffer_w;
	int h = o->topbar_buffer_h;

	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->topbar_buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(o->topbar_buffer);
		return;
	}
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	int font_px = server->topbar_font_size * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);
	int baseline = o->server->topbar_height / 2 * scale + font_px * 35 / 100;

	/* recompute the window list (same passes as the full render) */
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
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) == o->wlr_output &&
				nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) != o->wlr_output &&
				nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}
	int own_count = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) == o->wlr_output)
			own_count++;
	}
	for (int i = 0; i < nwins; i++) {
		if (wins[i] == kb_focus_t) {
			struct guibux_toplevel *f = wins[i];
			memmove(&wins + 1, wins, i * sizeof(*wins));
			wins[0] = f;
			break;
		}
	}

	/* repaint the window-pill region with the topbar background */
	int rx = o->topbar_win_region_x * scale;
	int rw = (o->topbar_win_region_end - o->topbar_win_region_x) * scale;
	set_color(cr, server->color_topbar_bg);
	cairo_rectangle(cr, rx, 0, rw, h);
	cairo_fill(cr);

	int win_x = o->topbar_win_region_x;
	int win_end = o->topbar_win_region_end;
	int cell_y = o->topbar_cell_y;
	int cell_h = o->topbar_cell_h;

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
		struct guibux_output *wo = guibux_output_for(o->server,
			toplevel_output_for(wins[i]));
		char mon = wo != NULL ? 'A' + (wo->topbar_number - 1) : 'A';
		char prefix[8];
		snprintf(prefix, sizeof(prefix), "%c%d: ", mon, wins[i]->workspace);
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
		if (rendered == own_count && own_count < nwins &&
				win_x < win_end) {
			int cx = win_x + 7;
			topbar_draw_separator(cr, cx, cell_y, cell_h, scale,
				server->color_border);
			win_x += 14;
		}
	}
	o->topbar_win_count = rendered;

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->topbar_buffer);
	if (o->topbar_node != NULL) {
		wlr_scene_buffer_set_buffer(o->topbar_node, o->topbar_buffer);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

void topbar_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (server->launcher.face == NULL || server->launcher.shm_alloc == NULL) {
		return;
	}
	if (!o->topbar_dirty) {
		/* focus-only change: only the active pill moved, redraw just
		 * the window-pill region (the cached layout must exist from a
		 * previous full render) */
		if (o->topbar_focus_dirty && o->topbar_buffer != NULL &&
				o->topbar_win_region_x > 0) {
			topbar_render_focus(o);
		}
		return;
	}

	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		/* keep the dirty flag: retry once the output has a real mode */
		return;
	}
	o->topbar_dirty = false;
	o->topbar_focus_dirty = false;

	/* buffer is in device pixels; the node stays at logical size via
	 * the dest size, so the bar renders sharp on fractional/integer
	 * scaled outputs */
	int scale = guibux_scale_round(o->wlr_output->scale);
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
	/* faint inner top highlight: a raised edge against the wallpaper */
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
	cairo_rectangle(cr, 0, 0, w, scale);
	cairo_fill(cr);
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
	/* monitor badge: outlined rounded rect around the letter so the
	 * monitor is visually distinct from the workspace numbers */
	int badge_pad = 4;
	int badge_w = guibux_text_width(server->launcher.face, left) / scale
		+ 2 * badge_pad;
	topbar_rounded_rect(cr, TOPBAR_PAD * scale, cell_y * scale,
		badge_w * scale, cell_h * scale, 4 * scale);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, 1.0 * scale);
	cairo_stroke(cr);
	launcher_draw_text_on_surface(cs, server->launcher.face, left,
		(TOPBAR_PAD + badge_pad) * scale, baseline,
		server->color_topbar_text);

	int cell_w = guibux_text_width(server->launcher.face, "9") / scale + 16;
	o->topbar_ws_cell_w = cell_w;
	int x = TOPBAR_PAD + badge_w + 12;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		o->topbar_ws_x[ws] = x;
		char num[8];
		snprintf(num, sizeof(num), "%d", ws);
		if (ws == o->current_workspace) {
			set_color(cr, server->color_highlight);
			topbar_rounded_rect(cr, x * scale, cell_y * scale,
				cell_w * scale, cell_h * scale, 4 * scale);
			cairo_fill(cr);
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 8) * scale, baseline, server->color_text);
		} else {
			launcher_draw_text_on_surface(cs, server->launcher.face, num,
				(x + 8) * scale, baseline, server->color_topbar_text);
		}
		x += cell_w;
	}

	/* occupancy dots: one small dot per non-fullscreen window on the
	 * workspace (this monitor), up to 4, under the workspace number */
	{
		int dot_r = 3;
		int dot_gap = 2;
		struct guibux_toplevel *dt;
		for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
			int n = 0;
			wl_list_for_each(dt, &server->toplevels, link) {
				if (dt->is_fullscreen)
					continue;
				if (dt->workspace == ws &&
						toplevel_output_for(dt) == o->wlr_output)
					n++;
			}
			o->topbar_ws_dots[ws] = n;
			if (n == 0)
				continue;
			int shown = n > 4 ? 4 : n;
			int total = shown * (2 * dot_r) + (shown - 1) * dot_gap;
			int dx0 = o->topbar_ws_x[ws] + cell_w / 2 - total / 2;
			int dy = cell_y + cell_h - dot_r - 2;
			if (ws == o->current_workspace) {
				set_color(cr, server->color_text);
			} else {
				set_color_alpha(cr, server->color_topbar_text, 0.5);
			}
			for (int d = 0; d < shown; d++) {
				cairo_arc(cr, (dx0 + d * (2 * dot_r + dot_gap) + dot_r) * scale,
					(dy + dot_r) * scale, dot_r * scale, 0, 2 * M_PI);
				cairo_fill(cr);
			}
		}
	}

	/* vertical separator after workspaces */
	int sep_gap = 12;
	topbar_draw_separator(cr, x + sep_gap / 2, cell_y, cell_h, scale,
		server->color_border);

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
	/* global list: own-monitor windows first, then the windows of the
	 * other monitors (a vertical separator is drawn between the two
	 * groups). The prefix "A2: " (monitor letter + workspace number)
	 * disambiguates cross-monitor entries */
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) == o->wlr_output &&
				nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) != o->wlr_output &&
				nwins < TOPBAR_WIN_MAX)
			wins[nwins++] = t;
	}
	/* number of own-monitor windows: the separator is drawn after this
	 * many cells (only when there are windows on both sides) */
	int own_count = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || (t->xdg_toplevel == NULL && t->xsurface == NULL))
			continue;
		if (toplevel_output_for(t) == o->wlr_output)
			own_count++;
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

	/* right-side indicator items: the enabled set + order comes from
	 * the config `topbar_items` key */
	struct topbar_items ctx = {
		.server = server,
		.o = o,
		.cr = cr,
		.cs = cs,
		.scale = scale,
		.baseline = baseline,
		.cell_y = cell_y,
		.cell_h = cell_h,
		.icon_size = server->topbar_font_size,
	};
	topbar_items_prepare(&ctx);
	ctx.clock_enabled = topbar_item_enabled(server, TOPBAR_ITEM_CLOCK);

	/* calculate indicator width (logical units; text widths come back
	 * in device pixels at the scaled font size) */
	int ind_w = 0;
	for (int i = 0; i < server->topbar_item_count; i++) {
		int id = server->topbar_items[i];
		if (id == TOPBAR_ITEM_CLOCK) {
			continue;
		}
		int seg = 0;
		switch (id) {
		case TOPBAR_ITEM_NETWORK:
			for (int j = 0; j < ctx.snap.net_iface_count; j++) {
				if (ctx.snap.net_ifaces[j].label[0] == '\0') {
					continue;
				}
				if (seg > 0) {
					seg += 8;
				}
				seg += topbar_item_width(&ctx,
					topbar_network_icon(ctx.snap.net_ifaces[j].is_wifi),
					ctx.snap.net_ifaces[j].label);
			}
			break;
		case TOPBAR_ITEM_VOLUME:
			if (ctx.vol[0] != '\0') {
				seg = topbar_item_width(&ctx,
					topbar_volume_icon(ctx.snap.volume, ctx.snap.muted),
					ctx.vol);
			}
			break;
		case TOPBAR_ITEM_MIC:
			if (ctx.mic[0] != '\0') {
				seg = topbar_item_width(&ctx,
					topbar_mic_icon(ctx.snap.mic_volume, ctx.snap.mic_muted),
					ctx.mic);
			}
			break;
		case TOPBAR_ITEM_BATTERY:
			if (ctx.batbuf[0] != '\0') {
				seg = topbar_item_width(&ctx,
					topbar_battery_icon(o->topbar_bat_pct,
						o->topbar_bat_state),
					ctx.batbuf);
			}
			break;
		case TOPBAR_ITEM_NOTIFICATIONS:
			/* fixed-width cell, see topbar_item_render */
			seg = notify_indicator_width(server->launcher.face, scale,
				ctx.notif_count);
			break;
		}
		if (seg > 0) {
			/* trailing pad is dropped when a separator follows (the
			 * separator's margins provide the spacing), so count only
			 * the leading pad here */
			seg += TOPBAR_ITEM_PAD;
			bool has_next = false;
			for (int k = i + 1; k < server->topbar_item_count; k++) {
				if (server->topbar_items[k] != TOPBAR_ITEM_CLOCK) {
					has_next = true;
				}
			}
			if (!has_next) {
				seg += TOPBAR_ITEM_PAD;
			}
			if (ind_w > 0) {
				/* separator between groups: 8px + 3px line + 9px */
				ind_w += 20;
			}
			ind_w += seg;
		}
	}

	int date_w = ctx.clock_enabled ?
		guibux_text_width(server->launcher.face, o->topbar_right) / scale : 0;
	int date_x = w / scale - TOPBAR_PAD - date_w;
	/* the gap before the clock holds the separator + a clear margin on
	 * both sides (8px + 3px line + 9px) */
	int date_gap = (ind_w > 0 && ctx.clock_enabled) ? 20 : 0;
	int ind_start = date_x - date_gap - ind_w;
	int win_end = ind_start - sep_gap;
	if (win_end < win_x)
		win_end = win_x;

	/* cache the window-pill region for the focus-only fast path */
	o->topbar_win_region_x = win_x;
	o->topbar_win_region_end = win_end;
	o->topbar_sep_gap = sep_gap;
	o->topbar_cell_y = cell_y;
	o->topbar_cell_h = cell_h;

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
		/* the prefix letter is the window's own monitor, not this bar's:
		 * the list is global, so cross-monitor entries need their real
		 * monitor letter to be unambiguous */
		struct guibux_output *wo = guibux_output_for(o->server,
			toplevel_output_for(wins[i]));
		char mon = wo != NULL ? 'A' + (wo->topbar_number - 1) : 'A';
		char prefix[8];
		snprintf(prefix, sizeof(prefix), "%c%d: ", mon, wins[i]->workspace);
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
		/* separator between the own-monitor group and the
		 * other-monitor group: drawn inside a widened gap so the
		 * neighbouring pills keep a clear margin from the line */
		if (rendered == own_count && own_count < nwins &&
				win_x < win_end) {
			int cx = win_x + 7;
			topbar_draw_separator(cr, cx, cell_y, cell_h, scale,
				server->color_border);
			win_x += 14;
		}
	}
	o->topbar_win_count = rendered;

	/* render the enabled indicator items in config order */
	ctx.ind_x = date_x - date_gap - ind_w;

	/* separator between indicators and the clock: centered in the gap so
	 * both the last item and the date keep a clear margin */
	if (date_gap > 0) {
		topbar_item_draw_separator(&ctx, date_x - date_gap, date_gap);
	}

	for (int i = 0; i < server->topbar_item_count; i++) {
		int id = server->topbar_items[i];
		if (id == TOPBAR_ITEM_CLOCK) {
			continue;
		}
		int before = ctx.ind_x;
		topbar_item_render(&ctx, id);
		/* separator only when the next entry is a real item: the clock
		 * gets its own separator via date_gap, so a separator after the
		 * last real item (the one before the clock) would double up */
		bool next_real = false;
		for (int j = i + 1; j < server->topbar_item_count; j++) {
			if (server->topbar_items[j] != TOPBAR_ITEM_CLOCK) {
				next_real = true;
				break;
			}
		}
		if (ctx.ind_x > before && next_real) {
			/* the 20px gap starts at ctx.ind_x (item's right edge, no
			 * trailing pad); the helper centers the 3px line in the gap,
			 * so both the item and the next item keep a clear margin */
			topbar_item_draw_separator(&ctx, ctx.ind_x, 20);
			ctx.ind_x += 20;
		}
	}

	if (ctx.clock_enabled) {
		launcher_draw_text_on_surface(cs, server->launcher.face, o->topbar_right,
			date_x * scale,
			baseline, server->color_topbar_text);
	}

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
		/* a full render supersedes any pending focus-only redraw */
		o->topbar_focus_dirty = false;
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
	return topbar_network_index_at(server, o, lx, ly) >= 0;
}

int topbar_network_index_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly) {
	if (o == NULL || o->topbar_buffer == NULL || o->topbar_net_count <= 0) {
		return -1;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (lx < box.x || lx >= box.x + box.width ||
			ly < box.y || ly >= box.y + o->server->topbar_height) {
		return -1;
	}
	double rel = lx - box.x;
	for (int i = 0; i < o->topbar_net_count; i++) {
		if (rel >= o->topbar_net_x[i] &&
				rel < o->topbar_net_x[i] + o->topbar_net_w[i]) {
			return i;
		}
	}
	return -1;
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
	o->topbar_win_region_x = 0;
	o->topbar_dirty = true;
	topbar_render(o);
}

void topbar_win_remove(struct guibux_output *o,
		struct guibux_toplevel *toplevel) {
	if (o == NULL) {
		return;
	}
	/* a preview of the unmapped window would dangle on a dead buffer */
	preview_on_unmap(o->server, toplevel);
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
		if (o->topbar_minute != minute &&
				topbar_item_enabled(server, TOPBAR_ITEM_CLOCK)) {
			o->topbar_minute = minute;
			o->topbar_dirty = true;
		} else {
			bool changed = false;
			if (topbar_item_enabled(server, TOPBAR_ITEM_NETWORK) &&
					strcmp(o->topbar_network, snap.net) != 0) {
				changed = true;
			}
			if (topbar_item_enabled(server, TOPBAR_ITEM_BATTERY) &&
					strcmp(o->topbar_battery, snap.bat) != 0) {
				changed = true;
			}
			if (topbar_item_enabled(server, TOPBAR_ITEM_VOLUME) ||
					topbar_item_enabled(server, TOPBAR_ITEM_MIC)) {
				if (o->topbar_audio_avail != snap.audio_available ||
						o->topbar_vol_pct != snap.volume ||
						o->topbar_vol_muted != snap.muted ||
						o->topbar_mic_pct != snap.mic_volume ||
						o->topbar_mic_muted != snap.mic_muted) {
					changed = true;
				}
			}
			if (changed) {
				o->topbar_dirty = true;
			}
		}
		if (o->topbar_dirty) {
			topbar_render(o);
		}
	}
	/* the battery tooltip arms on pointer motion and shows once the
	 * hover delay has elapsed; this tick is the delay checker */
	tooltip_tick(server);
	preview_tick(server);
	osd_tick(server);
	wl_event_source_timer_update(server->topbar_timer, 1000);
	return 0;
}

/* test hook: seed N fake toplevels on the first output's workspace <ws>
 * so the occupancy-dot count can be verified without a Wayland client.
 * The fakes have no xdg/xwayland surface (only a scene tree), so the
 * title/pill/switcher paths that dereference those are skipped for them;
 * the dot count and the pill list (which only reads workspace + output)
 * work. GUIBUX_TEST_WS_DOTS_SEED=<ws>:<n>. */
void topbar_seed_fake_toplevels(struct guibux_server *server) {
	const char *spec = getenv("GUIBUX_TEST_WS_DOTS_SEED");
	if (spec == NULL) {
		return;
	}
	int ws = 0, n = 0;
	if (sscanf(spec, "%d:%d", &ws, &n) != 2 ||
			ws < 1 || ws > NUM_WORKSPACES || n < 1 || n > 8) {
		return;
	}
	struct guibux_output *o = NULL;
	wl_list_for_each(o, &server->outputs, link) {
		break;
	}
	if (o == NULL || o->wlr_output == NULL) {
		return;
	}
	for (int i = 0; i < n; i++) {
		struct guibux_toplevel *t = calloc(1, sizeof(*t));
		if (t == NULL) {
			break;
		}
		t->server = server;
		t->workspace = ws;
		t->output = o;
		t->managed = true;
		t->is_fullscreen = false;
		t->scene_tree = wlr_scene_tree_create(&server->scene->tree);
		if (t->scene_tree == NULL) {
			free(t);
			break;
		}
		t->scene_tree->node.data = t;
		wl_list_insert(&server->toplevels, &t->link);
	}
	topbar_mark_dirty(o);
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
		/* test hook: GUIBUX_TEST_WS_DOTS=<ws>:<n> — the occupancy dot
		 * count for workspace <ws> must equal <n> (the test client
		 * maps that many toplevels there) */
		const char *dots = getenv("GUIBUX_TEST_WS_DOTS");
		if (dots != NULL) {
			int dws = 0, dn = 0;
			if (sscanf(dots, "%d:%d", &dws, &dn) == 2 &&
					dws >= 1 && dws <= NUM_WORKSPACES) {
				if (o->topbar_ws_dots[dws] != dn) {
					wlr_log(WLR_ERROR,
						"topbar-test: FAIL ws%d dots (got %d, want %d)",
						dws, o->topbar_ws_dots[dws], dn);
					return 0;
				}
			}
		}
		/* test hook: GUIBUX_TEST_TOPBAR_DISABLED=volume,battery — the
		 * listed items must not be rendered (hit rect zero) */
		const char *disabled = getenv("GUIBUX_TEST_TOPBAR_DISABLED");
		if (disabled != NULL) {
			char *disabled_copy = strdup(disabled);
			char *save = NULL;
			for (char *tok = strtok_r(disabled_copy, ",", &save);
					tok != NULL; tok = strtok_r(NULL, ",", &save)) {
				int id = -1;
				if (!strcmp(tok, "network")) id = TOPBAR_ITEM_NETWORK;
				else if (!strcmp(tok, "volume")) id = TOPBAR_ITEM_VOLUME;
				else if (!strcmp(tok, "mic")) id = TOPBAR_ITEM_MIC;
				else if (!strcmp(tok, "battery")) id = TOPBAR_ITEM_BATTERY;
				else if (!strcmp(tok, "notifications")) id = TOPBAR_ITEM_NOTIFICATIONS;
				else if (!strcmp(tok, "clock")) id = TOPBAR_ITEM_CLOCK;
				if (id < 0) {
					continue;
				}
				bool enabled = false;
				for (int i = 0; i < server->topbar_item_count; i++) {
					if (server->topbar_items[i] == id) {
						enabled = true;
					}
				}
				if (enabled) {
					wlr_log(WLR_ERROR,
						"topbar-test: FAIL disabled item '%s' still enabled",
						tok);
					free(disabled_copy);
					return 0;
				}
				int w = 0;
				switch (id) {
				case TOPBAR_ITEM_NETWORK: w = o->topbar_net_count; break;
				case TOPBAR_ITEM_VOLUME: w = o->topbar_vol_w; break;
				case TOPBAR_ITEM_MIC: w = o->topbar_mic_w; break;
				case TOPBAR_ITEM_BATTERY: w = o->topbar_bat_w; break;
				case TOPBAR_ITEM_NOTIFICATIONS: w = o->topbar_notif_w; break;
				default: break;
				}
				if (w != 0) {
					wlr_log(WLR_ERROR,
						"topbar-test: FAIL disabled item '%s' rendered (w=%d)",
						tok, w);
					free(disabled_copy);
					return 0;
				}
			}
			free(disabled_copy);
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
			/* color-state inputs: the rendered pct + state must match
			 * the snapshot (the render picks red/orange/normal from
			 * these) */
			if (o->topbar_bat_pct != pct ||
					o->topbar_bat_state != snap.bat_state) {
				wlr_log(WLR_ERROR,
					"battery-test: FAIL pct/state (got %d/%d, want %d/%d)",
					o->topbar_bat_pct, o->topbar_bat_state,
					pct, snap.bat_state);
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
