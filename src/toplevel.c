#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

void focus_toplevel(struct guibux_toplevel *toplevel, bool raise) {
	if (toplevel == NULL || toplevel->scene_tree == NULL) {
		return;
	}
	struct guibux_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel_get_surface(toplevel);
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		} else {
			struct wlr_xwayland_surface *prev_xs =
				wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
			if (prev_xs != NULL) {
				wlr_xwayland_surface_activate(prev_xs, false);
			}
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (raise) {
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		topbar_raise_all(server);
	}
	if (toplevel->managed) {
		wl_list_remove(&toplevel->link);
		wl_list_insert(&server->toplevels, &toplevel->link);
	}
	toplevel_set_activated(toplevel, true);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	struct guibux_output *fo = guibux_output_for(server,
		toplevel_output_for(toplevel));
	if (fo) {
		/* only the active pill moved: the fast path redraws just the
		 * window-pill region, no full-bar repaint */
		fo->topbar_focus_dirty = true;
		/* the pill list is global: a window mapping on one monitor
		 * must appear on every bar, not just its own */
		struct guibux_output *bo;
		wl_list_for_each(bo, &server->outputs, link) {
			if (bo != fo) {
				topbar_mark_dirty(bo);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Fullscreen
// ---------------------------------------------------------------------------

void set_fullscreen(struct guibux_toplevel *toplevel, bool fullscreen,
		struct wlr_output *output) {
	if (fullscreen == toplevel->is_fullscreen) {
		return;
	}
	struct guibux_server *server = toplevel->server;

	if (fullscreen) {
		toplevel->saved_x = toplevel->scene_tree->node.x;
		toplevel->saved_y = toplevel->scene_tree->node.y;
		/* save the windowed size for both protocols: an xdg window
		 * un-fullscreened with set_size(0,0) would revert to the app's
		 * preferred size and lose its pre-fullscreen dimensions */
		struct wlr_box geo;
		toplevel_get_geometry(toplevel, &geo);
		toplevel->saved_w = geo.width;
		toplevel->saved_h = geo.height;

		/* the requested output is authoritative (it may differ from
		 * the window's current position); fall back to the window's
		 * output for WM-initiated fullscreen */
		if (output == NULL) {
			output = toplevel_output_for(toplevel);
		}
		if (output != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output, &box);
			int th = server->topbar_height;
			int ew, eh;
			wlr_output_effective_resolution(output, &ew, &eh);
			/* fullscreen below the topbar, so the bar stays visible */
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				box.x, box.y + th);
			toplevel_set_size(toplevel, ew, eh - th);
		}
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		topbar_raise_all(server);
	} else {
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_x, toplevel->saved_y);
		if (toplevel->saved_w > 0 && toplevel->saved_h > 0) {
			toplevel_set_size(toplevel, toplevel->saved_w, toplevel->saved_h);
		} else {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		}
	}

	toplevel->is_fullscreen = fullscreen;
	toplevel_set_fullscreen_state(toplevel, fullscreen);
	if (!fullscreen) {
		struct guibux_output *o = guibux_output_for(server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
			retile_output(o);
		}
	}
}

// ---------------------------------------------------------------------------
// Move/Resize interactive
// ---------------------------------------------------------------------------

void begin_interactive(struct guibux_toplevel *toplevel,
		enum guibux_cursor_mode mode, uint32_t edges) {
	struct guibux_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == GUIBUX_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box geo_box;
		toplevel_get_geometry(toplevel, &geo_box);

		double border_x = (toplevel->scene_tree->node.x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

// ---------------------------------------------------------------------------
// xdg_toplevel lifecycle
// ---------------------------------------------------------------------------

/* the scene tree is owned by wlroots (xdg) or destroyed on
 * dissociate (xwayland): track its death so a dangling scene_tree
 * pointer can never be used (effects anims, hit testing) */
static void toplevel_scene_destroyed(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, scene_destroy);
	wl_list_remove(&toplevel->scene_destroy.link);
	toplevel->scene_tree = NULL;
}

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct guibux_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->managed = true;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->scene_destroy.notify = toplevel_scene_destroyed;
	wl_signal_add(&toplevel->scene_tree->node.events.destroy,
		&toplevel->scene_destroy);

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	if (toplevel->server->overview.active) {
		overview_hide(toplevel->server);
	}

	wlr_log(WLR_INFO, "mapped toplevel '%s'",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)");

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

	struct wlr_output *output = output_at_cursor(toplevel->server);
	struct guibux_output *o = output != NULL
		? guibux_output_for(toplevel->server, output) : NULL;
	enum restore_result rr = restore_apply(toplevel->server, toplevel);
	if (rr != RESTORE_FREE) {
		if (rr == RESTORE_TILE) {
			/* saved output+workspace resolved: tile it there */
			o = toplevel->output;
		}
		toplevel->output = o;
		toplevel->workspace = o != NULL ? o->current_workspace : 1;
		if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
			struct wlr_box box;
			wlr_output_layout_get_box(toplevel->server->output_layout,
				o->wlr_output, &box);
			wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
			retile_output(o);
		} else {
			place_toplevel(toplevel);
		}
	}
	focus_toplevel(toplevel, true);
	struct guibux_output *o2 = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o2)
		topbar_mark_dirty(o2);
	/* tell fractional-scale clients the output's scale so they can
	 * render at 1.5x/1.25x instead of being forced to integer */
	if (toplevel->xdg_toplevel->base->surface != NULL &&
			toplevel_output_for(toplevel) != NULL) {
		wlr_fractional_scale_v1_notify_scale(
			toplevel->xdg_toplevel->base->surface,
			toplevel_output_for(toplevel)->scale);
	}
	effects_window_open(toplevel);
}

/* Pick a visible toplevel to focus after one was unmapped; the
 * first entry of the list may live on another workspace/output */
static void focus_after_unmap(struct guibux_server *server) {
	struct guibux_toplevel *t;
	struct guibux_toplevel *next = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		if (toplevel_visible(t)) {
			next = t;
			break;
		}
	}
	if (next != NULL) {
		focus_toplevel(next, true);
	} else {
		clear_keyboard_focus(server);
	}
}

void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct guibux_server *server = toplevel->server;

	if (server->overview.active) {
		overview_hide(server);
	}

	/* remember the final position for the next launch */
	restore_save(server, toplevel);

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	toplevel->open_effect_pending = false;
	wl_list_remove(&toplevel->link);
	switcher_on_unmap(server, toplevel);
	topbar_win_remove(o, toplevel);
	if (o != NULL) {
		effects_window_closed(toplevel, o);
	}
	topbar_mark_dirty(o);
	/* the pill list is global: the removed window must vanish from
	 * every bar, not just its own */
	struct guibux_output *bo;
	wl_list_for_each(bo, &server->outputs, link) {
		if (bo != o) {
			topbar_mark_dirty(bo);
		}
	}

	focus_after_unmap(server);
}

void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->open_effect_pending) {
		effects_window_open_start(toplevel);
	}

	if (toplevel->xdg_toplevel->base->initial_commit) {
		struct guibux_output *o = guibux_output_for(toplevel->server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
			wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
		} else if (toplevel->restore_w > 0 && toplevel->restore_h > 0) {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
				toplevel->restore_w, toplevel->restore_h);
		} else {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		}
	}
}

void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wlr_log(WLR_INFO, "destroyed toplevel '%s'",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)");

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	toplevel->open_effect_pending = false;
	if (toplevel->scene_tree != NULL) {
		effects_cancel_node(toplevel->server, &toplevel->scene_tree->node);
		/* the scene tree may outlive the toplevel (the client can
		 * destroy the role and keep the surface): drop the back-pointer
		 * so hit testing and effects anims never dereference freed state */
		toplevel->scene_tree->node.data = NULL;
	}

	struct guibux_output *fo = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (fo)
		topbar_mark_dirty(fo);

	if (toplevel->server->last_ffm_toplevel == toplevel) {
		toplevel->server->last_ffm_toplevel = NULL;
	}

	free(toplevel);
}

void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
}

void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_RESIZE, event->edges);
}

void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	set_fullscreen(toplevel, toplevel->xdg_toplevel->requested.fullscreen,
		toplevel->xdg_toplevel->requested.fullscreen_output);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

// ---------------------------------------------------------------------------
// Toplevel accessors (xdg or xwayland)
// ---------------------------------------------------------------------------

struct wlr_surface *toplevel_get_surface(struct guibux_toplevel *toplevel) {
	if (toplevel->xdg_toplevel != NULL) {
		return toplevel->xdg_toplevel->base->surface;
	}
	return toplevel->xsurface->surface;
}

const char *toplevel_get_title(struct guibux_toplevel *toplevel) {
	if (toplevel->xdg_toplevel != NULL) {
		return toplevel->xdg_toplevel->title;
	}
	return toplevel->xsurface->title;
}

const char *toplevel_app_id(struct guibux_toplevel *toplevel) {
	if (toplevel->xdg_toplevel != NULL) {
		return toplevel->xdg_toplevel->app_id;
	}
	return toplevel->xsurface->instance;
}

struct guibux_toplevel *toplevel_for_app(struct guibux_server *server,
		const char *app_name) {
	if (app_name == NULL || app_name[0] == '\0') {
		return NULL;
	}
	struct guibux_toplevel *t, *fallback = NULL;
	/* pass 1: exact case-insensitive match (xdg app_id / WM_CLASS
	 * instance); a window on the current workspace wins */
	wl_list_for_each(t, &server->toplevels, link) {
		const char *id = toplevel_app_id(t);
		if (id != NULL && strcasecmp(id, app_name) == 0) {
			if (toplevel_visible(t)) {
				return t;
			}
			if (fallback == NULL) {
				fallback = t;
			}
		}
	}
	if (fallback != NULL) {
		return fallback;
	}
	/* pass 2: xwayland WM_CLASS class */
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->xsurface != NULL && t->xsurface->class != NULL &&
				strcasecmp(t->xsurface->class, app_name) == 0) {
			return t;
		}
	}
	/* pass 3: prefix match (e.g. app "gnome-terminal" vs app_id
	 * "gnome-terminal-server") */
	wl_list_for_each(t, &server->toplevels, link) {
		const char *id = toplevel_app_id(t);
		if (id == NULL || id[0] == '\0') {
			continue;
		}
		if (strncasecmp(id, app_name, strlen(app_name)) == 0 ||
				strncasecmp(app_name, id, strlen(id)) == 0) {
			return t;
		}
	}
	return NULL;
}

bool toplevel_is_xwayland(struct guibux_toplevel *toplevel) {
	return toplevel->xsurface != NULL;
}

void toplevel_set_size(struct guibux_toplevel *toplevel, int width, int height) {
	/* wlr_xdg_toplevel_set_size asserts non-negative sizes; a tiny
	 * output or a huge topbar can compute a negative height */
	if (width < 0) {
		width = 0;
	}
	if (height < 0) {
		height = 0;
	}
	if (toplevel->xdg_toplevel != NULL) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
	} else if (width > 0 && height > 0) {
		/* 0,0 has no meaning for X11 windows; the app keeps its size.
		 * Skip no-op configures: they send a synthetic ConfigureNotify
		 * and would loop with the commit handler in tile mode */
		if (width == toplevel->xsurface->width &&
				height == toplevel->xsurface->height &&
				toplevel->scene_tree->node.x == toplevel->xsurface->x &&
				toplevel->scene_tree->node.y == toplevel->xsurface->y) {
			return;
		}
		wlr_xwayland_surface_configure(toplevel->xsurface,
			toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
			width, height);
	}
}

void toplevel_get_geometry(struct guibux_toplevel *toplevel,
		struct wlr_box *box) {
	if (toplevel->xdg_toplevel != NULL) {
		*box = toplevel->xdg_toplevel->base->geometry;
	} else {
		box->x = 0;
		box->y = 0;
		box->width = toplevel->xsurface->width;
		box->height = toplevel->xsurface->height;
	}
}

void toplevel_close(struct guibux_toplevel *toplevel) {
	if (toplevel->xdg_toplevel != NULL) {
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
	} else {
		wlr_xwayland_surface_close(toplevel->xsurface);
	}
}

void toplevel_set_activated(struct guibux_toplevel *toplevel, bool activated) {
	if (toplevel->xdg_toplevel != NULL) {
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, activated);
	} else {
		wlr_xwayland_surface_activate(toplevel->xsurface, activated);
	}
}

void toplevel_set_fullscreen_state(struct guibux_toplevel *toplevel,
		bool fullscreen) {
	if (toplevel->xdg_toplevel != NULL) {
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);
	} else {
		wlr_xwayland_surface_set_fullscreen(toplevel->xsurface, fullscreen);
	}
}

// ---------------------------------------------------------------------------
// xwayland surface lifecycle
// ---------------------------------------------------------------------------

static void xsurface_associate(struct wl_listener *listener, void *data);
static void xsurface_dissociate(struct wl_listener *listener, void *data);
static void xsurface_map(struct wl_listener *listener, void *data);
static void xsurface_unmap(struct wl_listener *listener, void *data);
static void xsurface_commit(struct wl_listener *listener, void *data);
static void xsurface_destroy(struct wl_listener *listener, void *data);
static void xsurface_request_move(struct wl_listener *listener, void *data);
static void xsurface_request_resize(struct wl_listener *listener, void *data);
static void xsurface_request_fullscreen(struct wl_listener *listener, void *data);
static void xsurface_request_activate(struct wl_listener *listener, void *data);
static void xsurface_request_close(struct wl_listener *listener, void *data);
static void xsurface_request_configure(struct wl_listener *listener, void *data);
static void xsurface_set_title(struct wl_listener *listener, void *data);
static void xsurface_ping_timeout(struct wl_listener *listener, void *data);

void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	struct guibux_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xsurface = xsurface;
	toplevel->managed = !xsurface->override_redirect;

	toplevel->associate.notify = xsurface_associate;
	wl_signal_add(&xsurface->events.associate, &toplevel->associate);
	toplevel->dissociate.notify = xsurface_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &toplevel->dissociate);
	toplevel->destroy.notify = xsurface_destroy;
	wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
	toplevel->request_move.notify = xsurface_request_move;
	wl_signal_add(&xsurface->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xsurface_request_resize;
	wl_signal_add(&xsurface->events.request_resize, &toplevel->request_resize);
	toplevel->request_fullscreen.notify = xsurface_request_fullscreen;
	wl_signal_add(&xsurface->events.request_fullscreen,
		&toplevel->request_fullscreen);
	toplevel->request_activate.notify = xsurface_request_activate;
	wl_signal_add(&xsurface->events.request_activate, &toplevel->request_activate);
	toplevel->request_close.notify = xsurface_request_close;
	wl_signal_add(&xsurface->events.request_close, &toplevel->request_close);
	toplevel->request_configure.notify = xsurface_request_configure;
	wl_signal_add(&xsurface->events.request_configure,
		&toplevel->request_configure);
	toplevel->set_title.notify = xsurface_set_title;
	wl_signal_add(&xsurface->events.set_title, &toplevel->set_title);
	toplevel->ping_timeout.notify = xsurface_ping_timeout;
	wl_signal_add(&xsurface->events.ping_timeout, &toplevel->ping_timeout);
}

static void xsurface_associate(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, associate);
	struct wlr_xwayland_surface *xsurface = toplevel->xsurface;

	/* wrapper tree so children (e.g. overview labels) can attach,
	 * matching the xdg scene tree layout */
	toplevel->scene_tree = wlr_scene_tree_create(&toplevel->server->scene->tree);
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_surface_create(toplevel->scene_tree, xsurface->surface);

	toplevel->scene_destroy.notify = toplevel_scene_destroyed;
	wl_signal_add(&toplevel->scene_tree->node.events.destroy,
		&toplevel->scene_destroy);

	toplevel->map.notify = xsurface_map;
	wl_signal_add(&xsurface->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xsurface_unmap;
	wl_signal_add(&xsurface->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xsurface_commit;
	wl_signal_add(&xsurface->surface->events.commit, &toplevel->commit);
}

static void xsurface_dissociate(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, dissociate);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wlr_scene_node_destroy(&toplevel->scene_tree->node);
	toplevel->scene_tree = NULL;
}

static void xsurface_map(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct wlr_xwayland_surface *xsurface = toplevel->xsurface;

	if (toplevel->server->overview.active) {
		overview_hide(toplevel->server);
	}

	wlr_log(WLR_INFO, "mapped xwayland toplevel '%s' (OR=%d)",
		xsurface->title ? xsurface->title : "(untitled)",
		xsurface->override_redirect);

	if (!toplevel->managed) {
		/* override-redirect (menus, tooltips): place where the app
		 * asked, no tiling, no topbar, no focus stealing */
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			xsurface->x, xsurface->y);
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		topbar_raise_all(toplevel->server);
		if (wlr_xwayland_surface_override_redirect_wants_focus(xsurface)) {
			focus_toplevel(toplevel, true);
		}
		return;
	}

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	toplevel->initial_commit = true;

	struct wlr_output *output = output_at_cursor(toplevel->server);
	struct guibux_output *o = output != NULL
		? guibux_output_for(toplevel->server, output) : NULL;
	enum restore_result rr = restore_apply(toplevel->server, toplevel);
	if (rr != RESTORE_FREE) {
		if (rr == RESTORE_TILE) {
			/* saved output+workspace resolved: tile it there */
			o = toplevel->output;
		}
		toplevel->output = o;
		toplevel->workspace = o != NULL ? o->current_workspace : 1;
		if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
			struct wlr_box box;
			wlr_output_layout_get_box(toplevel->server->output_layout,
				o->wlr_output, &box);
			wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
			retile_output(o);
		} else {
			place_toplevel(toplevel);
		}
	}
	focus_toplevel(toplevel, true);
	if (xsurface->fullscreen) {
		set_fullscreen(toplevel, true, NULL);
	}
	struct guibux_output *o2 = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o2)
		topbar_mark_dirty(o2);
	effects_window_open(toplevel);
}

static void xsurface_unmap(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct guibux_server *server = toplevel->server;

	if (!toplevel->managed) {
		return;
	}

	if (server->overview.active) {
		overview_hide(server);
	}

	/* remember the final position for the next launch */
	restore_save(server, toplevel);

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	toplevel->open_effect_pending = false;
	wl_list_remove(&toplevel->link);
	switcher_on_unmap(server, toplevel);
	topbar_win_remove(o, toplevel);
	if (o != NULL) {
		effects_window_closed(toplevel, o);
	}
	topbar_mark_dirty(o);
	/* the pill list is global: the removed window must vanish from
	 * every bar, not just its own */
	struct guibux_output *bo;
	wl_list_for_each(bo, &server->outputs, link) {
		if (bo != o) {
			topbar_mark_dirty(bo);
		}
	}

	focus_after_unmap(server);
}

static void xsurface_commit(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (!toplevel->managed || !toplevel->xsurface->surface->mapped) {
		return;
	}
	if (toplevel->open_effect_pending) {
		effects_window_open_start(toplevel);
	}

	/* like xdg_toplevel_commit: only re-assert the tile geometry on the
	 * first commit (the app may have mapped at its own size). Retiling on
	 * every commit would fight apps that enforce a minimum size larger
	 * than the tile cell (configure/commit loop) and would snap a window
	 * being dragged back to its tile slot on every frame */
	if (!toplevel->initial_commit) {
		return;
	}
	toplevel->initial_commit = false;
	struct guibux_output *o = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
		retile_output(o);
	}
}

static void xsurface_destroy(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	wlr_log(WLR_INFO, "destroyed xwayland toplevel '%s'",
		toplevel->xsurface->title ? toplevel->xsurface->title : "(untitled)");

	if (toplevel->scene_tree != NULL) {
		wl_list_remove(&toplevel->map.link);
		wl_list_remove(&toplevel->unmap.link);
		wl_list_remove(&toplevel->commit.link);
	}
	wl_list_remove(&toplevel->associate.link);
	wl_list_remove(&toplevel->dissociate.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->request_activate.link);
	wl_list_remove(&toplevel->request_close.link);
	wl_list_remove(&toplevel->request_configure.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->ping_timeout.link);

	toplevel->open_effect_pending = false;
	if (toplevel->scene_tree != NULL) {
		effects_cancel_node(toplevel->server, &toplevel->scene_tree->node);
		toplevel->scene_tree->node.data = NULL;
	}

	struct guibux_output *fo = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (fo)
		topbar_mark_dirty(fo);

	if (toplevel->server->last_ffm_toplevel == toplevel) {
		toplevel->server->last_ffm_toplevel = NULL;
	}

	free(toplevel);
}

static void xsurface_request_move(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	if (toplevel->scene_tree == NULL) {
		return;
	}
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
}

static void xsurface_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xwayland_resize_event *event = data;
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	if (toplevel->scene_tree == NULL) {
		return;
	}
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_RESIZE, event->edges);
}

static void xsurface_request_fullscreen(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->scene_tree == NULL) {
		return;
	}
	set_fullscreen(toplevel, toplevel->xsurface->fullscreen, NULL);
}

static void xsurface_request_activate(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_activate);
	if (toplevel->scene_tree == NULL) {
		return;
	}
	focus_toplevel(toplevel, true);
}

// ---------------------------------------------------------------------------
// xdg-activation-v1: a background app (portal, gtk-launch, script)
// requests focus of a window it spawned
// ---------------------------------------------------------------------------

void xdg_activation_handle_request(struct wl_listener *listener, void *data) {
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	struct guibux_server *server = wl_container_of(listener, server,
		xdg_activation_request);
	/* the requesting surface's client owns the window to activate;
	 * prefer the currently focused toplevel of that client, falling
	 * back to its most-recently-mapped one (the list is ordered by
	 * map time, not focus) */
	struct wl_client *client = NULL;
	if (ev->surface != NULL && ev->surface->resource != NULL) {
		client = wl_resource_get_client(ev->surface->resource);
	}
	struct guibux_toplevel *t = NULL;
	struct guibux_toplevel *fallback = NULL;
	struct wlr_surface *kb_focus =
		server->seat->keyboard_state.focused_surface;
	struct guibux_toplevel *kb_focus_t = NULL;
	if (kb_focus != NULL) {
		struct guibux_toplevel *tmp;
		wl_list_for_each(tmp, &server->toplevels, link) {
			struct wlr_surface *s = toplevel_get_surface(tmp);
			if (s == kb_focus) {
				kb_focus_t = tmp;
				break;
			}
		}
	}
	struct guibux_toplevel *tmp;
	wl_list_for_each(tmp, &server->toplevels, link) {
		struct wlr_surface *s = toplevel_get_surface(tmp);
		if (s == NULL || s->resource == NULL) {
			continue;
		}
		if (wl_resource_get_client(s->resource) != client) {
			continue;
		}
		if (tmp == kb_focus_t) {
			t = tmp;
			break;
		}
		if (fallback == NULL) {
			fallback = tmp;
		}
	}
	if (t == NULL) {
		t = fallback;
	}
	if (t == NULL || t->scene_tree == NULL) {
		return;
	}
	/* the window may live on another workspace: switch to it first */
	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(t));
	if (o != NULL && t->workspace != o->current_workspace) {
		switch_workspace(o, t->workspace);
	}
	focus_toplevel(t, true);
	/* no cursor warp: the activation usually follows a click in another
	 * app (e.g. a link opening the browser); warping moves pointer focus
	 * onto the new window mid-click and the button release lands on it,
	 * which the app reads as a click on its content */
}

static void xsurface_request_close(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_close);
	toplevel_close(toplevel);
}

static void xsurface_request_configure(struct wl_listener *listener, void *data) {
	struct wlr_xwayland_surface_configure_event *event = data;
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_configure);
	if (!toplevel->managed || toplevel->is_fullscreen ||
			toplevel->scene_tree == NULL) {
		return;
	}
	struct guibux_output *o = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o != NULL && o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
		/* tile mode: the layout is authoritative. Accepting the app's
		 * geometry would break the tiling, and re-asserting the tile
		 * size would loop with apps that re-assert their minimum size
		 * on every ConfigureNotify */
		return;
	}
	/* free mode: accept the app's requested geometry. X11 apps don't
	 * know about the topbar and may re-assert a position at the top of
	 * the screen (e.g. on activation), sliding a snapped window under
	 * the bar so its visible height loses topbar_height on every focus:
	 * keep the requested top edge inside the work area */
	int nx = (int)toplevel->scene_tree->node.x;
	int ny = (int)toplevel->scene_tree->node.y;
	if (event->mask & XCB_CONFIG_WINDOW_X) {
		nx = event->x;
	}
	if (event->mask & XCB_CONFIG_WINDOW_Y) {
		ny = event->y;
		/* the request may move the window to another output: clamp
		 * against the work area of the output under the new center */
		struct wlr_output *target = wlr_output_layout_output_at(
			toplevel->server->output_layout,
			nx + event->width / 2, ny + event->height / 2);
		if (target != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(toplevel->server->output_layout,
				target, &box);
			if (ny < box.y + toplevel->server->topbar_height) {
				ny = box.y + toplevel->server->topbar_height;
			}
		}
	}
	toplevel->scene_tree->node.x = nx;
	toplevel->scene_tree->node.y = ny;
	/* skip no-op configures: an app that re-asserts its position on
	 * every ConfigureNotify would otherwise loop (it requests the
	 * pre-clamp position, we answer with the clamped one, it requests
	 * again, ...) */
	if (nx == toplevel->xsurface->x && ny == toplevel->xsurface->y &&
			event->width == toplevel->xsurface->width &&
			event->height == toplevel->xsurface->height) {
		return;
	}
	wlr_xwayland_surface_configure(toplevel->xsurface, nx, ny,
		event->width, event->height);
}

static void xsurface_set_title(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	if (!toplevel->managed || toplevel->scene_tree == NULL) {
		return;
	}
	struct guibux_output *o = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o)
		topbar_mark_dirty(o);
}

static void xsurface_ping_timeout(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, ping_timeout);
	wlr_log(WLR_INFO, "xwayland ping timeout, closing '%s'",
		toplevel->xsurface->title ? toplevel->xsurface->title : "(untitled)");
	wlr_xwayland_surface_close(toplevel->xsurface);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

struct guibux_toplevel *desktop_toplevel_at(
		struct guibux_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	return tree->node.data;
}
