#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Helpers: output lookup, toplevel output, visibility
// ---------------------------------------------------------------------------

struct wlr_output *output_at_cursor(struct guibux_server *server) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output != NULL) {
		return output;
	}
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		return o->wlr_output;
	}
	return NULL;
}

struct wlr_output *toplevel_output_for(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	if (toplevel->scene_tree == NULL) {
		return NULL;
	}
	/* requested.fullscreen_output is only cleared when the client
	 * resets the toplevel, so it goes stale after un-fullscreening;
	 * trust it only while the window is actually fullscreen */
	if (toplevel->xdg_toplevel != NULL &&
			toplevel->is_fullscreen &&
			toplevel->xdg_toplevel->requested.fullscreen_output != NULL) {
		return toplevel->xdg_toplevel->requested.fullscreen_output;
	}
	struct wlr_box geo;
	toplevel_get_geometry(toplevel, &geo);
	int cx = toplevel->scene_tree->node.x + geo.width / 2;
	int cy = toplevel->scene_tree->node.y + geo.height / 2;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (wlr_output_layout_contains_point(server->output_layout,
				o->wlr_output, cx, cy)) {
			return o->wlr_output;
		}
	}
	wl_list_for_each(o, &server->outputs, link) {
		return o->wlr_output;
	}
	return NULL;
}

struct guibux_output *guibux_output_for(struct guibux_server *server,
		struct wlr_output *wlr_output) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_output) {
			return o;
		}
	}
	return NULL;
}

bool toplevel_visible(struct guibux_toplevel *toplevel) {
	struct guibux_output *o = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	return o != NULL && toplevel->workspace == o->current_workspace;
}

// ---------------------------------------------------------------------------
// Topbar helpers
// ---------------------------------------------------------------------------

void topbar_raise_all(struct guibux_server *server) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_node != NULL) {
			wlr_scene_node_raise_to_top(&o->topbar_node->node);
		}
	}
}

int outputs_sorted_by_x(struct guibux_server *server,
		struct wlr_output **sorted, struct wlr_box *boxes, int cap) {
	int n = 0;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (n >= cap) {
			break;
		}
		sorted[n] = o->wlr_output;
		wlr_output_layout_get_box(server->output_layout, sorted[n], &boxes[n]);
		n++;
	}
	for (int i = 1; i < n; i++) {
		struct wlr_output *so = sorted[i];
		struct wlr_box sb = boxes[i];
		int j = i - 1;
		while (j >= 0 && boxes[j].x > sb.x) {
			sorted[j + 1] = sorted[j];
			boxes[j + 1] = boxes[j];
			j--;
		}
		sorted[j + 1] = so;
		boxes[j + 1] = sb;
	}
	return n;
}

// ---------------------------------------------------------------------------
// Output lifecycle
// ---------------------------------------------------------------------------

void server_new_output(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	const struct output_placement *placement = NULL;
	for (int i = 0; i < server->num_placements; i++) {
		if (strcmp(wlr_output->name, server->placements[i].name) == 0) {
			placement = &server->placements[i];
			break;
		}
	}

	if (server->num_placements > 0 && placement == NULL) {
		wlr_log(WLR_INFO, "%s: not in GUIBUX_OUTPUTS, disabling", wlr_output->name);
		wlr_output_destroy(wlr_output);
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	if (placement != NULL && placement->transform >= 0) {
		struct wlr_output_state tstate;
		wlr_output_state_init(&tstate);
		wlr_output_state_set_transform(&tstate, placement->transform);
		if (!wlr_output_commit_state(wlr_output, &tstate)) {
			wlr_log(WLR_ERROR, "%s: failed to apply transform %d "
				"(backend may not support it)",
				wlr_output->name, placement->transform);
		}
		wlr_output_state_finish(&tstate);
	}

	struct guibux_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->current_workspace = 1;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output = NULL;
	if (placement != NULL) {
		l_output = wlr_output_layout_add(server->output_layout, wlr_output,
			placement->x, placement->y);
	}
	if (l_output == NULL) {
		l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
	}
	if (l_output == NULL) {
		wlr_log(WLR_ERROR, "%s: failed to add output to layout",
			wlr_output->name);
	} else {
		struct wlr_scene_output *scene_output =
			wlr_scene_output_create(server->scene, wlr_output);
		if (scene_output == NULL) {
			wlr_log(WLR_ERROR, "%s: failed to create scene output",
				wlr_output->name);
		} else {
			wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
		}
	}
	/* background first: wlr-scene draws later children on top, so the
	 * topbar node must come after the background or the bar is hidden
	 * until the first toplevel raises it */
	background_create(output);
	topbar_create(output);
	topbar_renumber(server);
}

void output_frame(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, frame);
	struct guibux_server *server = output->server;
	struct wlr_scene *scene = server->scene;

	/* output resized or rescaled: the topbar buffer is stale */
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	int scale = output->wlr_output->scale > 1 ? (int)output->wlr_output->scale : 1;
	if (output->topbar_buffer != NULL &&
			(output->topbar_buffer_w != box.width * scale ||
			 output->topbar_buffer_h != server->topbar_height * scale)) {
		output->topbar_dirty = true;
	}

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

void output_request_state(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

void output_destroy(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, destroy);
	struct guibux_server *server = output->server;

	topbar_destroy(output);
	background_destroy(output);
	if (output->overview_dim) {
		wlr_scene_node_destroy(&output->overview_dim->node);
		output->overview_dim = NULL;
	}
	if (output->overview_ws_col_node) {
		wlr_scene_node_destroy(&output->overview_ws_col_node->node);
		output->overview_ws_col_node = NULL;
	}
	if (output->overview_ws_col_buf) {
		wlr_buffer_drop(output->overview_ws_col_buf);
		output->overview_ws_col_buf = NULL;
	}
	topbar_renumber(server);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}
