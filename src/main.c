// guibuxwm - a simple Wayland window manager built on wlroots 0.20
//
// Derived from tinywl (wlroots, MIT license).
//
// Features:
//   - xdg-shell toplevels: focus, move, resize, fullscreen
//   - starts a terminal at launch (GUIBUX_TERM env or -t flag, default: alacritty)
//   - keyboard layout: -k flag, GUIBUX_XKB_LAYOUT or XKB_DEFAULT_LAYOUT env
//     (e.g. -k fr for French)
//   - multi-monitor: new windows open on the output under the cursor,
//     windows move between monitors with Mod+Shift+Left/Right,
//     monitor arrangement via GUIBUX_OUTPUTS="NAME@XxY,NAME@XxY"
//   - keybindings (Mod = Super):
//       Mod+Return            start a new terminal
//       Mod+q                 close focused window
//       Mod+f                 toggle fullscreen
//       Mod+e                 command box: type a command, Enter runs it
//       Mod+Tab               cycle focus
//       Mod+Shift+Left/Right  move window to previous/next monitor
//       Mod+Shift+q           quit
//       Alt+Escape            quit
//
// Build:
//   meson setup build && ninja -C build
// Run:
//   ./build/guibuxwm

#include <assert.h>
#include <cairo.h>
#include <dirent.h>
#include <drm_fourcc.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define CASCADE_STEP 40
#define CASCADE_MAX 6
#define MAX_OUTPUT_PLACEMENTS 8
#define LAUNCHER_MAX_MATCHES 8
#define LAUNCHER_MAX_COMMANDS 4096

struct output_placement {
	char name[64];
	int x, y;
	int transform; // -1 = unset, else enum wl_output_transform
};

enum guibux_cursor_mode {
	GUIBUX_CURSOR_PASSTHROUGH,
	GUIBUX_CURSOR_MOVE,
	GUIBUX_CURSOR_RESIZE,
};

struct guibux_server;
struct guibux_toplevel;

struct guibux_output {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct guibux_toplevel {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	bool is_fullscreen;
	double saved_x, saved_y;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct guibux_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct guibux_keyboard {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct guibux_launcher {
	bool active;
	char text[512];
	int text_len;
	int box_w, box_h, box_scale;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	struct wlr_allocator *shm_alloc;
	FT_Library ft;
	FT_Face face;
	struct wl_event_source *test_timer;
	char **commands;
	int num_commands;
	char *matches[LAUNCHER_MAX_MATCHES];
	int num_matches;
	int selection;
};

struct guibux_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum guibux_cursor_mode cursor_mode;
	struct guibux_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	int cascade;
	char *term_cmd;
	const char *xkb_layout;
	struct output_placement placements[MAX_OUTPUT_PLACEMENTS];
	int num_placements;

	struct guibux_launcher launcher;
};

static void focus_toplevel(struct guibux_toplevel *toplevel) {
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
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void spawn_terminal(struct guibux_server *server) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %m");
		return;
	}
	if (pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", server->term_cmd, (void *)NULL);
		_exit(127);
	}
	wlr_log(WLR_INFO, "spawned terminal (%s) pid %d", server->term_cmd, pid);
}

static struct wlr_output *output_at_cursor(struct guibux_server *server) {
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

// ---------------------------------------------------------------------------
// Command launcher (Mod+E): input box, Enter runs the command via sh -c
// ---------------------------------------------------------------------------

// declared in wlr/render/allocator/shm.h (not installed with our wlroots build)
struct wlr_allocator *wlr_shm_allocator_create(void);

#define LAUNCHER_BOX_W 480
#define LAUNCHER_BOX_H 40
#define LAUNCHER_FONT_PX 20
#define LAUNCHER_LINE_H 28

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

// collect executable names from $PATH (deduplicated, capped). called once at
// startup; on failure the launcher simply has no candidates
static void launcher_load_commands(struct guibux_launcher *l) {
	const char *path = getenv("PATH");
	if (path == NULL) {
		return;
	}
	char *copy = strdup(path);
	if (copy == NULL) {
		return;
	}
	l->commands = calloc(1024, sizeof(char *));
	if (l->commands == NULL) {
		free(copy);
		return;
	}
	int cap = 1024;
	char *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
			dir = strtok_r(NULL, ":", &save)) {
		if (*dir == '\0' || l->num_commands >= LAUNCHER_MAX_COMMANDS) {
			break;
		}
		DIR *d = opendir(dir);
		if (d == NULL) {
			continue;
		}
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			char full[PATH_MAX];
			snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
			struct stat st;
			if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
				continue;
			}
			if (access(full, X_OK) != 0) {
				continue;
			}
			bool dup = false;
			for (int i = 0; i < l->num_commands; i++) {
				if (strcmp(l->commands[i], e->d_name) == 0) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			if (l->num_commands == cap) {
				cap *= 2;
				char **tmp = realloc(l->commands, (size_t)cap * sizeof(char *));
				if (tmp == NULL) {
					break;
				}
				l->commands = tmp;
			}
			l->commands[l->num_commands++] = strdup(e->d_name);
			if (l->commands[l->num_commands - 1] == NULL) {
				l->num_commands--;
				break;
			}
		}
		closedir(d);
		if (l->num_commands >= LAUNCHER_MAX_COMMANDS) {
			break;
		}
	}
	free(copy);
	wlr_log(WLR_INFO, "launcher: %d commands from $PATH", l->num_commands);
}

static void launcher_free_commands(struct guibux_launcher *l) {
	for (int i = 0; i < l->num_commands; i++) {
		free(l->commands[i]);
	}
	free(l->commands);
	l->commands = NULL;
	l->num_commands = 0;
}

// match the first word of the input against the command list
// (case-insensitive); exact matches first, then substring matches.
// results in l->matches, selection reset
static void launcher_filter(struct guibux_launcher *l) {
	l->num_matches = 0;
	l->selection = 0;
	if (l->text[0] == '\0' || l->num_commands == 0) {
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
		for (int i = 0; i < l->num_commands &&
				l->num_matches < LAUNCHER_MAX_MATCHES; i++) {
			bool exact = strcasecmp(l->commands[i], first) == 0;
			bool sub = strcasestr(l->commands[i], first) != NULL;
			if ((pass == 0 && exact) || (pass == 1 && sub && !exact)) {
				l->matches[l->num_matches++] = l->commands[i];
			}
		}
	}
}

// draw text in the given 0xRRGGBB color on an RGB24 cairo surface,
// returns width in px
static int launcher_draw_text(cairo_surface_t *cs, FT_Face face,
		const char *text, int x, int baseline, uint32_t color) {
	uint32_t *data = (uint32_t *)cairo_image_surface_get_data(cs);
	int stride = cairo_image_surface_get_stride(cs) / 4;
	int sw = cairo_image_surface_get_width(cs);
	int sh = cairo_image_surface_get_height(cs);
	int cx = x;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_RENDER) != 0) {
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		for (uint32_t row = 0; row < g->bitmap.rows; row++) {
			int py = baseline - (int)g->bitmap_top + (int)row;
			if (py < 0 || py >= sh) {
				continue;
			}
			for (uint32_t col = 0; col < g->bitmap.width; col++) {
				int px = cx + (int)g->bitmap_left + (int)col;
				if (px < 0 || px >= sw) {
					continue;
				}
				uint8_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
				if (a == 0) {
					continue;
				}
				uint32_t *dst = &data[py * stride + px];
				uint8_t r = (color >> 16) & 0xFF;
				uint8_t gg = (color >> 8) & 0xFF;
				uint8_t b = color & 0xFF;
				uint8_t dr = (*dst >> 16) & 0xFF;
				uint8_t dg = (*dst >> 8) & 0xFF;
				uint8_t db = *dst & 0xFF;
				r = (r * a + dr * (255 - a)) / 255;
				gg = (gg * a + dg * (255 - a)) / 255;
				b = (b * a + db * (255 - a)) / 255;
				*dst = (r << 16) | (gg << 8) | b;
			}
		}
		cx += g->advance.x / 64;
	}
	return cx - x;
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
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
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

	// background
	cairo_set_source_rgb(cr, 0x1e / 255.0, 0x1e / 255.0, 0x2e / 255.0);
	cairo_paint(cr);
	// border
	cairo_set_source_rgb(cr, 0x45 / 255.0, 0x47 / 255.0, 0x5a / 255.0);
	cairo_set_line_width(cr, l->box_scale);
	cairo_rectangle(cr, l->box_scale / 2.0, l->box_scale / 2.0,
		w - l->box_scale, h - l->box_scale);
	cairo_stroke(cr);

	// prompt + text + cursor (input line = top LAUNCHER_BOX_H px)
	int font_px = LAUNCHER_FONT_PX * l->box_scale;
	FT_Set_Pixel_Sizes(l->face, 0, font_px);
	int pad = 12 * l->box_scale;
	int baseline = LAUNCHER_BOX_H / 2 * l->box_scale + font_px * 35 / 100;
	int x = pad;
	x += launcher_draw_text(cs, l->face, "$ ", x, baseline, 0xFFFFFF)
		+ 4 * l->box_scale;
	x += launcher_draw_text(cs, l->face, l->text, x, baseline, 0xFFFFFF);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	cairo_rectangle(cr, x + 2 * l->box_scale, baseline - font_px * 7 / 10,
		font_px / 2, font_px * 8 / 10);
	cairo_fill(cr);

	// candidate list below the input line
	for (int i = 0; i < l->num_matches; i++) {
		int ly = (LAUNCHER_BOX_H + i * LAUNCHER_LINE_H) * l->box_scale;
		int lh = LAUNCHER_LINE_H * l->box_scale;
		if (i == l->selection) {
			cairo_set_source_rgb(cr, 0x3a / 255.0, 0x3c / 255.0, 0x55 / 255.0);
			cairo_rectangle(cr, l->box_scale, ly, w - 2 * l->box_scale, lh);
			cairo_fill(cr);
		}
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (i == l->selection) ? 0xFFFFFF : 0x8888AA;
		launcher_draw_text(cs, l->face, l->matches[i], pad, mb, mc);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(l->buffer);
	// the scene caches the buffer's texture: re-set the buffer so the new
	// pixels are uploaded, and ask the output for a frame
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
	l->box_h = LAUNCHER_BOX_H + LAUNCHER_MAX_MATCHES * LAUNCHER_LINE_H;
	l->box_scale = scale;
	l->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	// shm allocator: gbm/udmabuf buffers do not support data_ptr access
	l->buffer = wlr_allocator_create_buffer(l->shm_alloc,
		bw * scale, l->box_h * scale, &format);
	if (l->buffer == NULL) {
		wlr_log(WLR_ERROR, "launcher: failed to create buffer");
		return;
	}
	l->scene_node = wlr_scene_buffer_create(&server->scene->tree, l->buffer);
	wlr_scene_node_set_position(&l->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - l->box_h) / 2);

	l->text[0] = '\0';
	l->text_len = 0;
	l->num_matches = 0;
	l->selection = 0;
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
		if (l->num_matches > 0 && l->selection < l->num_matches) {
			// selected candidate + args typed after the first word
			const char *space = strchr(l->text, ' ');
			if (space != NULL) {
				snprintf(cmd, sizeof(cmd), "%s%s",
					l->matches[l->selection], space);
			} else {
				snprintf(cmd, sizeof(cmd), "%s", l->matches[l->selection]);
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
	case XKB_KEY_Down:
		if (l->num_matches > 0) {
			l->selection = (l->selection + (sym == XKB_KEY_Down ? 1 :
				l->num_matches - 1)) % l->num_matches;
			launcher_render(server);
		}
		return true;
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
		// Latin-1 keysyms (é, à, ç, ...), function keys excluded
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

static void launcher_init(struct guibux_server *server) {
	struct guibux_launcher *l = &server->launcher;
	l->active = false;
	l->text[0] = '\0';
	l->shm_alloc = wlr_shm_allocator_create();
	if (l->shm_alloc == NULL) {
		wlr_log(WLR_ERROR, "launcher: disabled (no shm allocator)");
		return;
	}
	launcher_load_commands(l);
	if (!launcher_init_font(server)) {
		wlr_log(WLR_ERROR, "launcher: disabled (no font)");
	}
}

// test hook: GUIBUX_TEST_LAUNCHER_CMD="cmd" shows the launcher shortly after
// start, types the command and presses Enter, then shows and Escapes again
static int launcher_test_run(void *data) {
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
			"(num_matches=%d, commands=%d)", l->num_matches, l->num_commands);
		return 0;
	}
	wlr_log(WLR_INFO, "launcher-test: MATCHES OK (%d matches, selected '%s')",
		l->num_matches, l->matches[l->selection]);
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
	return 0;
}

static struct wlr_output *toplevel_output_for(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	struct wlr_output *output = toplevel->xdg_toplevel->requested.fullscreen_output;
	if (output != NULL) {
		return output;
	}
	// output containing the window center (logical size, scale aware)
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	int w = surface->buffer ? surface->current.width : 0;
	int h = surface->buffer ? surface->current.height : 0;
	int cx = toplevel->scene_tree->node.x + w / 2;
	int cy = toplevel->scene_tree->node.y + h / 2;
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

static void set_fullscreen(struct guibux_toplevel *toplevel, bool fullscreen) {
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
			// position the node at the output's layout coordinates
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output, &box);
			wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
			// tell the client to fill the output (logical size,
			// rotation and scale aware)
			int ew, eh;
			wlr_output_effective_resolution(output, &ew, &eh);
			wlr_xdg_toplevel_set_size(xdg_toplevel, ew, eh);
		}
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	} else {
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_x, toplevel->saved_y);
		// let the client pick its size again
		wlr_xdg_toplevel_set_size(xdg_toplevel, 0, 0);
	}

	toplevel->is_fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, fullscreen);
}

static void move_toplevel_to_output(struct guibux_toplevel *toplevel,
		struct wlr_output *output) {
	struct guibux_server *server = toplevel->server;
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false);
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	int32_t w = (surface->buffer && surface->current.width > 0)
		? surface->current.width : 800;
	int32_t h = (surface->buffer && surface->current.height > 0)
		? surface->current.height : 600;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x + (box.width - w) / 2,
		box.y + (box.height - h) / 2);
	wlr_log(WLR_INFO, "moved '%s' to output %s",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)",
		output->name ? output->name : "(unknown)");
}

static void move_toplevel_to_adjacent_output(struct guibux_server *server,
		struct guibux_toplevel *toplevel, int dir) {
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = 0;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (n >= 16) {
			break;
		}
		sorted[n] = o->wlr_output;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &boxes[n]);
		n++;
	}
	if (n < 2) {
		return;
	}
	// insertion sort by layout x
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

static bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym,
		uint32_t modifiers) {
	if ((modifiers & WLR_MODIFIER_ALT) && sym == XKB_KEY_Escape) {
		wl_display_terminate(server->wl_display);
		return true;
	}

	if (!(modifiers & WLR_MODIFIER_LOGO)) {
		return false;
	}

	struct guibux_toplevel *toplevel = wl_list_empty(&server->toplevels) ? NULL :
		wl_container_of(server->toplevels.next, toplevel, link);

	if (modifiers & WLR_MODIFIER_SHIFT) {
		if (sym == XKB_KEY_q) {
			wl_display_terminate(server->wl_display);
			return true;
		}
		if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
			if (toplevel != NULL) {
				move_toplevel_to_adjacent_output(server, toplevel,
					sym == XKB_KEY_Right ? 1 : -1);
			}
			return true;
		}
		return false;
	}

	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		spawn_terminal(server);
		return true;
	case XKB_KEY_q:
		if (toplevel != NULL) {
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		}
		return true;
	case XKB_KEY_f:
		if (toplevel != NULL) {
			set_fullscreen(toplevel, !toplevel->is_fullscreen);
		}
		return true;
	case XKB_KEY_Tab:
		if (toplevel != NULL && wl_list_length(&server->toplevels) > 1) {
			struct guibux_toplevel *next = wl_container_of(
				toplevel->link.next, next, link);
			focus_toplevel(next);
		}
		return true;
	case XKB_KEY_e:
		launcher_show(server);
		return true;
	default:
		return false;
	}
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct guibux_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (server->launcher.active) {
				handled = launcher_handle_key(server, syms[i]);
			} else {
				handled = handle_keybinding(server, syms[i], modifiers);
			}
			if (handled) {
				break;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct guibux_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct guibux_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct guibux_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap;
	if (server->xkb_layout) {
		struct xkb_rule_names rules = {0};
		rules.layout = server->xkb_layout;
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	} else {
		keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (!keymap) {
		wlr_log(WLR_ERROR, "failed to compile keymap for layout '%s'",
			server->xkb_layout ? server->xkb_layout : "default");
		xkb_context_unref(context);
		free(keyboard);
		return;
	}
	wlr_log(WLR_INFO, "keyboard layout: %s",
		server->xkb_layout ? server->xkb_layout : "default");

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct guibux_server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct guibux_server *server = wl_container_of(
		listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct guibux_toplevel *desktop_toplevel_at(
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

static void reset_cursor_mode(struct guibux_server *server) {
	server->cursor_mode = GUIBUX_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void process_cursor_move(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct guibux_server *server) {
	struct guibux_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(struct guibux_server *server, uint32_t time) {
	if (server->cursor_mode == GUIBUX_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == GUIBUX_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!toplevel) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface) {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	if (server->launcher.active &&
			event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		launcher_hide(server);
		return;
	}
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct guibux_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		focus_toplevel(toplevel);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct guibux_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct guibux_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	// find manual placement (GUIBUX_OUTPUTS), if any
	const struct output_placement *placement = NULL;
	for (int i = 0; i < server->num_placements; i++) {
		if (strcmp(wlr_output->name, server->placements[i].name) == 0) {
			placement = &server->placements[i];
			break;
		}
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
		// committed separately so a backend without transform support
		// (e.g. headless) does not break the base commit
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

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	// manual placement from GUIBUX_OUTPUTS, else auto-arrange
	struct wlr_output_layout_output *l_output = NULL;
	if (placement != NULL) {
		l_output = wlr_output_layout_add(server->output_layout, wlr_output,
			placement->x, placement->y);
	}
	if (l_output == NULL) {
		l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
	}
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	if (l_output == NULL) {
		wlr_log(WLR_ERROR, "%s: failed to add output to layout",
			wlr_output->name);
	} else {
		wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
	}
}

static void place_toplevel(struct guibux_toplevel *toplevel) {
	struct guibux_server *server = toplevel->server;
	// place on the output under the cursor
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

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, map);

	wlr_log(WLR_INFO, "mapped toplevel '%s'",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)");

	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

	place_toplevel(toplevel);
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct guibux_server *server = toplevel->server;

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	wl_list_remove(&toplevel->link);

	if (!wl_list_empty(&server->toplevels)) {
		struct guibux_toplevel *next =
			wl_container_of(server->toplevels.next, next, link);
		focus_toplevel(next);
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
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

	free(toplevel);
}

static void begin_interactive(struct guibux_toplevel *toplevel,
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

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false);
	}
	begin_interactive(toplevel, GUIBUX_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	set_fullscreen(toplevel, toplevel->xdg_toplevel->requested.fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
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

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct guibux_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct guibux_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	struct guibux_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	assert(parent != NULL);
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void parse_output_placements(struct guibux_server *server) {
	const char *env = getenv("GUIBUX_OUTPUTS");
	if (env == NULL) {
		return;
	}
	char *copy = strdup(env);
	if (copy == NULL) {
		return;
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
			tok = strtok_r(NULL, ",", &save)) {
		char *at = strchr(tok, '@');
		if (at == NULL) {
			wlr_log(WLR_ERROR, "GUIBUX_OUTPUTS: bad entry '%s' (expected NAME@XxY[:ROT])", tok);
			continue;
		}
		*at = '\0';
		int x, y;
		if (sscanf(at + 1, "%dx%d", &x, &y) != 2) {
			wlr_log(WLR_ERROR, "GUIBUX_OUTPUTS: bad position in '%s'", tok);
			continue;
		}
		int transform = -1;
		char *rot = strchr(at + 1, ':');
		if (rot != NULL) {
			rot[0] = '\0';
			rot++;
			if (!strcmp(rot, "normal")) {
				transform = WL_OUTPUT_TRANSFORM_NORMAL;
			} else if (!strcmp(rot, "90")) {
				transform = WL_OUTPUT_TRANSFORM_90;
			} else if (!strcmp(rot, "180")) {
				transform = WL_OUTPUT_TRANSFORM_180;
			} else if (!strcmp(rot, "270")) {
				transform = WL_OUTPUT_TRANSFORM_270;
			} else {
				wlr_log(WLR_ERROR, "GUIBUX_OUTPUTS: bad rotation '%s' (expected normal|90|180|270)", rot);
				continue;
			}
		}
		if (server->num_placements >= MAX_OUTPUT_PLACEMENTS) {
			wlr_log(WLR_ERROR, "GUIBUX_OUTPUTS: too many outputs (max %d)",
				MAX_OUTPUT_PLACEMENTS);
			break;
		}
		struct output_placement *p = &server->placements[server->num_placements++];
		snprintf(p->name, sizeof(p->name), "%s", tok);
		p->x = x;
		p->y = y;
		p->transform = transform;
		wlr_log(WLR_INFO, "GUIBUX_OUTPUTS: %s at %dx%d transform %d",
			p->name, x, y, transform);
	}
	free(copy);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);

	char *term_cmd = NULL;
	char *xkb_layout = NULL;
	int c;
	while ((c = getopt(argc, argv, "t:k:h")) != -1) {
		switch (c) {
		case 't':
			term_cmd = optarg;
			break;
		case 'k':
			xkb_layout = optarg;
			break;
		default:
			printf("Usage: %s [-t terminal command] [-k keyboard layout]\n", argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-t terminal command] [-k keyboard layout]\n", argv[0]);
		return 1;
	}

	struct guibux_server server = {0};
	const char *env_term = getenv("GUIBUX_TERM");
	const char *default_term = term_cmd ? term_cmd
		: (env_term ? env_term : "alacritty");
	server.term_cmd = strdup(default_term);
	server.xkb_layout = xkb_layout
		? xkb_layout
		: (getenv("GUIBUX_XKB_LAYOUT") ? getenv("GUIBUX_XKB_LAYOUT")
			: getenv("XKB_DEFAULT_LAYOUT"));
	parse_output_placements(&server);

	server.wl_display = wl_display_create();
	// test mode: force the headless backend so extra outputs can be added
	const char *extra_outputs = getenv("GUIBUX_TEST_EXTRA_OUTPUTS");
	if (extra_outputs != NULL) {
		server.backend = wlr_headless_backend_create(
			wl_display_get_event_loop(server.wl_display));
	} else {
		server.backend = wlr_backend_autocreate(
			wl_display_get_event_loop(server.wl_display), NULL);
	}
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	launcher_init(&server);

	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = GUIBUX_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);

	// test hook: the headless backend has no default output, so add N+1
	if (extra_outputs != NULL) {
		int n = atoi(extra_outputs) + 1;
		for (int i = 0; i < n; i++) {
			wlr_headless_add_output(server.backend, 1280, 720);
		}
	}

	// test hook: GUIBUX_TEST_LAUNCHER_CMD drives the launcher headlessly
	const char *launcher_test_cmd = getenv("GUIBUX_TEST_LAUNCHER_CMD");
	if (launcher_test_cmd != NULL) {
		server.launcher.test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			launcher_test_run, &server);
		wl_event_source_timer_update(server.launcher.test_timer, 500);
	}

	spawn_terminal(&server);

	wlr_log(WLR_INFO, "guibuxwm running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);

	wl_list_remove(&server.new_output.link);

	launcher_hide(&server);
	launcher_free_commands(&server.launcher);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_output_layout_destroy(server.output_layout);
	wlr_allocator_destroy(server.allocator);
	if (server.launcher.shm_alloc != NULL) {
		wlr_allocator_destroy(server.launcher.shm_alloc);
	}
	if (server.launcher.ft != NULL) {
		FT_Done_FreeType(server.launcher.ft);
	}
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	free(server.term_cmd);
	return 0;
}
