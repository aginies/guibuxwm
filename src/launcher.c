#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <drm_fourcc.h>

static const char *launcher_font_candidates[] = {
	"/usr/share/fonts/truetype/LiberationMono-Regular.ttf",
	"/usr/share/fonts/truetype/SUSEMono-Regular.otf",
};

static bool launcher_init_font(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	if (FT_Init_FreeType(&l->ft) != 0) {
		wlr_log(WLR_ERROR, "launcher: freetype init failed");
		return false;
	}
	for (size_t i = 0; i < sizeof(launcher_font_candidates) /
			sizeof(launcher_font_candidates[0]); i++) {
		if (FT_New_Face(l->ft, launcher_font_candidates[i], 0, &l->face) == 0) {
			wlr_log(WLR_INFO, "launcher: using font %s",
				launcher_font_candidates[i]);
			return true;
		}
	}
	wlr_log(WLR_ERROR, "launcher: no usable font found");
	FT_Done_FreeType(l->ft);
	l->ft = NULL;
	return false;
}

static void launcher_add_entry(struct guibux_launcher *l, const char *name, const char *exec) {
	bool dup = false;
	for (int i = 0; i < l->num_entries; i++) {
		if (strcasecmp(l->entries[i].name, name) == 0) {
			dup = true;
			break;
		}
	}
	if (dup) return;
	if (l->num_entries >= LAUNCHER_MAX_COMMANDS) return;

	if (l->num_entries % 256 == 0) {
		int newcap = l->num_entries + 256;
		struct launcher_entry *new_entries = realloc(l->entries ? l->entries : 0,
			(size_t)newcap * sizeof(struct launcher_entry));
		if (!new_entries) return;
		l->entries = new_entries;
	}
	l->entries[l->num_entries].name = strdup(name);
	l->entries[l->num_entries].exec = strdup(exec);
	l->num_entries++;
}

static void launcher_load_commands(struct guibux_launcher *l) {
	const char *path = getenv("PATH");
	if (path == NULL) return;
	char *copy = strdup(path);
	if (copy == NULL) return;

	int saved_num = l->num_entries;
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
			dir = strtok_r(NULL, ":", &save)) {
		if (*dir == '\0') continue;
		DIR *d = opendir(dir);
		if (d == NULL) continue;
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
			struct stat st;
			if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
			if (access(full, X_OK) != 0) continue;
			launcher_add_entry(l, e->d_name, e->d_name);
			if (l->num_entries >= LAUNCHER_MAX_COMMANDS) break;
		}
		closedir(d);
		if (l->num_entries >= LAUNCHER_MAX_COMMANDS) break;
	}
	free(copy);
	wlr_log(WLR_INFO, "launcher: %d entries from $PATH", l->num_entries - saved_num);
}

// strip freedesktop field codes from one Exec token; the launcher
// passes no files or URLs, so value codes are dropped and %% -> %
static char *strip_field_codes(const char *tok) {
	char *out = malloc(strlen(tok) + 1);
	int n = 0;
	for (size_t i = 0; tok[i]; i++) {
		if (tok[i] == '%' && tok[i + 1] != '\0') {
			char fc = tok[i + 1];
			if (fc == '%') {
				out[n++] = '%';
				i++;
				continue;
			}
			if (strchr("fFuUicknvNg", fc)) {
				i++;
				continue;
			}
		}
		out[n++] = tok[i];
	}
	out[n] = '\0';
	return out;
}

// convert a desktop Exec line to a shell command: split into tokens
// (quotes respected), strip field codes, drop tokens that become
// empty, rejoin (tokens with spaces get quoted)
static char *desktop_exec_to_cmd(const char *exec) {
	size_t cap = strlen(exec) * 2 + 3;
	char *cmd = malloc(cap);
	cmd[0] = '\0';
	size_t clen = 0;
	bool first = true;
	bool in_squote = false, in_dquote = false;
	char tok[256];
	size_t tlen = 0;
	for (size_t i = 0; ; i++) {
		char c = exec[i];
		bool end = (c == '\0');
		bool sep = !in_squote && !in_dquote &&
			(c == ' ' || c == '\t' || c == '\n' || c == '\r');
		if (!end && !sep) {
			if (c == '\'' && !in_dquote) {
				in_squote = !in_squote;
			} else if (c == '"' && !in_squote) {
				in_dquote = !in_dquote;
			} else if (c == '\\' && !in_squote && exec[i + 1] != '\0') {
				if (tlen < sizeof(tok) - 1) {
					tok[tlen++] = exec[++i];
				}
			} else {
				if (tlen < sizeof(tok) - 1) {
					tok[tlen++] = c;
				}
			}
			continue;
		}
		tok[tlen] = '\0';
		if (tlen > 0) {
			char *clean = strip_field_codes(tok);
			if (clean[0] != '\0') {
				if (!first) {
					clen += snprintf(cmd + clen, cap - clen, " ");
				}
				if (strchr(clean, ' ')) {
					clen += snprintf(cmd + clen, cap - clen, "'%s'", clean);
				} else {
					clen += snprintf(cmd + clen, cap - clen, "%s", clean);
				}
				first = false;
			}
			free(clean);
			tlen = 0;
		}
		in_squote = in_dquote = false;
		if (end) break;
	}
	return cmd;
}

static void launcher_parse_desktop(const char *filepath, struct guibux_launcher *l) {
	FILE *f = fopen(filepath, "r");
	if (!f) return;
	char line[512];
	char *name = NULL, *exec = NULL, *flatpak_id = NULL;
	bool in_main = false;
	while (fgets(line, sizeof(line), f)) {
		if (strcmp(line, "[Desktop Entry]\n") == 0 ||
			strcmp(line, "[DesktopEntry]\n") == 0) {
			in_main = true;
			continue;
		}
		if (in_main && line[0] == '[') break;
		if (!in_main) continue;

		char *val = NULL;
		if (strncmp(line, "Name=", 5) == 0) {
			val = line + 5;
		} else if (strncmp(line, "Exec=", 5) == 0) {
			val = line + 5;
		} else if (strncmp(line, "X-Flatpak=", 10) == 0) {
			val = line + 10;
		} else continue;

		int len = strlen(val);
		while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r'))
			val[--len] = '\0';

		if (val[0] == '\0') continue;
		if (strncmp(line, "Name=", 5) == 0) {
			if (!name) name = strdup(val);
		} else if (strncmp(line, "Exec=", 5) == 0) {
			if (!exec) exec = strdup(val);
		} else {
			if (!flatpak_id) flatpak_id = strdup(val);
		}
		if (name && flatpak_id) break;
	}
	fclose(f);

	if (name && flatpak_id) {
		/* flatpak-exported desktop files: the Exec line carries
		 * flatpak @@...@@ field-code wrappers that do not survive a
		 * plain shell launch; run the app id directly instead */
		size_t n = strlen(flatpak_id);
		char *cmd = malloc(n + 16);
		snprintf(cmd, n + 16, "flatpak run %s", flatpak_id);
		launcher_add_entry(l, name, cmd);
		free(cmd);
	} else if (name && exec) {
		char *cmd = desktop_exec_to_cmd(exec);
		launcher_add_entry(l, name, cmd);
		free(cmd);
	}
	free(name);
	free(exec);
	free(flatpak_id);
}

static void launcher_load_desktop_files(struct guibux_launcher *l) {
	const char *dirs[5] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
		"/var/lib/flatpak/exports/share/applications",
		NULL,
		NULL,
	};
	int dir_count = 3;
	int saved_num = l->num_entries;

	char homedir[PATH_MAX];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(homedir, sizeof(homedir), "%s/.local/share/applications", home);
		dirs[3] = homedir;
		dir_count++;

		char flatpak_home[PATH_MAX];
		snprintf(flatpak_home, sizeof(flatpak_home),
			"%s/.local/share/flatpak/exports/share/applications", home);
		dirs[4] = flatpak_home;
		dir_count++;
	}

	for (int d = 0; d < dir_count; d++) {
		if (!dirs[d]) continue;
		DIR *dir = opendir(dirs[d]);
		if (!dir) continue;
		struct dirent *e;
		while ((e = readdir(dir)) != NULL) {
			size_t len = strlen(e->d_name);
			if (len < 9 || strcmp(e->d_name + len - 8, ".desktop") != 0)
				continue;
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
			launcher_parse_desktop(path, l);
			if (l->num_entries >= LAUNCHER_MAX_COMMANDS) break;
		}
		closedir(dir);
		if (l->num_entries >= LAUNCHER_MAX_COMMANDS) break;
	}
	wlr_log(WLR_INFO, "launcher: %d entries from .desktop", l->num_entries - saved_num);
}

void launcher_free_commands(struct guibux_launcher *l) {
	for (int i = 0; i < l->num_entries; i++) {
		free(l->entries[i].name);
		free(l->entries[i].exec);
	}
	free(l->entries);
	l->entries = NULL;
	l->num_entries = 0;
	for (int i = 0; i < l->num_preferred; i++) {
		free(l->preferred[i].name);
		free(l->preferred[i].exec);
	}
	l->num_preferred = 0;
}

void launcher_filter(struct guibux_launcher *l) {
	l->num_matches = 0;
	l->selection = -1;
	if (l->text[0] == '\0' || l->num_entries == 0) {
		return;
	}
	char first[128];
	int n = 0;
	for (const char *p = l->text;
			*p != '\0' && *p != ' ' && n < (int)sizeof(first) - 1; p++) {
		first[n++] = *p;
	}
	first[n] = '\0';
	if (first[0] == '\0') {
		return;
	}
	for (int pass = 0; pass < 2 &&
			l->num_matches < LAUNCHER_MAX_MATCHES; pass++) {
		for (int i = 0; i < l->num_entries &&
				l->num_matches < LAUNCHER_MAX_MATCHES; i++) {
			bool exact = strcasecmp(l->entries[i].name, first) == 0;
			bool sub = strcasestr(l->entries[i].name, first) != NULL;
			if ((pass == 0 && exact) || (pass == 1 && sub && !exact)) {
				l->matches[l->num_matches++] = i;
			}
		}
	}
	if (l->num_matches > 0) {
		l->selection = l->num_preferred;
	}
}

static void launcher_render(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	if (l->buffer == NULL || l->face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(l->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "launcher: cannot access buffer data");
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "launcher: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(l->buffer);
		return;
	}

	int w = l->box_w * l->box_scale;
	int h = l->box_h * l->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, l->box_scale);
	cairo_rectangle(cr, l->box_scale / 2.0, l->box_scale / 2.0,
		w - l->box_scale, h - l->box_scale);
	cairo_stroke(cr);

	int font_px = LAUNCHER_FONT_PX * l->box_scale;
	FT_Set_Pixel_Sizes(l->face, 0, font_px);
	int pad = 12 * l->box_scale;
	int np = l->num_preferred;

	for (int i = 0; i < np; i++) {
		int ly = i * LAUNCHER_LINE_H * l->box_scale;
		int lh = LAUNCHER_LINE_H * l->box_scale;
		if (i == l->selection) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, l->box_scale, ly, w - 2 * l->box_scale, lh);
			cairo_fill(cr);
		}
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (i == l->selection) ? server->color_text :
			server->color_dim;
		launcher_draw_text_on_surface(cs, l->face, l->preferred[i].name, pad, mb, mc);
	}

	if (np > 0) {
		set_color(cr, server->color_border);
		cairo_set_line_width(cr, l->box_scale);
		double sy = np * LAUNCHER_LINE_H * l->box_scale;
		cairo_move_to(cr, l->box_scale, sy);
		cairo_line_to(cr, w - l->box_scale, sy);
		cairo_stroke(cr);
	}

	int prompt_y = np * LAUNCHER_LINE_H * l->box_scale;
	int baseline = prompt_y + LAUNCHER_BOX_H / 2 * l->box_scale + font_px * 35 / 100;
	int x = pad;
	x += launcher_draw_text_on_surface(cs, l->face, "$ ", x, baseline, server->color_text)
		+ 4 * l->box_scale;
	x += launcher_draw_text_on_surface(cs, l->face, l->text, x, baseline, server->color_text);
	set_color(cr, server->color_text);
	cairo_rectangle(cr, x + 2 * l->box_scale, baseline - font_px * 7 / 10,
		font_px / 2, font_px * 8 / 10);
	cairo_fill(cr);

	for (int i = 0; i < l->num_matches; i++) {
		int ly = (LAUNCHER_BOX_H + (np + i) * LAUNCHER_LINE_H) * l->box_scale;
		int lh = LAUNCHER_LINE_H * l->box_scale;
		if (np + i == l->selection) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, l->box_scale, ly, w - 2 * l->box_scale, lh);
			cairo_fill(cr);
		}
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (np + i == l->selection) ? server->color_text :
			server->color_dim;
		launcher_draw_text_on_surface(cs, l->face, l->entries[l->matches[i]].name, pad, mb, mc);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(l->buffer);
	if (l->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(l->scene_node, l->buffer);
	}
	if (l->output != NULL) {
		wlr_output_schedule_frame(l->output);
	}
}

static void launcher_spawn(struct guibux_server *server, const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "launcher: fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
	spawn_track(pid);
	wlr_log(WLR_INFO, "launcher: running '%s' (pid %d)", cmd, pid);
}

void launcher_show(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	if (l->active || l->face == NULL) {
		return;
	}

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = output->scale > 1 ? (int)output->scale : 1;

	int bw = LAUNCHER_BOX_W;
	if (bw > ew - 20) {
		bw = ew - 20;
	}
	l->box_w = bw;
	l->box_h = LAUNCHER_BOX_H + (LAUNCHER_MAX_MATCHES + l->num_preferred) * LAUNCHER_LINE_H;
	l->box_scale = scale;
	l->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	l->buffer = wlr_allocator_create_buffer(l->shm_alloc,
		bw * scale, l->box_h * scale, &format);
	if (l->buffer == NULL) {
		wlr_log(WLR_ERROR, "launcher: failed to create buffer");
		return;
	}
	l->scene_node = wlr_scene_buffer_create(&server->scene->tree, l->buffer);
	wlr_scene_buffer_set_dest_size(l->scene_node, bw, l->box_h);
	wlr_scene_node_set_position(&l->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - l->box_h) / 2);

	l->text[0] = '\0';
	l->text_len = 0;
	l->num_matches = 0;
	l->selection = -1;
	l->active = true;
	wlr_log(WLR_INFO, "launcher: shown");
	launcher_render(server);
}

void launcher_hide(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	if (!l->active) {
		return;
	}
	l->active = false;
	l->output = NULL;
	if (l->scene_node != NULL) {
		wlr_scene_node_destroy(&l->scene_node->node);
		l->scene_node = NULL;
	}
	if (l->buffer != NULL) {
		wlr_buffer_drop(l->buffer);
		l->buffer = NULL;
	}
}

bool launcher_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_launcher *l = &server->launcher;
	if (!l->active) {
		return false;
	}

	switch (sym) {
	case XKB_KEY_Escape:
		launcher_hide(server);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter: {
		char cmd[512];
		const char *exec = NULL;
		if (l->selection >= 0 && l->selection < l->num_preferred) {
			exec = l->preferred[l->selection].exec;
		} else if (l->selection >= l->num_preferred &&
				l->selection - l->num_preferred < l->num_matches) {
			exec = l->entries[l->matches[l->selection - l->num_preferred]].exec;
		}
		if (exec != NULL) {
			const char *space = strchr(l->text, ' ');
			if (space != NULL) {
				snprintf(cmd, sizeof(cmd), "%s%s", exec, space);
			} else {
				snprintf(cmd, sizeof(cmd), "%s", exec);
			}
		} else {
			snprintf(cmd, sizeof(cmd), "%s", l->text);
		}
		if (cmd[0] != '\0') {
			launcher_spawn(server, cmd);
		}
		launcher_hide(server);
		return true;
	}
	case XKB_KEY_Up:
	case XKB_KEY_Down: {
		int total = l->num_preferred + l->num_matches;
		if (total > 0) {
			if (l->selection < 0) {
				l->selection = (sym == XKB_KEY_Up) ? total - 1 : 0;
			} else {
				l->selection = (l->selection + (sym == XKB_KEY_Down ? 1 :
					total - 1)) % total;
			}
			launcher_render(server);
		}
		return true;
	}
	case XKB_KEY_BackSpace:
		if (l->text_len > 0) {
			l->text[--l->text_len] = '\0';
			launcher_filter(l);
			launcher_render(server);
		}
		return true;
	default:
		break;
	}

	uint32_t c = 0;
	if (sym >= 0x20 && sym <= 0x7e) {
		c = sym;
	} else if ((sym & 0xFF000000) == 0x01000000) {
		uint32_t low = sym & 0xFF;
		if (low >= 0x20 && low < 0xA0) {
			c = low;
		}
	}
	if (c != 0 && l->text_len < (int)sizeof(l->text) - 1) {
		l->text[l->text_len++] = (char)c;
		l->text[l->text_len] = '\0';
		launcher_filter(l);
		launcher_render(server);
	}
	return true;
}

void launcher_init(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	l->active = false;
	l->text[0] = '\0';
	l->shm_alloc = wlr_shm_allocator_create();
	if (l->shm_alloc == NULL) {
		wlr_log(WLR_ERROR, "launcher: disabled (no shm allocator)");
		return;
	}
	launcher_load_desktop_files(l);
	launcher_load_commands(l);
	if (!launcher_init_font(server)) {
		wlr_log(WLR_ERROR, "launcher: disabled (no font)");
	}
}

int launcher_test_run(void *data) {
	struct guibux_server *server = data;
	struct guibux_launcher *l = &server->launcher;
	const char *cmd = getenv("GUIBUX_TEST_LAUNCHER_CMD");

	launcher_show(server);
	if (!l->active) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL show (active=%d)", l->active);
		return 0;
	}
	for (const char *p = cmd; *p; p++) {
		launcher_handle_key(server, (xkb_keysym_t)(uint8_t)*p);
	}
	if (strcmp(l->text, cmd) != 0) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL typing (got '%s')", l->text);
		return 0;
	}
	if (l->num_matches < 1) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL matches "
			"(num_matches=%d, entries=%d)", l->num_matches, l->num_entries);
		return 0;
	}
	int sel_idx = l->selection - l->num_preferred;
	if (sel_idx < 0 || sel_idx >= l->num_matches) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL selection out of range "
			"(selection=%d, preferred=%d, matches=%d)",
			l->selection, l->num_preferred, l->num_matches);
		return 0;
	}
	wlr_log(WLR_INFO, "launcher-test: MATCHES OK (%d matches, selected '%s')",
		l->num_matches,
		l->entries[l->matches[sel_idx]].name);
	wlr_log(WLR_INFO, "launcher-test: EXEC '%s'",
		l->entries[l->matches[sel_idx]].exec);
	launcher_handle_key(server, XKB_KEY_Return);
	if (l->active) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL enter (still active)");
		return 0;
	}
	wlr_log(WLR_INFO, "launcher-test: ENTER OK (ran '%s')", l->text);

	launcher_show(server);
	launcher_handle_key(server, XKB_KEY_BackSpace);
	launcher_handle_key(server, XKB_KEY_Escape);
	if (l->active) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL escape (still active)");
		return 0;
	}
	wlr_log(WLR_INFO, "launcher-test: ESCAPE OK");

	if (l->num_preferred > 0) {
		/* no Enter here: the configured command is a real app,
		 * only verify Up selects the preferred app closest to the prompt */
		launcher_show(server);
		if (!l->active) {
			wlr_log(WLR_ERROR, "launcher-test: FAIL preferred show");
			return 0;
		}
		launcher_handle_key(server, XKB_KEY_Up);
		if (l->selection != l->num_preferred - 1) {
			wlr_log(WLR_ERROR, "launcher-test: FAIL preferred up (selection=%d, expected %d)",
				l->selection, l->num_preferred - 1);
			return 0;
		}
		wlr_log(WLR_INFO, "launcher-test: PREFERRED UP OK (selected '%s')",
			l->preferred[l->selection].name);
		launcher_handle_key(server, XKB_KEY_Escape);
		if (l->active) {
			wlr_log(WLR_ERROR, "launcher-test: FAIL preferred escape (still active)");
			return 0;
		}
	}
	return 0;
}
