#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Tiling layout
// ---------------------------------------------------------------------------

/* compute the tile cells for every visible, non-fullscreen window on
 * the output; absolute node positions and logical sizes */
int retile_compute(struct guibux_output *output, struct guibux_tile_target *out,
		int cap) {
	struct guibux_server *server = output->server;
	if (server->overview.active) {
		return 0;
	}
	if (output->tile_modes[output->current_workspace] == GUIBUX_TILE_FREE) {
		return 0;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return 0;
	}
	box.y += output->server->topbar_height;
	box.height -= output->server->topbar_height;
	if (box.height <= 0) {
		return 0;
	}

	int n = 0;
	struct guibux_toplevel *wins[MAX_WINDOWS];
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || !toplevel_visible(t)) {
			continue;
		}
		if (toplevel_output_for(t) != output->wlr_output) {
			continue;
		}
		if (n < MAX_WINDOWS && n < cap) {
			wins[n++] = t;
		}
	}
	if (n == 0) {
		return 0;
	}

	for (int i = 0; i < n; i++) {
		int rx, ry, rw, rh;
		if (output->tile_modes[output->current_workspace] == GUIBUX_TILE_SPLIT) {
			int col = i % 2;
			int row = i / 2;
			int per_col = (col == 0) ? (n + 1) / 2 : n / 2;
			rx = (box.width / 2) * col;
			rw = (col == 0) ? box.width / 2 : box.width - box.width / 2;
			ry = (box.height * row) / per_col;
			rh = (box.height * (row + 1)) / per_col - ry;
		} else { // GUIBUX_TILE_MAIN_STACK
			if (i == 0) {
				rx = 0;
				ry = 0;
				rw = box.width / 2;
				rh = box.height;
			} else {
				int stack = n - 1;
				int row = i - 1;
				rx = box.width / 2;
				rw = box.width - box.width / 2;
				ry = (box.height * row) / stack;
				rh = (box.height * (row + 1)) / stack - ry;
			}
		}
		out[i].t = wins[i];
		out[i].x = box.x + rx;
		out[i].y = box.y + ry;
		out[i].w = rw;
		out[i].h = rh;
	}
	return n;
}

void retile_output(struct guibux_output *output) {
	struct guibux_server *server = output->server;
	/* an in-flight animation (close retile) would
	 * overwrite the positions set here: settle it first */
	effects_cancel_output(server, output);

	struct guibux_tile_target targets[MAX_WINDOWS];
	int n = retile_compute(output, targets, MAX_WINDOWS);
	for (int i = 0; i < n; i++) {
		wlr_scene_node_set_position(&targets[i].t->scene_tree->node,
			targets[i].x, targets[i].y);
		toplevel_set_size(targets[i].t, targets[i].w, targets[i].h);
	}
}

// ---------------------------------------------------------------------------
// Workspaces
// ---------------------------------------------------------------------------

void clear_keyboard_focus(struct guibux_server *server) {
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void end_seat_grabs(struct guibux_server *server) {
	if (wlr_seat_keyboard_has_grab(server->seat)) {
		wlr_seat_keyboard_end_grab(server->seat);
	}
	if (wlr_seat_pointer_has_grab(server->seat)) {
		wlr_seat_pointer_end_grab(server->seat);
	}
}

/* after a workspace switch, focus the window under the pointer on the
 * switched output; if the pointer is on empty space, focus the first
 * visible window of that output */
static void ws_switch_refocus(struct guibux_output *output) {
	struct guibux_server *server = output->server;
	struct wlr_surface *surface;
	double sx, sy;
	struct guibux_toplevel *at = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (at != NULL && toplevel_visible(at) &&
			toplevel_output_for(at) == output->wlr_output) {
		focus_toplevel(at, toplevel_covered(at));
		return;
	}
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (toplevel_visible(t) &&
				toplevel_output_for(t) == output->wlr_output) {
			focus_toplevel(t, true);
			return;
		}
	}
	clear_keyboard_focus(server);
}

/* state change only: current workspace, background, grabs, focus,
 * topbar. Scene node visibility is applied by the caller (immediately,
 * or by the transition animation) */
void ws_switch_state(struct guibux_output *output, int ws) {
	output->tile_modes[output->current_workspace] = output->tile_mode;
	output->tile_mode = output->tile_modes[ws];
	struct guibux_server *server = output->server;
	output->current_workspace = ws;
	background_render(output);

	end_seat_grabs(server);
	ws_switch_refocus(output);
	topbar_mark_dirty(output);
}

void ws_switch_immediate(struct guibux_output *output, int ws) {
	ws_switch_state(output, ws);
	struct guibux_server *server = output->server;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->scene_tree != NULL &&
				toplevel_output_for(t) == output->wlr_output) {
			wlr_scene_node_set_enabled(&t->scene_tree->node,
				t->workspace == ws);
		}
	}
	/* the focused window's border is a child of its scene tree; when the
	 * tree was disabled (hidden workspace) the border node was hidden too.
	 * Re-show it now that the tree is visible again */
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	wl_list_for_each(t, &server->toplevels, link) {
		if (toplevel_get_surface(t) == focused) {
			toplevel_border_show(t);
			break;
		}
	}
	retile_output(output);
}

void switch_workspace(struct guibux_output *output, int ws) {
	if (ws < 1 || ws > NUM_WORKSPACES || ws == output->current_workspace) {
		return;
	}
	ws_switch_immediate(output, ws);
	osd_ws(output->server, output, ws);
}

void move_toplevel_to_workspace(struct guibux_toplevel *toplevel, int ws) {
	if (ws < 1 || ws > NUM_WORKSPACES || toplevel->workspace == ws) {
		return;
	}
	struct guibux_server *server = toplevel->server;
	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	bool was_focused = focused == toplevel_get_surface(toplevel);
	end_seat_grabs(server);
	toplevel->workspace = ws;
	toplevel_border_refresh(toplevel);
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
		o != NULL && ws == o->current_workspace);
	if (o != NULL) {
		retile_output(o);
		topbar_mark_dirty(o);
	}
	/* the focused window may have moved to a hidden workspace:
	 * keyboard focus must not stay on an invisible window */
	if (was_focused && !toplevel_visible(toplevel)) {
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
}

void place_toplevel(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	int32_t ox = box.x + 100 + (server->cascade % CASCADE_MAX) * CASCADE_STEP;
	int32_t oy = box.y + 80 + (server->cascade % CASCADE_MAX) * CASCADE_STEP;
	server->cascade++;
	wlr_scene_node_set_position(&toplevel->scene_tree->node, ox, oy);
}

void move_toplevel_to_output(struct guibux_toplevel *toplevel, struct wlr_output *output) {
	struct guibux_server *server = toplevel->server;
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	struct wlr_output *src = toplevel_output_for(toplevel);
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	struct wlr_box geo;
	toplevel_get_geometry(toplevel, &geo);
	int32_t w = geo.width > 0 ? geo.width : 800;
	int32_t h = geo.height > 0 ? geo.height : 600;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x + (box.width - w) / 2,
		box.y + (box.height - h) / 2);
	wlr_log(WLR_INFO, "moved '%s' to output %s",
		toplevel_get_title(toplevel) ? toplevel_get_title(toplevel) : "(untitled)",
		output->name ? output->name : "(unknown)");
	struct guibux_output *o = guibux_output_for(server, output);
	if (o != NULL) {
		toplevel->output = o;
		toplevel->workspace = o->current_workspace;
		toplevel_border_refresh(toplevel);
		topbar_mark_dirty(o);
		if (o->tile_modes[o->current_workspace] != GUIBUX_TILE_FREE) {
			retile_output(o);
		}
	}
	if (src != output) {
		struct guibux_output *so = guibux_output_for(server, src);
		topbar_mark_dirty(so);
		if (so != NULL && so->tile_modes[so->current_workspace] != GUIBUX_TILE_FREE) {
			retile_output(so);
		}
	}
}

void move_toplevel_to_adjacent_output(struct guibux_server *server,
		struct guibux_toplevel *toplevel, int dir) {
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	if (n < 2) {
		return;
	}
	struct wlr_output *cur_output = toplevel_output_for(toplevel);
	int cur = 0;
	for (int i = 0; i < n; i++) {
		if (sorted[i] == cur_output) {
			cur = i;
			break;
		}
	}
	int next = cur + dir;
	if (next < 0 || next >= n) {
		return;
	}
	move_toplevel_to_output(toplevel, sorted[next]);
}

// ---------------------------------------------------------------------------
// Snap (half-screen positioning)
// ---------------------------------------------------------------------------

void snap_toplevel_left(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = toplevel_output_for(toplevel);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	int w = box.width / 2;
	int h = box.height - toplevel->server->topbar_height;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + toplevel->server->topbar_height);
	toplevel_set_size(toplevel, w, h);
}

void snap_toplevel_right(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = toplevel_output_for(toplevel);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	int rx = box.width / 2;
	int w = box.width - box.width / 2;
	int h = box.height - server->topbar_height;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x + rx, box.y + server->topbar_height);
	toplevel_set_size(toplevel, w, h);
}

void snap_toplevel_top(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = toplevel_output_for(toplevel);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	int w = box.width;
	int h = (box.height - toplevel->server->topbar_height) / 2;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + toplevel->server->topbar_height);
	toplevel_set_size(toplevel, w, h);
}

void snap_toplevel_bottom(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = toplevel_output_for(toplevel);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false, NULL);
	}
	int w = box.width;
	int h = (box.height - toplevel->server->topbar_height) - (box.height - toplevel->server->topbar_height) / 2;
	int ry = toplevel->server->topbar_height + (box.height - toplevel->server->topbar_height) / 2;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + ry);
	toplevel_set_size(toplevel, w, h);
}
