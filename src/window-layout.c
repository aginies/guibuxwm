#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Tiling layout
// ---------------------------------------------------------------------------

void retile_output(struct guibux_output *output) {
	struct guibux_server *server = output->server;
	if (server->overview.active) {
		return;
	}
	if (output->tile_mode == GUIBUX_TILE_FREE) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	box.y += output->server->topbar_height;
	box.height -= output->server->topbar_height;
	if (box.height <= 0) {
		return;
	}

	struct guibux_toplevel *wins[64];
	int n = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->is_fullscreen || !toplevel_visible(t)) {
			continue;
		}
		if (toplevel_output_for(t) != output->wlr_output) {
			continue;
		}
		if (n < 64) {
			wins[n++] = t;
		}
	}
	if (n == 0) {
		return;
	}

	for (int i = 0; i < n; i++) {
		int rx, ry, rw, rh;
		if (output->tile_mode == GUIBUX_TILE_SPLIT) {
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
		wlr_scene_node_set_position(&wins[i]->scene_tree->node,
			box.x + rx, box.y + ry);
		wlr_xdg_toplevel_set_size(wins[i]->xdg_toplevel, rw, rh);
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

void switch_workspace(struct guibux_output *output, int ws) {
	if (ws < 1 || ws > NUM_WORKSPACES || ws == output->current_workspace) {
		return;
	}
	struct guibux_server *server = output->server;
	output->current_workspace = ws;
	background_render(output);

	end_seat_grabs(server);

	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (toplevel_output_for(t) == output->wlr_output) {
			wlr_scene_node_set_enabled(&t->scene_tree->node,
				t->workspace == ws);
		}
	}
	retile_output(output);

	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	struct wlr_xdg_toplevel *focused_xdg = NULL;
	if (focused != NULL) {
		focused_xdg = wlr_xdg_toplevel_try_from_wlr_surface(focused);
	}
	struct guibux_toplevel *focused_toplevel = NULL;
	if (focused_xdg != NULL) {
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->xdg_toplevel == focused_xdg) {
				focused_toplevel = t;
				break;
			}
		}
	}
	if (focused_toplevel != NULL && !toplevel_visible(focused_toplevel)) {
		struct guibux_toplevel *next = NULL;
		wl_list_for_each(t, &server->toplevels, link) {
			if (toplevel_visible(t)) {
				next = t;
				break;
			}
		}
		if (next != NULL) {
			focus_toplevel(next);
		} else {
			clear_keyboard_focus(server);
		}
	}
	topbar_mark_dirty(output);
}

void move_toplevel_to_workspace(struct guibux_toplevel *toplevel, int ws) {
	if (ws < 1 || ws > NUM_WORKSPACES || toplevel->workspace == ws) {
		return;
	}
	struct guibux_server *server = toplevel->server;
	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	end_seat_grabs(server);
	toplevel->workspace = ws;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node,
		o != NULL && ws == o->current_workspace);
	if (o != NULL) {
		retile_output(o);
		topbar_mark_dirty(o);
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
		set_fullscreen(toplevel, false);
	}
	struct wlr_output *src = toplevel_output_for(toplevel);
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
	int32_t w = geo.width > 0 ? geo.width : 800;
	int32_t h = geo.height > 0 ? geo.height : 600;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x + (box.width - w) / 2,
		box.y + (box.height - h) / 2);
	wlr_log(WLR_INFO, "moved '%s' to output %s",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)",
		output->name ? output->name : "(unknown)");
	struct guibux_output *o = guibux_output_for(server, output);
	if (o != NULL) {
		toplevel->workspace = o->current_workspace;
		if (o->tile_mode != GUIBUX_TILE_FREE) {
			retile_output(o);
		}
	}
	if (src != output) {
		struct guibux_output *so = guibux_output_for(server, src);
		if (so != NULL && so->tile_mode != GUIBUX_TILE_FREE) {
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
		set_fullscreen(toplevel, false);
	}
	int w = box.width / 2;
	int h = box.height - toplevel->server->topbar_height;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + toplevel->server->topbar_height);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
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
		set_fullscreen(toplevel, false);
	}
	int rx = box.width / 2;
	int w = box.width - box.width / 2;
	int h = box.height - server->topbar_height;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x + rx, box.y + server->topbar_height);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
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
		set_fullscreen(toplevel, false);
	}
	int w = box.width;
	int h = (box.height - toplevel->server->topbar_height) / 2;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + toplevel->server->topbar_height);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
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
		set_fullscreen(toplevel, false);
	}
	int w = box.width;
	int h = (box.height - toplevel->server->topbar_height) - (box.height - toplevel->server->topbar_height) / 2;
	int ry = toplevel->server->topbar_height + (box.height - toplevel->server->topbar_height) / 2;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x, box.y + ry);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
}
