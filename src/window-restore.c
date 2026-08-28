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
// State file: app_id|output|box_x|box_y|workspace|x|y|w|h per line, under
// XDG_STATE_HOME (or ~/.local/state). box_x/box_y is the output's position
// in the layout: a monitor that reappears under the same name but at a
// different box is a different physical monitor and must not receive the
// restored window.
// ---------------------------------------------------------------------------

void guibux_state_path(char *path, size_t path_size, const char *file) {
	const char *state_home = getenv("XDG_STATE_HOME");
	if (state_home != NULL && state_home[0] != '\0') {
		snprintf(path, path_size, "%s/guibuxwm/%s", state_home, file);
		return;
	}
	const char *home = getenv("HOME");
	if (home != NULL) {
		snprintf(path, path_size, "%s/.local/state/guibuxwm/%s", home, file);
		return;
	}
	snprintf(path, path_size, "/tmp/guibuxwm-%s", file);
}

void guibux_state_mkdir(const char *path) {
	/* create the directory chain if it does not exist yet: parent
	 * first, then the state dir itself */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash != NULL && slash != dir) {
		*slash = '\0';
		char *slash2 = strrchr(dir, '/');
		if (slash2 != NULL && slash2 != dir) {
			*slash2 = '\0';
			mkdir(dir, 0755);
			*slash2 = '/';
		}
		mkdir(dir, 0755);
	}
}

static void restore_state_path(char *path, size_t path_size) {
	guibux_state_path(path, path_size, "window-positions");
}

static void restore_write_file(struct guibux_server *server) {
	char path[PATH_MAX];
	restore_state_path(path, sizeof(path));
	guibux_state_mkdir(path);
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		wlr_log(WLR_ERROR, "restore: cannot write %s: %m", path);
		return;
	}
	fprintf(f, "# guibuxwm window positions: "
		"app_id|output|box_x|box_y|workspace|x|y|w|h\n");
	for (int i = 0; i < server->window_restore.count; i++) {
		struct guibux_window_restore_entry *e =
			&server->window_restore.entries[i];
		fprintf(f, "%s|%s|%d|%d|%d|%d|%d|%d|%d\n",
			e->app_id, e->pos.output_name, e->pos.box_x, e->pos.box_y,
			e->pos.workspace, e->pos.x, e->pos.y, e->pos.w, e->pos.h);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------
// Terminal detection: the basename of the first token of `term`, or the
// explicit `term_app_id` config key when set (the command name and the
// Wayland app_id differ, e.g. gnome-terminal vs org.gnome.Terminal)
// ---------------------------------------------------------------------------

void restore_derive_terminal_id(struct guibux_server *server) {
	if (server->term_app_id != NULL) {
		server->terminal_app_id = strdup(server->term_app_id);
		wlr_log(WLR_INFO, "restore: terminal app_id '%s' (term_app_id)",
			server->terminal_app_id);
		return;
	}
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

/* app_id prefixes that are always excluded from position restore,
 * in addition to the configured terminal: terminals are short-lived
 * and their position is not meaningful to persist */
static const char *restore_excluded_ids[] = {
	"kgx",
	"org.gnome.Console",
	"org.kde.konsole",
	"gnome-terminal",
	"org.gnome.Terminal",
	"foot",
	"alacritty",
	"org.alacritty.Alacritty",
	"kitty",
	"st",
	"xfce4-terminal",
	"org.xfce.Terminal",
	"terminator",
	"net.tsudor.Terminator",
	"tilix",
	"qterminal",
	"wezterm",
	"org.wezfurlong.wezterm",
	"hyper",
	"co.elastic.hyper",
	"tabby",
	"org.xtig.tabby",
	"guake",
	"mate-terminal",
	"org.mate.Terminal",
	"lxterminal",
	"org.lxde.Lxterminal",
	"eterm",
	"rxvt",
};
#define RESTORE_EXCLUDED_COUNT \
	(sizeof(restore_excluded_ids) / sizeof(restore_excluded_ids[0]))

static bool restore_is_excluded(struct guibux_server *server,
		const char *app_id) {
	if (app_id == NULL || app_id[0] == '\0') {
		return false;
	}
	/* the configured terminal */
	if (server->terminal_app_id != NULL) {
		size_t len = strlen(server->terminal_app_id);
		if (strncasecmp(app_id, server->terminal_app_id, len) == 0) {
			return true;
		}
	}
	/* known terminal app_ids; prefix match so variants like "foot-client"
	 * or "rxvt-unicode" are caught, but the next char must be a separator
	 * so "st" does not match "steam" */
	for (size_t i = 0; i < RESTORE_EXCLUDED_COUNT; i++) {
		size_t len = strlen(restore_excluded_ids[i]);
		if (strncasecmp(app_id, restore_excluded_ids[i], len) == 0) {
			char next = app_id[len];
			if (next == '\0' || next == '-' || next == '.' ||
					next == '_' || next == '+') {
				return true;
			}
		}
	}
	return false;
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
		int box_x = 0, box_y = 0;
		bool box_valid = false;
		int ws, x, y, w, h;
		int n = sscanf(line,
			"%255[^|]|%63[^|]|%d|%d|%d|%d|%d|%d|%d",
			app_id, output_name, &box_x, &box_y, &ws, &x, &y, &w, &h);
		if (n == 9) {
			box_valid = true;
		} else if (n == 7) {
			/* legacy format: app_id|output|ws|x|y|w|h. The 9-field
			 * parse above left shifted values in place: re-parse */
			if (sscanf(line, "%255[^|]|%63[^|]|%d|%d|%d|%d|%d",
					app_id, output_name, &ws, &x, &y, &w, &h) != 7) {
				wlr_log(WLR_ERROR, "restore: bad line '%s'", line);
				continue;
			}
			wlr_log(WLR_INFO, "restore: legacy line for '%s', "
				"monitor identity check disabled", app_id);
		} else {
			wlr_log(WLR_ERROR, "restore: bad line '%s'", line);
			continue;
		}
		if (ws < 1 || ws > NUM_WORKSPACES || w <= 0 || h <= 0) {
			wlr_log(WLR_ERROR, "restore: bad values in '%s'", line);
			continue;
		}
		if (restore_is_excluded(server, app_id)) {
			/* the terminal is excluded: also purges stale entries
			 * saved before the exclusion (or a term_app_id change) */
			wlr_log(WLR_INFO, "restore: skipping terminal entry '%s'",
				app_id);
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
		e->pos.box_x = box_x;
		e->pos.box_y = box_y;
		e->pos.box_valid = box_valid;
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
// Save / apply
// ---------------------------------------------------------------------------

/* update the in-memory entry for one toplevel; false if the window is
 * not eligible (terminal, fullscreen, no output, no geometry, ...) */
static bool restore_update_entry(struct guibux_server *server,
		struct guibux_toplevel *toplevel) {
	const char *app_id = toplevel_app_id(toplevel);
	if (app_id == NULL || app_id[0] == '\0') {
		return false;
	}
	if (restore_is_excluded(server, app_id)) {
		wlr_log(WLR_INFO, "restore: skipping terminal '%s'", app_id);
		return false;
	}
	/* a fullscreen window's geometry is the whole output: keep the
	 * last windowed position instead of remembering the fullscreen one */
	if (toplevel->is_fullscreen) {
		return false;
	}
	if (toplevel->scene_tree == NULL) {
		return false;
	}
	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	if (o == NULL || o->wlr_output->name == NULL) {
		return false;
	}
	struct wlr_box geo;
	toplevel_get_geometry(toplevel, &geo);
	if (geo.width <= 0 || geo.height <= 0) {
		return false;
	}
	int x = (int)toplevel->scene_tree->node.x;
	int y = (int)toplevel->scene_tree->node.y;
	int ws = toplevel->workspace;
	/* remember where this output sits in the layout: at restore time a
	 * monitor with the same name but a different box is a replugged
	 * (different physical) monitor and must not take the window */
	struct wlr_box ob;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &ob);

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
			return false;
		}
		idx = server->window_restore.count++;
	}
	struct guibux_window_restore_entry *e =
		&server->window_restore.entries[idx];
	snprintf(e->app_id, sizeof(e->app_id), "%s", app_id);
	snprintf(e->pos.output_name, sizeof(e->pos.output_name), "%s",
		o->wlr_output->name);
	e->pos.box_x = ob.x;
	e->pos.box_y = ob.y;
	e->pos.box_valid = true;
	e->pos.workspace = ws;
	e->pos.x = x;
	e->pos.y = y;
	e->pos.w = geo.width;
	e->pos.h = geo.height;
	wlr_log(WLR_INFO, "restore: saved '%s' -> %s ws%d %d,%d %dx%d",
		app_id, o->wlr_output->name, ws, x, y, geo.width, geo.height);
	return true;
}

void restore_save(struct guibux_server *server, struct guibux_toplevel *toplevel) {
	if (!server->restore_positions) {
		return;
	}
	if (restore_update_entry(server, toplevel)) {
		restore_write_file(server);
	}
}

/* save every mapped window: called on a clean WM exit, before the
 * clients are destroyed */
void restore_save_all(struct guibux_server *server) {
	if (!server->restore_positions) {
		return;
	}
	int saved = 0;
	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (restore_update_entry(server, t)) {
			saved++;
		}
	}
	if (saved > 0) {
		restore_write_file(server);
		wlr_log(WLR_INFO, "restore: saved %d positions on exit", saved);
	}
}

enum restore_result restore_apply(struct guibux_server *server,
		struct guibux_toplevel *toplevel) {
	if (!server->restore_positions) {
		return RESTORE_NONE;
	}
	const char *app_id = toplevel_app_id(toplevel);
	if (app_id == NULL || app_id[0] == '\0' ||
			restore_is_excluded(server, app_id)) {
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
	/* the name matched but the layout box differs: the monitor was
	 * unplugged and a different one took its name. Never place the
	 * window on a monitor the app was not last seen on */
	if (pos.box_valid) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			saved_o->wlr_output, &box);
		if (box.x != pos.box_x || box.y != pos.box_y) {
			wlr_log(WLR_INFO, "restore: '%s' output '%s' moved "
				"(was %d,%d now %d,%d), placing normally",
				app_id, pos.output_name, pos.box_x, pos.box_y,
				box.x, box.y);
			return RESTORE_NONE;
		}
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
