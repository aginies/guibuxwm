#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* delay before the preview appears after the pointer enters the
 * window label; checked by preview_tick (topbar_tick, 500ms) */
#define PREVIEW_HOVER_DELAY_MS 500
#define PREVIEW_W 480
#define PREVIEW_H 300

static uint32_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool preview_contains(struct guibux_server *server, double lx, double ly) {
	struct guibux_window_preview *pv = &server->window_preview;
	if (!pv->active || pv->output == NULL) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, pv->output, &box);
	return lx >= box.x + pv->box_x && lx < box.x + pv->box_x + pv->box_w &&
		ly >= box.y + pv->box_y && ly < box.y + pv->box_y + pv->box_h;
}

void preview_hide(struct guibux_server *server) {
	struct guibux_window_preview *pv = &server->window_preview;
	pv->active = false;
	pv->hover_toplevel = NULL;
	pv->hover_output = NULL;
	pv->toplevel = NULL;
	pv->output = NULL;
	if (pv->scene_node != NULL) {
		wlr_scene_node_destroy(&pv->scene_node->node);
		pv->scene_node = NULL;
	}
	/* the buffer is persistent: it is reused by the next show so a
	 * hover only repositions the node; dropped in preview_destroy */
}

/* nearest-neighbor downscale of an XRGB8888 image into dst
 * (dstride bytes per row); used to fit the readback into the fixed
 * preview buffer without a second full-size allocation */
static void downscale_xrgb(const uint8_t *src, uint32_t sw, uint32_t sh,
		uint32_t sstride, uint8_t *dst, uint32_t dw, uint32_t dh,
		uint32_t dstride) {
	for (uint32_t y = 0; y < dh; y++) {
		uint32_t sy = y * sh / dh;
		const uint8_t *srow = src + (size_t)sy * sstride;
		uint8_t *drow = dst + (size_t)y * dstride;
		for (uint32_t x = 0; x < dw; x++) {
			uint32_t sx = x * sw / dw;
			memcpy(drow + x * 4, srow + (size_t)sx * 4, 4);
		}
	}
}

/* read the window's current buffer into the persistent preview buffer.
 * wrapping the live buffer directly would hold a lock that blocks
 * xwayland client buffer damage updates, so copy the pixels into the
 * shm buffer instead; the copy is downscaled to the buffer size so a
 * 1080p window does not read back 8MB */
static bool preview_snapshot(struct guibux_server *server,
		struct wlr_scene_buffer *sb) {
	struct guibux_window_preview *pv = &server->window_preview;
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer,
		sb->buffer);
	if (tex == NULL) {
		return false;
	}
	uint32_t fmt = wlr_texture_preferred_read_format(tex);
	if (fmt == DRM_FORMAT_INVALID) {
		wlr_texture_destroy(tex);
		return false;
	}
	uint32_t w = tex->width, h = tex->height;
	uint32_t stride = w * 4;
	uint8_t *data = malloc((size_t)stride * h);
	if (data == NULL) {
		wlr_texture_destroy(tex);
		return false;
	}
	bool ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
		.data = data,
		.format = fmt,
		.stride = stride,
	});
	wlr_texture_destroy(tex);
	if (!ok) {
		free(data);
		return false;
	}

	if (pv->buffer == NULL || pv->buffer_w < PREVIEW_W ||
			pv->buffer_h < PREVIEW_H) {
		if (pv->buffer != NULL) {
			wlr_buffer_drop(pv->buffer);
			pv->buffer = NULL;
			pv->buffer_w = 0;
			pv->buffer_h = 0;
		}
		uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
		struct wlr_drm_format drm_fmt = {
			.format = DRM_FORMAT_XRGB8888,
			.len = 1,
			.modifiers = mods,
		};
		pv->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
			PREVIEW_W, PREVIEW_H, &drm_fmt);
		if (pv->buffer == NULL) {
			free(data);
			return false;
		}
		pv->buffer_w = PREVIEW_W;
		pv->buffer_h = PREVIEW_H;
	}

	void *bdata;
	uint32_t bfmt;
	size_t bstride;
	if (!wlr_buffer_begin_data_ptr_access(pv->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &bdata, &bfmt, &bstride)) {
		free(data);
		return false;
	}
	if (bfmt == DRM_FORMAT_XRGB8888) {
		if (w <= PREVIEW_W && h <= PREVIEW_H) {
			/* center the smaller window in the buffer */
			uint32_t ox = (PREVIEW_W - w) / 2;
			uint32_t oy = (PREVIEW_H - h) / 2;
			for (uint32_t y = 0; y < h; y++) {
				memcpy((uint8_t *)bdata + (size_t)(oy + y) * bstride +
					(size_t)ox * 4,
					data + (size_t)y * stride, (size_t)w * 4);
			}
		} else {
			downscale_xrgb(data, w, h, stride, bdata,
				PREVIEW_W, PREVIEW_H, (uint32_t)bstride);
		}
	}
	wlr_buffer_end_data_ptr_access(pv->buffer);
	free(data);
	return true;
}

static void preview_show(struct guibux_server *server,
		struct guibux_output *o, struct guibux_toplevel *t) {
	struct guibux_window_preview *pv = &server->window_preview;
	if (pv->active) {
		return;
	}
	struct wlr_scene_buffer *sb = toplevel_inner_buffer(t);
	if (sb == NULL || sb->buffer == NULL) {
		return;
	}
	if (!preview_snapshot(server, sb)) {
		return;
	}
	/* find the hovered cell to anchor the preview under it */
	int ax = 0, aw = 0;
	for (int i = 0; i < o->topbar_win_count; i++) {
		if (o->topbar_wins[i] == t) {
			ax = o->topbar_win_x[i];
			aw = o->topbar_win_w[i];
			break;
		}
	}
	if (aw <= 0) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	int bw = PREVIEW_W, bh = PREVIEW_H;
	int bx = ax + aw / 2 - bw / 2;
	if (bx < 4)
		bx = 4;
	if (bx + bw > box.width - 4)
		bx = box.width - 4 - bw;
	if (bx < 4)
		bx = 4;
	int by = server->topbar_height + 4;

	if (pv->scene_node == NULL) {
		pv->scene_node = wlr_scene_buffer_create(&server->scene->tree,
			pv->buffer);
		if (pv->scene_node == NULL) {
			return;
		}
	} else {
		wlr_scene_buffer_set_buffer(pv->scene_node, pv->buffer);
	}
	pv->toplevel = t;
	pv->output = o->wlr_output;
	pv->box_w = bw;
	pv->box_h = bh;
	pv->box_x = bx;
	pv->box_y = by;
	wlr_scene_buffer_set_dest_size(pv->scene_node, bw, bh);
	wlr_scene_node_set_position(&pv->scene_node->node, box.x + bx, box.y + by);
	wlr_scene_node_raise_to_top(&pv->scene_node->node);
	topbar_raise_all(server);
	pv->active = true;
	wlr_output_schedule_frame(o->wlr_output);
}

void preview_update_hover(struct guibux_server *server, uint32_t time) {
	struct guibux_window_preview *pv = &server->window_preview;
	double x = server->cursor->x, y = server->cursor->y;

	/* the preview itself keeps the hover alive: the pointer may move
	 * from the label onto the box */
	if (pv->active && preview_contains(server, x, y)) {
		return;
	}
	/* full-screen UI takes over the pointer: drop the hover and hide */
	if (server->launcher.active || server->switcher.active ||
			server->overview.active || server->help.active ||
			server->notify_panel.active) {
		pv->hover_toplevel = NULL;
		pv->hover_output = NULL;
		if (pv->active) {
			preview_hide(server);
		}
		return;
	}
	struct guibux_output *o = NULL;
	int ws = 0;
	if (topbar_workspace_at(server, x, y, &o, &ws)) {
		struct guibux_toplevel *t = topbar_win_at(o, x, y);
		if (t != NULL) {
			if (pv->hover_toplevel != t) {
				pv->hover_toplevel = t;
				pv->hover_output = o->wlr_output;
				pv->hover_since = time;
			}
			return;
		}
	}
	if (pv->hover_toplevel != NULL) {
		pv->hover_toplevel = NULL;
		pv->hover_output = NULL;
		if (pv->active) {
			preview_hide(server);
		}
	}
}

void preview_tick(struct guibux_server *server) {
	struct guibux_window_preview *pv = &server->window_preview;
	if (pv->active || pv->hover_toplevel == NULL) {
		return;
	}
	uint32_t now = monotonic_ms();
	if (now - pv->hover_since < PREVIEW_HOVER_DELAY_MS) {
		return;
	}
	/* the label may have moved or disappeared since the hover was
	 * armed: show the preview on the bar the window was hovered on, not
	 * the window's own monitor (the topbar list is global) */
	struct guibux_toplevel *t = pv->hover_toplevel;
	struct guibux_output *o = guibux_output_for(server, pv->hover_output);
	if (o == NULL) {
		pv->hover_toplevel = NULL;
		pv->hover_output = NULL;
		return;
	}
	preview_show(server, o, t);
}

void preview_on_unmap(struct guibux_server *server,
		struct guibux_toplevel *toplevel) {
	struct guibux_window_preview *pv = &server->window_preview;
	if (pv->toplevel == toplevel) {
		preview_hide(server);
	}
	if (pv->hover_toplevel == toplevel) {
		pv->hover_toplevel = NULL;
		pv->hover_output = NULL;
	}
}

void preview_destroy(struct guibux_server *server) {
	struct guibux_window_preview *pv = &server->window_preview;
	preview_hide(server);
	if (pv->buffer != NULL) {
		wlr_buffer_drop(pv->buffer);
		pv->buffer = NULL;
		pv->buffer_w = 0;
		pv->buffer_h = 0;
	}
}
