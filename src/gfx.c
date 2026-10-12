#include "guibuxwm.h"
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include <drm_fourcc.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared drawing/buffer helpers used by every overlay in the compositor
// (topbar, panels, OSD, tooltips, lock screen, window borders, previews).
// ---------------------------------------------------------------------------

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

// width in px of text at the face's current pixel size
int guibux_text_width(FT_Face face, const char *text) {
	int w = 0;
	const char *p = text;
	while (*p) {
		FT_UInt glyph = FT_Get_Char_Index(face, utf8_next(&p));
		if (glyph != 0 && FT_Load_Glyph(face, glyph, FT_LOAD_RENDER) == 0) {
			w += face->glyph->advance.x / 64;
		}
	}
	return w;
}

void set_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0,
		((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
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

/* alpha-blend FreeType glyph bitmaps straight into an XRGB8888 image
 * surface. Cairo's own text path is avoided because every overlay shares
 * one FT_Face whose pixel size is switched per draw, and this keeps the
 * blending identical across the topbar, panels, OSD and tooltips. */
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

/* allocate an shm-backed buffer of width x height *device* pixels in the
 * given DRM format. Every overlay draws into exactly this kind of buffer
 * with the same single-modifier format set, so the block lives here once
 * instead of being repeated at each call site.
 *
 * Returns NULL when the shm allocator is missing (logged here: the caller
 * cannot diagnose that case) or when the allocation fails. Allocation
 * failures are not logged so callers can attach their own context; they
 * must still check the result. */
struct wlr_buffer *guibux_shm_buffer(struct guibux_server *server,
		int width, int height, uint32_t drm_format) {
	if (server->launcher.shm_alloc == NULL) {
		wlr_log(WLR_ERROR, "gfx: no shm allocator, cannot create %dx%d buffer",
			width, height);
		return NULL;
	}
	if (width <= 0 || height <= 0) {
		wlr_log(WLR_ERROR, "gfx: bad buffer size %dx%d", width, height);
		return NULL;
	}
	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = drm_format,
		.len = 1,
		.modifiers = mods,
	};
	return wlr_allocator_create_buffer(server->launcher.shm_alloc,
		width, height, &format);
}
