#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

void focus_toplevel(struct guibux_toplevel *toplevel) {
	if (toplevel == NULL) {
		return;
	}
	struct guibux_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	topbar_raise_all(server);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
	struct guibux_output *fo = guibux_output_for(server,
		toplevel_output_for(toplevel));
	if (fo)
		topbar_mark_dirty(fo);
}

// ---------------------------------------------------------------------------
// Fullscreen
// ---------------------------------------------------------------------------

void set_fullscreen(struct guibux_toplevel *toplevel, bool fullscreen) {
	if (fullscreen == toplevel->is_fullscreen) {
		return;
	}
	struct guibux_server *server = toplevel->server;
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;

	if (fullscreen) {
		toplevel->saved_x = toplevel->scene_tree->node.x;
		toplevel->saved_y = toplevel->scene_tree->node.y;

		struct wlr_output *output = toplevel_output_for(toplevel);
		if (output != NULL) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output, &box);
			int th = server->topbar_height;
			int ew, eh;
			wlr_output_effective_resolution(output, &ew, &eh);
			/* fullscreen below the topbar, so the bar stays visible */
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				box.x, box.y + th);
			wlr_xdg_toplevel_set_size(xdg_toplevel, ew, eh - th);
		}
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
		topbar_raise_all(server);
	} else {
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_x, toplevel->saved_y);
		wlr_xdg_toplevel_set_size(xdg_toplevel, 0, 0);
	}

	toplevel->is_fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, fullscreen);
	if (!fullscreen) {
		struct guibux_output *o = guibux_output_for(server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
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
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
	}
}

// ---------------------------------------------------------------------------
// xdg_toplevel lifecycle
// ---------------------------------------------------------------------------

void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct guibux_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&toplevel->server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

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
	toplevel->workspace = o != NULL ? o->current_workspace : 1;
	if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
		struct wlr_box box;
		wlr_output_layout_get_box(toplevel->server->output_layout,
			output, &box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
		retile_output(o);
	} else {
		place_toplevel(toplevel);
	}
	focus_toplevel(toplevel);
	struct guibux_output *o2 = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	if (o2)
		topbar_mark_dirty(o2);
}

void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct guibux_server *server = toplevel->server;

	if (server->overview.active) {
		overview_hide(server);
	}

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	wl_list_remove(&toplevel->link);
	if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
		retile_output(o);
	}
	topbar_mark_dirty(o);

	if (!wl_list_empty(&server->toplevels)) {
		struct guibux_toplevel *next =
			wl_container_of(server->toplevels.next, next, link);
		focus_toplevel(next);
	} else {
		clear_keyboard_focus(server);
	}
}

void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		struct guibux_output *o = guibux_output_for(toplevel->server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
			wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
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
		set_fullscreen(toplevel, false);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
}

void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false);
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
	set_fullscreen(toplevel, toplevel->xdg_toplevel->requested.fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
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
