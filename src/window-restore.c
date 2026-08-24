#include "guibuxwm.h"
#include <wlr/util/log.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------------------------------------------------------------------
// State file: app_id|output|workspace|x|y|w|h per line, under
// XDG_STATE_HOME (or ~/.local/state)
// ---------------------------------------------------------------------------

static void restore_state_path(char *path, size_t path_size) {
	const char *state_home = getenv("XDG_STATE_HOME");
	if (state_home != NULL && state_home[0] != '\0') {
		snprintf(path, path_size, "%s/guibuxwm/window-positions", state_home);
		return;
	}
	const char *home = getenv("HOME");
	if (home != NULL) {
		snprintf(path, path_size,
			"%s/.local/state/guibuxwm/window-positions", home);
		return;
	}
	snprintf(path, path_size, "/tmp/guibuxwm-window-positions");
}

static void restore_write_file(struct guibux_server *server) {
	char path[PATH_MAX];
	restore_state_path(path, sizeof(path));
	/* create the directory chain if it does not exist yet */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
		char *slash2 = strrchr(dir, '/');
		if (slash2 != NULL && slash2 != dir) {
			*slash2 = '\0';
			mkdir(dir, 0755);
		}
	}
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		wlr_log(WLR_ERROR, "restore: cannot write %s: %m", path);
		return;
	}
	fprintf(f, "# guibuxwm window positions: app_id|output|workspace|x|y|w|h\n");
	for (int i = 0; i < server->window_restore.count; i++) {
		struct guibux_window_restore_entry *e =
			&server->window_restore.entries[i];
		fprintf(f, "%s|%s|%d|%d|%d|%d|%d\n",
			e->app_id, e->pos.output_name, e->pos.workspace,
			e->pos.x, e->pos.y, e->pos.w, e->pos.h);
	}
	fclose(f);
}

void restore_load(struct guibux_server *server) {
	char path[PATH_MAX];
	restore_state_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		if (errno != ENOENT) {
			wlr_log(WLR_ERROR, "restore: cannot open %s: %m", path);
		}
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '\0' || line[0] == '#') {
			continue;
		}
		char app_id[256], output_name[64];
		int ws, x, y, w, h;
		if (sscanf(line, "%255[^|]|%63[^|]|%d|%d|%d|%d|%d",
				app_id, output_name, &ws, &x, &y, &w, &h) != 7) {
			wlr_log(WLR_ERROR, "restore: bad line '%s'", line);
			continue;
		}
		if (ws < 1 || ws > NUM_WORKSPACES || w <= 0 || h <= 0) {
			wlr_log(WLR_ERROR, "restore: bad values in '%s'", line);
			continue;
		}
		/* one entry per app: a newer line for the same app wins */
		int idx = -1;
		for (int i = 0; i < server->window_restore.count; i++) {
			if (strcasecmp(server->window_restore.entries[i].app_id,
					app_id) == 0) {
				idx = i;
				break;
			}
		}
		if (idx == -1) {
			if (server->window_restore.count >= RESTORE_MAX_ENTRIES) {
				break;
			}
			idx = server->window_restore.count++;
		}
		struct guibux_window_restore_entry *e =
			&server->window_restore.entries[idx];
		snprintf(e->app_id, sizeof(e->app_id), "%s", app_id);
		snprintf(e->pos.output_name, sizeof(e->pos.output_name), "%s",
			output_name);
		e->pos.workspace = ws;
		e->pos.x = x;
		e->pos.y = y;
		e->pos.w = w;
		e->pos.h = h;
	}
	fclose(f);
	wlr_log(WLR_INFO, "restore: loaded %d positions from %s",
		server->window_restore.count, path);
}

void restore_free(struct guibux_server *server) {
	server->window_restore.count = 0;
}

// ---------------------------------------------------------------------------
// Terminal detection: the basename of the first token of `term`
// ---------------------------------------------------------------------------

void restore_derive_terminal_id(struct guibux_server *server) {
	if (server->term_cmd == NULL) {
		return;
	}
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "%s", server->term_cmd);
	char *space = strchr(cmd, ' ');
	if (space != NULL) {
		*space = '\0';
	}
	char *slash = strrchr(cmd, '/');
	server->terminal_app_id = strdup(slash != NULL ? slash + 1 : cmd);
	wlr_log(WLR_INFO, "restore: terminal app_id '%s'",
		server->terminal_app_id);
}

static bool restore_is_terminal(struct guibux_server *server,
		const char *app_id) {
	if (server->terminal_app_id == NULL || app_id == NULL ||
			app_id[0] == '\0') {
		return false;
	}
	/* exact or prefix match, like toplevel_for_app: the terminal's
	 * app_id may carry a suffix (gnome-terminal vs gnome-terminal-server) */
	size_t len = strlen(server->terminal_app_id);
	return strncasecmp(app_id, server->terminal_app_id, len) == 0;
}

// ---------------------------------------------------------------------------
// Save / apply
// ---------------------------------------------------------------------------

void restore_save(struct guibux_server *server, struct guibux_toplevel *toplevel) {
	if (!server->restore_positions) {
		return;
	}
	const char *app_id = toplevel_app_id(toplevel);
	if (app_id == NULL || app_id[0] == '\0' ||
			restore_is_terminal(server, app_id)) {
		return;
	}
	/* a fullscreen window's geometry is the whole output: keep the
	 * last windowed position instead of remembering the fullscreen one */
	if (toplevel->is_fullscreen) {
		return;
	}
	if (toplevel->scene_tree == NULL) {
		return;
	}
	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	if (o == NULL || o->wlr_output->name == NULL) {
		return;
	}
	struct wlr_box geo;
	toplevel_get_geometry(toplevel, &geo);
	if (geo.width <= 0 || geo.height <= 0) {
		return;
	}
	int x = (int)toplevel->scene_tree->node.x;
	int y = (int)toplevel->scene_tree->node.y;
	int ws = toplevel->workspace;

	int idx = -1;
	for (int i = 0; i < server->window_restore.count; i++) {
		if (strcasecmp(server->window_restore.entries[i].app_id,
				app_id) == 0) {
			idx = i;
			break;
		}
	}
	if (idx == -1) {
		if (server->window_restore.count >= RESTORE_MAX_ENTRIES) {
			return;
		}
		idx = server->window_restore.count++;
	}
	struct guibux_window_restore_entry *e =
		&server->window_restore.entries[idx];
	snprintf(e->app_id, sizeof(e->app_id), "%s", app_id);
	snprintf(e->pos.output_name, sizeof(e->pos.output_name), "%s",
		o->wlr_output->name);
	e->pos.workspace = ws;
	e->pos.x = x;
	e->pos.y = y;
	e->pos.w = geo.width;
	e->pos.h = geo.height;

	restore_write_file(server);
	wlr_log(WLR_INFO, "restore: saved '%s' -> %s ws%d %d,%d %dx%d",
		app_id, o->wlr_output->name, ws, x, y, geo.width, geo.height);
}

enum restore_result restore_apply(struct guibux_server *server,
		struct guibux_toplevel *toplevel) {
	if (!server->restore_positions) {
		return RESTORE_NONE;
	}
	const char *app_id = toplevel_app_id(toplevel);
	if (app_id == NULL || app_id[0] == '\0' ||
			restore_is_terminal(server, app_id)) {
		return RESTORE_NONE;
	}
	struct guibux_window_pos pos;
	bool found = false;
	for (int i = 0; i < server->window_restore.count; i++) {
		if (strcasecmp(server->window_restore.entries[i].app_id,
				app_id) == 0) {
			pos = server->window_restore.entries[i].pos;
			found = true;
			break;
		}
	}
	if (!found) {
		return RESTORE_NONE;
	}
	struct guibux_output *saved_o = NULL;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->name != NULL &&
				strcmp(o->wlr_output->name, pos.output_name) == 0) {
			saved_o = o;
			break;
		}
	}
	if (saved_o == NULL) {
		wlr_log(WLR_INFO, "restore: '%s' was on missing output '%s', "
			"placing normally", app_id, pos.output_name);
		return RESTORE_NONE;
	}
	bool tiled = saved_o->tile_modes[pos.workspace] != GUIBUX_TILE_FREE;
	/* a resolution change may have pushed the saved position off the
	 * output: only fall back to normal placement in free mode (in tile
	 * mode the layout places the window anyway) */
	if (!tiled) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			saved_o->wlr_output, &box);
		if (pos.x >= box.x + box.width || pos.y >= box.y + box.height ||
				pos.x + pos.w <= box.x || pos.y + pos.h <= box.y) {
			wlr_log(WLR_INFO, "restore: '%s' saved position is off-screen, "
				"placing normally", app_id);
			return RESTORE_NONE;
		}
	}
	/* the window goes back to its saved workspace, even if the cursor
	 * is on another output */
	if (saved_o->current_workspace != pos.workspace) {
		ws_switch_state(saved_o, pos.workspace);
	}
	toplevel->output = saved_o;
	toplevel->workspace = saved_o->current_workspace;
	if (tiled) {
		/* the tile layout is authoritative: it places the window */
		return RESTORE_TILE;
	}
	wlr_scene_node_set_position(&toplevel->scene_tree->node, pos.x, pos.y);
	if (toplevel->xdg_toplevel != NULL) {
		/* the size must be re-asserted on the first commit: the
		 * initial-commit handler would otherwise send 0,0 (the app's
		 * preferred size) and discard the restored size */
		toplevel->restore_w = pos.w;
		toplevel->restore_h = pos.h;
	} else {
		toplevel_set_size(toplevel, pos.w, pos.h);
	}
	wlr_log(WLR_INFO, "restore: '%s' -> %s ws%d %d,%d %dx%d",
		app_id, pos.output_name, pos.workspace, pos.x, pos.y, pos.w, pos.h);
	return RESTORE_FREE;
}
