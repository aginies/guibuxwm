// Optional animations: window open, window close (animated retile),
// notification panel slide.
//
// wlroots 0.20 scene nodes have no alpha and trees no scale, so the
// engine animates what the scene API offers: node positions and scene
// buffer dest sizes (visual scaling without reconfiguring the client).
//
// Ticked from output_frame() before the scene commit, so a client
// commit that resets a dest size in the same event loop iteration is
// re-asserted before the frame is rendered.

#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define EFFECTS_OPEN_SCALE_FROM 0.85

static int64_t effects_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ease-out cubic: fast start, gentle landing */
static double effects_ease(double t) {
	if (t < 0) {
		t = 0;
	}
	if (t > 1) {
		t = 1;
	}
	double u = 1.0 - t;
	return 1.0 - u * u * u;
}

static void effects_on_node_destroy(struct wl_listener *listener, void *data) {
	struct guibux_effects_anim *a = wl_container_of(listener, a, node_destroy);
	/* the target is gone: drop the anim, its completion callback must
	 * not run (it would touch freed state). wlroots asserts the destroy
	 * listener list is empty after the signal, so detach ourselves */
	a->used = false;
	wl_list_remove(&a->node_destroy.link);
	wl_list_init(&a->node_destroy.link);
}

static struct guibux_effects_anim *effects_start(struct guibux_server *server,
		struct wlr_scene_node *node, enum guibux_effect_kind kind,
		int duration_ms) {
	struct guibux_effects *e = &server->effects;
	struct guibux_effects_anim *a = NULL;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		if (!e->anims[i].used) {
			a = &e->anims[i];
			break;
		}
	}
	if (a == NULL) {
		return NULL;
	}
	memset(a, 0, sizeof(*a));
	a->used = true;
	a->kind = kind;
	a->node = node;
	a->duration_ms = duration_ms > 0 ? duration_ms : 1;
	a->start_ms = effects_now_ms();
	a->node_destroy.notify = effects_on_node_destroy;
	wl_signal_add(&node->events.destroy, &a->node_destroy);
	return a;
}

static void effects_apply(struct guibux_effects_anim *a, double t) {
	double v = effects_ease(t);
	switch (a->kind) {
	case EFFECT_POS:
		wlr_scene_node_set_position(a->node,
			(int)(a->fx + (a->tx - a->fx) * v),
			(int)(a->fy + (a->ty - a->fy) * v));
		break;
	case EFFECT_SCALE_FACTOR: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(a->node);
		struct wlr_scene_surface *ss =
			sb != NULL ? wlr_scene_surface_try_from_buffer(sb) : NULL;
		int w = ss != NULL ? ss->surface->current.width : 0;
		int h = ss != NULL ? ss->surface->current.height : 0;
		if (sb != NULL && w > 0 && h > 0) {
			int dw, dh;
			if (v >= 1.0) {
				/* snap to the exact natural size: 0.85+0.15 is not 1.0 in
				 * floating point, so scaling by the eased factor would land
				 * one pixel short */
				dw = w;
				dh = h;
			} else {
				double f = a->f0 + (a->f1 - a->f0) * v;
				dw = (int)(w * f + 0.5);
				dh = (int)(h * f + 0.5);
			}
			if (dw < 1) {
				dw = 1;
			}
			if (dh < 1) {
				dh = 1;
			}
			wlr_scene_buffer_set_dest_size(sb, dw, dh);
		}
		break;
	}
	case EFFECT_SCALE_TO: {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(a->node);
		if (sb != NULL) {
			int dw, dh;
			if (v >= 1.0) {
				dw = a->tw;
				dh = a->th;
			} else {
				dw = (int)(a->sw + (a->tw - a->sw) * v + 0.5);
				dh = (int)(a->sh + (a->th - a->sh) * v + 0.5);
			}
			if (dw < 1) {
				dw = 1;
			}
			if (dh < 1) {
				dh = 1;
			}
			wlr_scene_buffer_set_dest_size(sb, dw, dh);
		}
		break;
	}
	}
}

/* the toplevel owning a scene node: pos anims target the toplevel's
 * scene tree (node->data), scale anims target the inner scene buffer
 * (parent tree carries the data) */
static struct guibux_toplevel *effects_node_toplevel(struct wlr_scene_node *node) {
	if (node->data != NULL && node->type == WLR_SCENE_NODE_TREE) {
		return node->data;
	}
	struct wlr_scene_tree *parent = node->parent;
	while (parent != NULL && parent->node.data == NULL) {
		parent = parent->node.parent;
	}
	if (parent != NULL) {
		return parent->node.data;
	}
	return NULL;
}

/* completion of a toplevel position anim: deliver the deferred size
 * configure (the visual size was animated via dest size) */
static void effects_pos_done(void *data) {
	struct guibux_effects_anim *a = data;
	if (a->pw > 0 && a->ph > 0) {
		struct guibux_toplevel *t = effects_node_toplevel(a->node);
		if (t != NULL && t->scene_tree != NULL) {
			toplevel_set_size(t, a->pw, a->ph);
		}
	}
}

static void effects_kill(struct guibux_effects_anim *a) {
	a->used = false;
	if (a->node_destroy.link.next != NULL) {
		wl_list_remove(&a->node_destroy.link);
	}
}

/* jump the anim to its final state and run the completion callback */
static void effects_settle(struct guibux_server *server,
		struct guibux_effects_anim *a) {
	effects_apply(a, 1.0);
	void (*cb)(void *) = a->done;
	void *cb_data = a->done_data;
	effects_kill(a);
	if (cb != NULL) {
		cb(cb_data);
	}
}

void effects_init(struct guibux_server *server) {
#if !GUIBUX_EFFECTS
	server->effects_enabled = false;
#endif
}

void effects_tick(struct guibux_server *server) {
	if (!server->effects_enabled) {
		return;
	}
	int64_t now = effects_now_ms();
	/* every output fires a frame event per vsync: tick once per stamp */
	if (now == server->effects.last_tick_ms) {
		return;
	}
	server->effects.last_tick_ms = now;
	struct guibux_effects *e = &server->effects;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		struct guibux_effects_anim *a = &e->anims[i];
		if (!a->used) {
			continue;
		}
		double t = (double)(now - a->start_ms) / (double)a->duration_ms;
		if (t >= 1.0) {
			effects_settle(server, a);
		} else {
			effects_apply(a, t);
		}
	}
}

bool effects_active(struct guibux_server *server) {
	struct guibux_effects *e = &server->effects;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		if (e->anims[i].used) {
			return true;
		}
	}
	return false;
}

void effects_flush(struct guibux_server *server) {
	struct guibux_effects *e = &server->effects;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		if (e->anims[i].used) {
			effects_settle(server, &e->anims[i]);
		}
	}
}

void effects_cancel_node(struct guibux_server *server,
		struct wlr_scene_node *node) {
	struct guibux_effects *e = &server->effects;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		struct guibux_effects_anim *a = &e->anims[i];
		if (a->used && a->node == node) {
			effects_kill(a);
		}
	}
}

/* settle (final state + completion callbacks) every anim belonging to
 * a window on this output; used before an immediate retile overwrites
 * animated positions */
void effects_cancel_output(struct guibux_server *server,
		struct guibux_output *o) {
	struct guibux_effects *e = &server->effects;
	for (int i = 0; i < EFFECTS_MAX_ANIMS; i++) {
		struct guibux_effects_anim *a = &e->anims[i];
		if (!a->used) {
			continue;
		}
		struct guibux_toplevel *t = effects_node_toplevel(a->node);
		if (t != NULL && toplevel_output_for(t) == o->wlr_output) {
			effects_settle(server, a);
		}
	}
}

/* the toplevel's main scene buffer. the xdg/xwayland scene tree nests the
 * real surface one level down (outer tree -> subsurface tree -> scene
 * surface), so search the subtree depth-first; the main surface is added
 * first, so it is found before any sub-surface */
static struct wlr_scene_buffer *find_inner_buffer(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		return wlr_scene_buffer_from_node(node);
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_node *child;
	wl_list_for_each(child, &((struct wlr_scene_tree *)node)->children, link) {
		struct wlr_scene_buffer *sb = find_inner_buffer(child);
		if (sb != NULL) {
			return sb;
		}
	}
	return NULL;
}

struct wlr_scene_buffer *toplevel_inner_buffer(struct guibux_toplevel *toplevel) {
	if (toplevel->scene_tree == NULL) {
		return NULL;
	}
	return find_inner_buffer(&toplevel->scene_tree->node);
}

// ---------------------------------------------------------------------------
// Window open
// ---------------------------------------------------------------------------

void effects_window_open(struct guibux_toplevel *toplevel) {
	if (toplevel->is_fullscreen) {
		return;
	}
	toplevel->open_effect_pending = true;
}

/* called on every commit while pending; starts the effect once the
 * window has buffer content (a size to scale) */
void effects_window_open_start(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	if (!toplevel->open_effect_pending) {
		return;
	}
	if (!server->effects_enabled || server->effects_duration_ms <= 0 ||
			server->window_open_effect == OPEN_EFFECT_NONE ||
			toplevel->is_fullscreen || server->overview.active) {
		toplevel->open_effect_pending = false;
		return;
	}
	struct wlr_scene_buffer *sb = toplevel_inner_buffer(toplevel);
	if (sb == NULL || sb->buffer == NULL) {
		return;
	}
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	int w = ss != NULL ? ss->surface->current.width : 0;
	int h = ss != NULL ? ss->surface->current.height : 0;
	if (w <= 0 || h <= 0) {
		return;
	}
	toplevel->open_effect_pending = false;
	int dur = server->effects_duration_ms;
	if (server->window_open_effect == OPEN_EFFECT_SCALE) {
		struct guibux_effects_anim *a = effects_start(server, &sb->node,
			EFFECT_SCALE_FACTOR, dur);
		if (a != NULL) {
			a->f0 = EFFECTS_OPEN_SCALE_FROM;
			a->f1 = 1.0;
		}
	} else {
		/* slide in from the center of the window's output */
		struct wlr_output *wo = toplevel_output_for(toplevel);
		if (wo != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, wo, &box);
			int nx = toplevel->scene_tree->node.x;
			int ny = toplevel->scene_tree->node.y;
			int cx = box.x + box.width / 2 - w / 2;
			int cy = box.y + box.height / 2 - h / 2;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, cx, cy);
			struct guibux_effects_anim *a = effects_start(server,
				&toplevel->scene_tree->node, EFFECT_POS, dur);
			if (a != NULL) {
				a->fx = cx;
				a->fy = cy;
				a->tx = nx;
				a->ty = ny;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Window close: the window's last buffer is frozen and shrinks to a point
// at its center while the remaining windows slide/zoom into the freed cells
// ---------------------------------------------------------------------------

static void close_snap_done(void *data) {
	struct wlr_scene_buffer *snap = data;
	wlr_scene_node_destroy(&snap->node);
}

/* the scene tree dies with the toplevel, so the close animation runs on a
 * copy: the last committed buffer, drawn by its own scene buffer node */
static void effects_close_snapshot(struct guibux_toplevel *t) {
	struct guibux_server *server = t->server;
	if (!server->effects_enabled || server->effects_duration_ms <= 0 ||
			server->overview.active || t->scene_tree == NULL ||
			!t->scene_tree->node.enabled) {
		return;
	}
	struct wlr_scene_buffer *sb = toplevel_inner_buffer(t);
	if (sb == NULL || sb->buffer == NULL) {
		return;
	}
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	int w = ss != NULL ? ss->surface->current.width : 0;
	int h = ss != NULL ? ss->surface->current.height : 0;
	if (w <= 0 || h <= 0) {
		return;
	}
	int x = t->scene_tree->node.x;
	int y = t->scene_tree->node.y;

	struct wlr_scene_buffer *snap = wlr_scene_buffer_create(&server->scene->tree,
		sb->buffer);
	if (snap == NULL) {
		return;
	}
	wlr_scene_node_set_position(&snap->node, x, y);
	wlr_scene_buffer_set_dest_size(snap, w, h);
	wlr_scene_node_raise_to_top(&snap->node);
	topbar_raise_all(server);

	int dur = server->effects_duration_ms;
	struct guibux_effects_anim *p = effects_start(server, &snap->node,
		EFFECT_POS, dur);
	if (p != NULL) {
		p->fx = x;
		p->fy = y;
		p->tx = x + w / 2;
		p->ty = y + h / 2;
	}
	struct guibux_effects_anim *s = effects_start(server, &snap->node,
		EFFECT_SCALE_TO, dur);
	if (s != NULL) {
		s->sw = w;
		s->sh = h;
		s->tw = 1;
		s->th = 1;
	}
	if (p != NULL) {
		p->done = close_snap_done;
		p->done_data = snap;
	} else if (s != NULL) {
		s->done = close_snap_done;
		s->done_data = snap;
	} else {
		wlr_scene_node_destroy(&snap->node);
	}
}

void effects_window_closed(struct guibux_toplevel *t, struct guibux_output *o) {
	effects_close_snapshot(t);
	effects_retile(o);
}

void effects_retile(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (!server->effects_enabled || server->effects_duration_ms <= 0) {
		retile_output(o);
		return;
	}
	if (o->tile_modes[o->current_workspace] == GUIBUX_TILE_FREE || server->overview.active) {
		return;
	}
	struct guibux_tile_target targets[MAX_WINDOWS];
	int n = retile_compute(o, targets, MAX_WINDOWS);
	if (n == 0) {
		return;
	}
	int dur = server->effects_duration_ms;
	for (int i = 0; i < n; i++) {
		struct guibux_toplevel *t = targets[i].t;
		if (t->scene_tree == NULL) {
			continue;
		}
		double cx = t->scene_tree->node.x;
		double cy = t->scene_tree->node.y;
		int tx = targets[i].x;
		int ty = targets[i].y;
		struct wlr_box geo;
		toplevel_get_geometry(t, &geo);
		bool move = (cx != (double)tx || cy != (double)ty);
		bool resize = (geo.width != targets[i].w || geo.height != targets[i].h);
		if (!move && !resize) {
			continue;
		}
		struct guibux_effects_anim *a = effects_start(server,
			&t->scene_tree->node, EFFECT_POS, dur);
		if (a == NULL) {
			wlr_scene_node_set_position(&t->scene_tree->node, tx, ty);
			if (resize) {
				toplevel_set_size(t, targets[i].w, targets[i].h);
			}
			continue;
		}
		a->fx = cx;
		a->fy = cy;
		a->tx = tx;
		a->ty = ty;
		a->done = effects_pos_done;
		a->done_data = a;
		if (resize && t->xdg_toplevel != NULL) {
			/* animate the visual size, deliver the configure at the end:
			 * reconfiguring mid-flight would fight the animation */
			a->pw = targets[i].w;
			a->ph = targets[i].h;
			struct wlr_scene_buffer *sb = toplevel_inner_buffer(t);
			if (sb != NULL && sb->buffer != NULL) {
				struct wlr_scene_surface *ss =
					wlr_scene_surface_try_from_buffer(sb);
				int sw = ss != NULL ? ss->surface->current.width : geo.width;
				int sh = ss != NULL ? ss->surface->current.height : geo.height;
				struct guibux_effects_anim *s = effects_start(server,
					&sb->node, EFFECT_SCALE_TO, dur);
				if (s != NULL) {
					s->sw = sw;
					s->sh = sh;
					s->tw = targets[i].w;
					s->th = targets[i].h;
				}
			}
		}
	}
}


// ---------------------------------------------------------------------------
// Notification panel slide
// ---------------------------------------------------------------------------

void effects_notify_show(struct guibux_server *server,
		struct wlr_output *output) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!server->effects_enabled || !server->notify_effect_slide ||
			server->effects_duration_ms <= 0 || p->scene_node == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int fy = box.y + server->topbar_height;
	int fx = box.x + p->box_x + p->box_w;
	int tx = box.x + p->box_x;
	wlr_scene_node_set_position(&p->scene_node->node, fx, fy);
	struct guibux_effects_anim *a = effects_start(server,
		&p->scene_node->node, EFFECT_POS, server->effects_duration_ms);
	if (a != NULL) {
		a->fx = fx;
		a->fy = fy;
		a->tx = tx;
		a->ty = fy;
	}
}

bool effects_notify_hide(struct guibux_server *server) {
	struct guibux_notif_panel *p = &server->notify_panel;
	if (!server->effects_enabled || !server->notify_effect_slide ||
			server->effects_duration_ms <= 0 || p->scene_node == NULL ||
			p->output == NULL) {
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, p->output, &box);
	int nx = p->scene_node->node.x;
	int ny = p->scene_node->node.y;
	struct guibux_effects_anim *a = effects_start(server,
		&p->scene_node->node, EFFECT_POS, server->effects_duration_ms);
	if (a == NULL) {
		return false;
	}
	a->fx = nx;
	a->fy = ny;
	a->tx = box.x + p->box_x + p->box_w;
	a->ty = ny;
	a->done = notify_panel_hide_done;
	a->done_data = server;
	return true;
}
