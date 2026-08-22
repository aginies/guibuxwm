// guibuxwm - a simple Wayland window manager built on wlroots 0.20
//
// Derived from tinywl (wlroots, MIT license).
//
// Features:
//   - xdg-shell toplevels: focus, move, resize, fullscreen
//   - starts a terminal at launch (config `term`, GUIBUX_TERM env or -t
//     flag, default: gnome-terminal)
//   - keyboard layout: -k flag, config `xkb_layout`, GUIBUX_XKB_LAYOUT or
//     XKB_DEFAULT_LAYOUT env (e.g. -k fr); variant and options via config
//   - multi-monitor: new windows open on the output under the cursor,
//     windows move between monitors with Mod+Shift+Left/Right,
//     monitor arrangement via GUIBUX_OUTPUTS="NAME@XxY,NAME@XxY"
//   - topbar per monitor: monitor letter (A, B, C, ...) on the left, date
//     and time on the right (updates every second)
//   - workspaces per monitor (4, numbered 1 2 3 4): Mod+1..4 switch,
//     Mod+Shift+1..4 move a window; workspace numbers shown in the topbar
//     (current highlighted, clickable)
//   - keybindings (Mod = Super), all configurable via the config file:
//       Mod+Return            start a new terminal
//       Mod+q                 close focused window
//       Mod+f                 toggle fullscreen
//       Mod+t                 cycle tile mode (free / split / main+stack)
//       Mod+e                 command box: type a command, Enter runs it
//       Mod+Tab               cycle focus
//       Mod+1..4              switch to workspace 1..4 on the focused monitor
//       Mod+Shift+1..4        move focused window to workspace 1..4
//       Mod+Shift+Left/Right  move window to previous/next monitor
//       Mod+Shift+q           quit
//       Alt+Escape            quit
//       Mod+Alt+Escape        quit
//   - config file: -c flag, GUIBUX_CONFIG env or ~/.config/guibuxwm/config
//     (keybinds, terminal, keyboard layout/variant/options, colors)
//
// Build:
//   meson setup build && ninja -C build
// Run:
//   ./build/guibuxwm

#include <assert.h>
#include <cairo.h>
#include <dirent.h>
#include <errno.h>
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
#define TOPBAR_H 24
#define TOPBAR_FONT_PX 14
#define TOPBAR_PAD 8
#define NUM_WORKSPACES 4
#define NUM_KEYBINDS 64
#define DEFAULT_COLOR_BG 0x1e1e2e
#define DEFAULT_COLOR_BORDER 0x45475a
#define DEFAULT_COLOR_HIGHLIGHT 0x3a3c55
#define DEFAULT_COLOR_TEXT 0xffffff
#define DEFAULT_COLOR_DIM 0x8888aa
#define DEFAULT_COLOR_TOPBAR_BG 0x73ba25
#define DEFAULT_COLOR_TOPBAR_TEXT 0x1e1e2e

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

enum guibux_tile_mode {
	GUIBUX_TILE_FREE,
	GUIBUX_TILE_SPLIT,
	GUIBUX_TILE_MAIN_STACK,
};

enum guibux_action {
	GUIBUX_ACT_TERMINAL,
	GUIBUX_ACT_CLOSE,
	GUIBUX_ACT_FULLSCREEN,
	GUIBUX_ACT_TILE,
	GUIBUX_ACT_LAUNCHER,
	GUIBUX_ACT_FOCUS_NEXT,
	GUIBUX_ACT_QUIT,
	GUIBUX_ACT_SWITCH_WS,
	GUIBUX_ACT_MOVE_WS,
	GUIBUX_ACT_MOVE_MON_LEFT,
	GUIBUX_ACT_MOVE_MON_RIGHT,
};

struct guibux_keybind {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	enum guibux_action action;
	int arg;
};

struct guibux_server;
struct guibux_toplevel;

struct guibux_output {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_output *wlr_output;
	int tile_mode;
	int current_workspace; // 1..NUM_WORKSPACES
	struct wlr_scene_buffer *topbar_node;
	struct wlr_buffer *topbar_buffer;
	int topbar_number;
	char topbar_right[64];
	int topbar_ws_x[NUM_WORKSPACES + 1]; // cell x, logical px (set by render)
	int topbar_ws_cell_w; // cell width, logical px (set by render)
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
	int workspace; // 1..NUM_WORKSPACES
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
	char *xkb_layout;
	char *xkb_variant;
	char *xkb_options;
	struct guibux_keybind keybinds[NUM_KEYBINDS];
	int num_keybinds;
	uint32_t color_bg, color_border, color_highlight, color_text, color_dim;
	uint32_t color_topbar_bg, color_topbar_text;
	struct output_placement placements[MAX_OUTPUT_PLACEMENTS];
	int num_placements;

	struct wl_event_source *tile_test_timer;
	struct wl_event_source *topbar_timer;
	struct wl_event_source *topbar_test_timer;
	struct wl_event_source *workspace_test_timer;
	struct wl_event_source *keybind_test_timer;
	struct guibux_launcher launcher;
};

static void topbar_raise_all(struct guibux_server *server);
static bool toplevel_visible(struct guibux_toplevel *toplevel);

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
	topbar_raise_all(server);
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

static void set_color(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0,
		((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
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
	set_color(cr, server->color_bg);
	cairo_paint(cr);
	// border
	set_color(cr, server->color_border);
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
	x += launcher_draw_text(cs, l->face, "$ ", x, baseline, server->color_text)
		+ 4 * l->box_scale;
	x += launcher_draw_text(cs, l->face, l->text, x, baseline, server->color_text);
	set_color(cr, server->color_text);
	cairo_rectangle(cr, x + 2 * l->box_scale, baseline - font_px * 7 / 10,
		font_px / 2, font_px * 8 / 10);
	cairo_fill(cr);

	// candidate list below the input line
	for (int i = 0; i < l->num_matches; i++) {
		int ly = (LAUNCHER_BOX_H + i * LAUNCHER_LINE_H) * l->box_scale;
		int lh = LAUNCHER_LINE_H * l->box_scale;
		if (i == l->selection) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, l->box_scale, ly, w - 2 * l->box_scale, lh);
			cairo_fill(cr);
		}
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (i == l->selection) ? server->color_text :
			server->color_dim;
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

static struct guibux_output *guibux_output_for(struct guibux_server *server,
		struct wlr_output *wlr_output) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_output) {
			return o;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Topbar: per-output bar at the top of each monitor, monitor letter on the
// left, date and time on the right. reuses the launcher's shm allocator,
// freetype face and text renderer
// ---------------------------------------------------------------------------

// width in px of text at the face's current pixel size
static int guibux_text_width(FT_Face face, const char *text) {
	int w = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_RENDER) != 0) {
			continue;
		}
		w += face->glyph->advance.x / 64;
	}
	return w;
}

// outputs sorted by layout x (same order as Mod+Shift+Left/Right moves)
static int outputs_sorted_by_x(struct guibux_server *server,
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

static void topbar_render(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (o->topbar_buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(o->topbar_buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "topbar: cannot access buffer data");
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_log(WLR_ERROR, "topbar: unexpected buffer format 0x%x", format);
		wlr_buffer_end_data_ptr_access(o->topbar_buffer);
		return;
	}

	int ew, eh;
	wlr_output_effective_resolution(o->wlr_output, &ew, &eh);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;
	int w = ew * scale;
	int h = TOPBAR_H * scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, h, (int)stride);
	cairo_t *cr = cairo_create(cs);

	// background + bottom border
	set_color(cr, server->color_topbar_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_rectangle(cr, 0, h - scale, w, scale);
	cairo_fill(cr);

	int font_px = TOPBAR_FONT_PX * scale;
	FT_Set_Pixel_Sizes(server->launcher.face, 0, font_px);
	int baseline = TOPBAR_H / 2 * scale + font_px * 35 / 100;

	// monitor letter on the left (A, B, C, ...)
	char left[16];
	snprintf(left, sizeof(left), "%c", 'A' + (o->topbar_number - 1));
	launcher_draw_text(cs, server->launcher.face, left,
		TOPBAR_PAD * scale, baseline, server->color_topbar_text);

	// workspace cells after the monitor letter, current one highlighted.
	// workspaces are numbered 1 2 3 4. cell layout is stored in logical px
	// for click hit-testing
	int cell_w = guibux_text_width(server->launcher.face, "9") / scale + 8;
	o->topbar_ws_cell_w = cell_w;
	int x = TOPBAR_PAD + guibux_text_width(server->launcher.face, left) / scale + 12;
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		o->topbar_ws_x[ws] = x;
		char num[8];
		snprintf(num, sizeof(num), "%d", ws);
		if (ws == o->current_workspace) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, x * scale, (TOPBAR_H / 4) * scale,
				cell_w * scale, (TOPBAR_H / 2) * scale);
			cairo_fill(cr);
			launcher_draw_text(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_text);
		} else {
			launcher_draw_text(cs, server->launcher.face, num,
				(x + 4) * scale, baseline, server->color_topbar_text);
		}
		x += cell_w;
	}

	// date and time on the right
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(o->topbar_right, sizeof(o->topbar_right),
		"%a %d %b %Y  %H:%M", &tm);
	launcher_draw_text(cs, server->launcher.face, o->topbar_right,
		w - TOPBAR_PAD * scale -
			guibux_text_width(server->launcher.face, o->topbar_right),
		baseline, server->color_topbar_text);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(o->topbar_buffer);
	// the scene caches the buffer's texture: re-set the buffer so the new
	// pixels are uploaded, and ask the output for a frame
	if (o->topbar_node != NULL) {
		wlr_scene_buffer_set_buffer(o->topbar_node, o->topbar_buffer);
	}
	wlr_output_schedule_frame(o->wlr_output);
}

// bar under a global point, if any; *ws = workspace cell hit (0 = none)
static bool topbar_workspace_at(struct guibux_server *server, double lx,
		double ly, struct guibux_output **output, int *ws) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_buffer == NULL) {
			continue;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		if (lx < box.x || lx >= box.x + box.width ||
				ly < box.y || ly >= box.y + TOPBAR_H) {
			continue;
		}
		if (output != NULL) {
			*output = o;
		}
		if (ws != NULL) {
			*ws = 0;
			double rel = lx - box.x;
			for (int i = 1; i <= NUM_WORKSPACES; i++) {
				if (rel >= o->topbar_ws_x[i] &&
						rel < o->topbar_ws_x[i] + o->topbar_ws_cell_w) {
					*ws = i;
					break;
				}
			}
		}
		return true;
	}
	return false;
}

static void topbar_create(struct guibux_output *o) {
	struct guibux_server *server = o->server;
	if (server->launcher.shm_alloc == NULL || server->launcher.face == NULL) {
		wlr_log(WLR_ERROR, "topbar: disabled on %s (no allocator or font)",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	int ew, eh;
	wlr_output_effective_resolution(o->wlr_output, &ew, &eh);
	int scale = o->wlr_output->scale > 1 ? (int)o->wlr_output->scale : 1;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	o->topbar_buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		ew * scale, TOPBAR_H * scale, &format);
	if (o->topbar_buffer == NULL) {
		wlr_log(WLR_ERROR, "topbar: failed to create buffer on %s",
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
		return;
	}
	o->topbar_node = wlr_scene_buffer_create(&server->scene->tree, o->topbar_buffer);
	wlr_scene_node_set_position(&o->topbar_node->node, box.x, box.y);
	topbar_render(o);
}

static void topbar_destroy(struct guibux_output *o) {
	if (o->topbar_node != NULL) {
		wlr_scene_node_destroy(&o->topbar_node->node);
		o->topbar_node = NULL;
	}
	if (o->topbar_buffer != NULL) {
		wlr_buffer_drop(o->topbar_buffer);
		o->topbar_buffer = NULL;
	}
}

// number the outputs 1..n by layout x and re-render the bars
static void topbar_renumber(struct guibux_server *server) {
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o != NULL && o->topbar_buffer != NULL) {
			o->topbar_number = i + 1;
			topbar_render(o);
		}
	}
}

// toplevels are raised to top on focus/fullscreen: keep the bars above them
static void topbar_raise_all(struct guibux_server *server) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->topbar_node != NULL) {
			wlr_scene_node_raise_to_top(&o->topbar_node->node);
		}
	}
}

// 1s tick: refresh the clock on every bar
static int topbar_tick(void *data) {
	struct guibux_server *server = data;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		topbar_render(o);
	}
	wl_event_source_timer_update(server->topbar_timer, 1000);
	return 0;
}

// test hook: GUIBUX_TEST_TOPBAR=1 verifies the topbars shortly after start
static int topbar_test_run(void *data) {
	struct guibux_server *server = data;
	struct wlr_output *sorted[16];
	struct wlr_box boxes[16];
	int n = outputs_sorted_by_x(server, sorted, boxes, 16);
	for (int i = 0; i < n; i++) {
		struct guibux_output *o = guibux_output_for(server, sorted[i]);
		if (o == NULL || o->topbar_buffer == NULL) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL no buffer on output %d", i + 1);
			return 0;
		}
		if (o->topbar_number != i + 1) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL number (got %d, want %d)",
				o->topbar_number, i + 1);
			return 0;
		}
		if (strlen(o->topbar_right) < 10) {
			wlr_log(WLR_ERROR, "topbar-test: FAIL time string '%s'",
				o->topbar_right);
			return 0;
		}
	}
	wlr_log(WLR_INFO, "topbar-test: OK (%d outputs)", n);
	return 0;
}

// re-layout all non-fullscreen windows of an output according to its tile
// mode. window order = focus order (focused window first)
static void retile_output(struct guibux_output *output) {
	struct guibux_server *server = output->server;
	if (output->tile_mode == GUIBUX_TILE_FREE) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	// windows tile below the topbar
	box.y += TOPBAR_H;
	box.height -= TOPBAR_H;
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
			// two 50% columns, filled round-robin, stacked within a column
			int col = i % 2;
			int row = i / 2;
			int per_col = (col == 0) ? (n + 1) / 2 : n / 2;
			rx = (box.width / 2) * col;
			rw = (col == 0) ? box.width / 2 : box.width - box.width / 2;
			ry = (box.height * row) / per_col;
			rh = (box.height * (row + 1)) / per_col - ry;
		} else { // GUIBUX_TILE_MAIN_STACK
			// focused window: left 50%; the rest: right 50% stacked
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
// Workspaces: each output has NUM_WORKSPACES workspaces, one current.
// windows not on the current workspace are hidden (scene node disabled:
// not rendered, not hit-testable)
// ---------------------------------------------------------------------------

static bool toplevel_visible(struct guibux_toplevel *toplevel) {
	struct guibux_output *o = guibux_output_for(toplevel->server,
		toplevel_output_for(toplevel));
	return o != NULL && toplevel->workspace == o->current_workspace;
}

static void clear_keyboard_focus(struct guibux_server *server) {
	// notify_clear_focus is the grab-compatible way to drop focus
	// (notify_enter with a NULL surface is prohibited)
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

// a popup (menu, dropdown) may hold seat grabs: end them so a hidden
// workspace does not keep grabbing an invisible popup
static void end_seat_grabs(struct guibux_server *server) {
	if (wlr_seat_keyboard_has_grab(server->seat)) {
		wlr_seat_keyboard_end_grab(server->seat);
	}
	if (wlr_seat_pointer_has_grab(server->seat)) {
		wlr_seat_pointer_end_grab(server->seat);
	}
}

static void switch_workspace(struct guibux_output *output, int ws) {
	if (ws < 1 || ws > NUM_WORKSPACES || ws == output->current_workspace) {
		return;
	}
	struct guibux_server *server = output->server;
	output->current_workspace = ws;

	end_seat_grabs(server);

	struct guibux_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (toplevel_output_for(t) == output->wlr_output) {
			wlr_scene_node_set_enabled(&t->scene_tree->node,
				t->workspace == ws);
		}
	}
	retile_output(output);

	// the focused window may have just been hidden: focus a visible
	// window, or clear keyboard focus if none is visible
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
	topbar_render(output);
}

static void move_toplevel_to_workspace(struct guibux_toplevel *toplevel,
		int ws) {
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
		topbar_render(o);
	}
}

// test helper: the headless backend has no input devices, but the seat
// focus paths (enter/clear) need a keyboard to be exercised; must run
// before any window maps
static void test_seat_add_keyboard(struct guibux_server *server) {
	if (wlr_seat_get_keyboard(server->seat) != NULL) {
		return;
	}
	struct wlr_keyboard *kb = calloc(1, sizeof(*kb));
	if (kb == NULL) {
		return;
	}
	wl_signal_init(&kb->base.events.destroy);
	wl_signal_init(&kb->events.key);
	wl_signal_init(&kb->events.modifiers);
	wl_signal_init(&kb->events.keymap);
	wl_signal_init(&kb->events.repeat_info);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (map != NULL && wlr_keyboard_set_keymap(kb, map)) {
		wlr_seat_set_keyboard(server->seat, kb);
	}
	if (map != NULL) {
		xkb_keymap_unref(map);
	}
	xkb_context_unref(ctx);
}

// test hook: GUIBUX_TEST_WORKSPACES=N exercises the workspace state machine
// shortly after start (late enough for a test client to map windows)
static int workspace_test_run(void *data) {
	struct guibux_server *server = data;
	int ws = atoi(getenv("GUIBUX_TEST_WORKSPACES"));
	if (ws < 1 || ws > NUM_WORKSPACES) {
		ws = 2;
	}
	struct guibux_toplevel *t;
	int n_outputs = 0, n_toplevels = 0;

	wl_list_for_each(t, &server->toplevels, link) {
		n_toplevels++;
	}
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		n_outputs++;
		if (o->current_workspace != 1) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL initial workspace "
				"on %s (got %d, want 1)",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)",
				o->current_workspace);
			return 0;
		}
	}
	if (n_outputs == 0) {
		wlr_log(WLR_ERROR, "workspace-test: FAIL no outputs");
		return 0;
	}
	// all toplevels start visible (workspace 1 == current)
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel hidden "
				"before any switch");
			return 0;
		}
	}

	// switch every output to ws. Windows on not-yet-switched outputs stay
	// visible (per-monitor isolation); once the last one is switched, all
	// windows are hidden and keyboard focus must be cleared.
	wl_list_for_each(o, &server->outputs, link) {
		switch_workspace(o, ws);
		if (o->current_workspace != ws) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL switch on %s "
				"(got %d, want %d)",
				o->wlr_output->name ? o->wlr_output->name : "(unknown)",
				o->current_workspace, ws);
			return 0;
		}
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel visible "
				"on non-current workspace");
			return 0;
		}
	}

	// move a toplevel to ws (becomes visible) and back to 1 (hides)
	struct guibux_toplevel *mover = NULL;
	wl_list_for_each(t, &server->toplevels, link) {
		mover = t;
		break;
	}
	if (mover != NULL) {
		move_toplevel_to_workspace(mover, ws);
		if (mover->workspace != ws || !mover->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL move to current ws "
				"(workspace=%d, enabled=%d, want %d/1)",
				mover->workspace, mover->scene_tree->node.enabled, ws);
			return 0;
		}
		move_toplevel_to_workspace(mover, 1);
		if (mover->workspace != 1 || mover->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL move away "
				"(workspace=%d, enabled=%d, want 1/0)",
				mover->workspace, mover->scene_tree->node.enabled);
			return 0;
		}
	}

	// switch everything back: all toplevels visible again
	wl_list_for_each(o, &server->outputs, link) {
		switch_workspace(o, 1);
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->scene_tree->node.enabled) {
			wlr_log(WLR_ERROR, "workspace-test: FAIL toplevel still "
				"hidden after switch back");
			return 0;
		}
	}
	wlr_log(WLR_INFO, "workspace-test: OK (%d outputs, ws %d, %d toplevels)",
		n_outputs, ws, n_toplevels);
	return 0;
}

// test hook: GUIBUX_TEST_TILE_MODE=N sets the tile mode of all outputs
// shortly after start (0=free, 1=split, 2=main+stack)
static int tile_test_run(void *data) {
	struct guibux_server *server = data;
	int mode = atoi(getenv("GUIBUX_TEST_TILE_MODE"));
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		o->tile_mode = mode;
		retile_output(o);
		wlr_log(WLR_INFO, "tile-test: mode %d on %s", mode,
			o->wlr_output->name ? o->wlr_output->name : "(unknown)");
	}
	return 0;
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
		topbar_raise_all(server);
	} else {
		wlr_scene_node_set_position(&toplevel->scene_tree->node,
			toplevel->saved_x, toplevel->saved_y);
		// let the client pick its size again
		wlr_xdg_toplevel_set_size(xdg_toplevel, 0, 0);
	}

	toplevel->is_fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(xdg_toplevel, fullscreen);
	if (!fullscreen) {
		// window returns to its tile slot, if any
		struct guibux_output *o = guibux_output_for(server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
			retile_output(o);
		}
	}
}

static void move_toplevel_to_output(struct guibux_toplevel *toplevel,
		struct wlr_output *output) {
	struct guibux_server *server = toplevel->server;
	if (toplevel->is_fullscreen) {
		set_fullscreen(toplevel, false);
	}
	struct wlr_output *src = toplevel_output_for(toplevel);
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
	struct guibux_output *o;
	// the window joins the target monitor's current workspace
	if ((o = guibux_output_for(server, output)) != NULL) {
		toplevel->workspace = o->current_workspace;
	}
	if ((o = guibux_output_for(server, src)) != NULL &&
			o->tile_mode != GUIBUX_TILE_FREE) {
		retile_output(o);
	}
	if ((o = guibux_output_for(server, output)) != NULL &&
			o->tile_mode != GUIBUX_TILE_FREE) {
		retile_output(o);
	}
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

static void do_action(struct guibux_server *server, enum guibux_action action,
		int arg, struct guibux_toplevel *toplevel) {
	switch (action) {
	case GUIBUX_ACT_TERMINAL:
		spawn_terminal(server);
		break;
	case GUIBUX_ACT_CLOSE:
		if (toplevel != NULL) {
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		}
		break;
	case GUIBUX_ACT_FULLSCREEN:
		if (toplevel != NULL) {
			set_fullscreen(toplevel, !toplevel->is_fullscreen);
		}
		break;
	case GUIBUX_ACT_TILE:
		if (toplevel != NULL) {
			struct guibux_output *o = guibux_output_for(server,
				toplevel_output_for(toplevel));
			if (o != NULL) {
				o->tile_mode = (o->tile_mode + 1) % 3;
				retile_output(o);
				wlr_log(WLR_INFO, "tile mode on %s: %s",
					o->wlr_output->name ? o->wlr_output->name : "(unknown)",
					o->tile_mode == GUIBUX_TILE_FREE ? "free"
					: o->tile_mode == GUIBUX_TILE_SPLIT ? "split"
					: "main+stack");
			}
		}
		break;
	case GUIBUX_ACT_LAUNCHER:
		launcher_show(server);
		break;
	case GUIBUX_ACT_FOCUS_NEXT: {
		// cycle focus over visible windows only (wraps around)
		struct guibux_toplevel *next = NULL;
		struct guibux_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t == toplevel) {
				continue;
			}
			if (toplevel_visible(t)) {
				next = t;
				break;
			}
		}
		if (next != NULL) {
			focus_toplevel(next);
		}
		break;
	}
	case GUIBUX_ACT_QUIT:
		wl_display_terminate(server->wl_display);
		break;
	case GUIBUX_ACT_SWITCH_WS: {
		struct wlr_output *out = toplevel != NULL
			? toplevel_output_for(toplevel) : output_at_cursor(server);
		struct guibux_output *o = out != NULL
			? guibux_output_for(server, out) : NULL;
		if (o != NULL) {
			switch_workspace(o, arg);
		}
		break;
	}
	case GUIBUX_ACT_MOVE_WS:
		if (toplevel != NULL) {
			move_toplevel_to_workspace(toplevel, arg);
		}
		break;
	case GUIBUX_ACT_MOVE_MON_LEFT:
		if (toplevel != NULL) {
			move_toplevel_to_adjacent_output(server, toplevel, -1);
		}
		break;
	case GUIBUX_ACT_MOVE_MON_RIGHT:
		if (toplevel != NULL) {
			move_toplevel_to_adjacent_output(server, toplevel, 1);
		}
		break;
	}
}

// look up (modifiers, keysym) in the keybind table and run the action;
// returns true if a binding matched
static bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym,
		uint32_t modifiers) {
	uint32_t mods = modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT |
		WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL);
	struct guibux_toplevel *toplevel = wl_list_empty(&server->toplevels) ? NULL :
		wl_container_of(server->toplevels.next, toplevel, link);
	for (int i = 0; i < server->num_keybinds; i++) {
		struct guibux_keybind *kb = &server->keybinds[i];
		if (kb->modifiers == mods && kb->keysym == sym) {
			do_action(server, kb->action, kb->arg, toplevel);
			return true;
		}
	}
	return false;
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
	if (server->xkb_layout != NULL || server->xkb_variant != NULL ||
			server->xkb_options != NULL) {
		struct xkb_rule_names rules = {0};
		rules.layout = server->xkb_layout;
		rules.variant = server->xkb_variant;
		rules.options = server->xkb_options;
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
	wlr_log(WLR_INFO, "keyboard: layout '%s' variant '%s' options '%s'",
		server->xkb_layout ? server->xkb_layout : "default",
		server->xkb_variant ? server->xkb_variant : "default",
		server->xkb_options ? server->xkb_options : "default");

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
	int ws = 0;
	if (topbar_workspace_at(server, server->cursor->x, server->cursor->y,
			NULL, &ws) && ws != 0) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "pointer");
	} else if (!toplevel) {
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
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct guibux_output *o = NULL;
		int ws = 0;
		if (topbar_workspace_at(server, server->cursor->x, server->cursor->y,
				&o, &ws)) {
			// bar click: switch workspace on a cell hit, always consumed
			if (ws != 0) {
				switch_workspace(o, ws);
			}
			return;
		}
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
	struct guibux_server *server = output->server;

	topbar_destroy(output);
	topbar_renumber(server);

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
	output->current_workspace = 1;

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
	topbar_create(output);
	topbar_renumber(server);
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

	struct wlr_output *output = output_at_cursor(toplevel->server);
	struct guibux_output *o = output != NULL
		? guibux_output_for(toplevel->server, output) : NULL;
	toplevel->workspace = o != NULL ? o->current_workspace : 1;
	if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
		// park the node on its target output so retile_output picks it up
		struct wlr_box box;
		wlr_output_layout_get_box(toplevel->server->output_layout,
			output, &box);
		wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
		retile_output(o);
	} else {
		place_toplevel(toplevel);
	}
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct guibux_server *server = toplevel->server;

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	struct guibux_output *o = guibux_output_for(server,
		toplevel_output_for(toplevel));
	wl_list_remove(&toplevel->link);
	if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
		retile_output(o);
	}

	if (!wl_list_empty(&server->toplevels)) {
		struct guibux_toplevel *next =
			wl_container_of(server->toplevels.next, next, link);
		focus_toplevel(next);
	} else {
		// last window gone: drop keyboard focus so the seat does not
		// keep pointing at a destroyed surface
		clear_keyboard_focus(server);
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct guibux_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		struct guibux_output *o = guibux_output_for(toplevel->server,
			toplevel_output_for(toplevel));
		if (o != NULL && o->tile_mode != GUIBUX_TILE_FREE) {
			// the window is tiled on map (it is not in the toplevels list
			// yet); send an initial configure so the client can commit its
			// first buffer
			wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
		} else {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		}
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

// ---------------------------------------------------------------------------
// Config file: keybinds, terminal, keyboard layout/variant/options, colors
//
// keybinds_defaults() fills the table with the built-in bindings; the config
// file then replaces entries with the same (modifiers, keysym) or appends
// new ones.
// ---------------------------------------------------------------------------

static void keybind_add(struct guibux_server *server, uint32_t modifiers,
		xkb_keysym_t keysym, enum guibux_action action, int arg) {
	for (int i = 0; i < server->num_keybinds; i++) {
		struct guibux_keybind *kb = &server->keybinds[i];
		if (kb->modifiers == modifiers && kb->keysym == keysym) {
			kb->action = action;
			kb->arg = arg;
			return;
		}
	}
	if (server->num_keybinds >= NUM_KEYBINDS) {
		wlr_log(WLR_ERROR, "config: too many keybinds (max %d)", NUM_KEYBINDS);
		return;
	}
	struct guibux_keybind *kb = &server->keybinds[server->num_keybinds++];
	kb->modifiers = modifiers;
	kb->keysym = keysym;
	kb->action = action;
	kb->arg = arg;
}

static void keybinds_defaults(struct guibux_server *server) {
	keybind_add(server, WLR_MODIFIER_ALT, XKB_KEY_Escape, GUIBUX_ACT_QUIT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT, XKB_KEY_Escape,
		GUIBUX_ACT_QUIT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Return, GUIBUX_ACT_TERMINAL, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_q, GUIBUX_ACT_CLOSE, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_f, GUIBUX_ACT_FULLSCREEN, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_t, GUIBUX_ACT_TILE, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_e, GUIBUX_ACT_LAUNCHER, 0);
	keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_Tab, GUIBUX_ACT_FOCUS_NEXT, 0);
	for (int ws = 1; ws <= NUM_WORKSPACES; ws++) {
		keybind_add(server, WLR_MODIFIER_LOGO, XKB_KEY_1 + ws - 1,
			GUIBUX_ACT_SWITCH_WS, ws);
		keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT,
			XKB_KEY_1 + ws - 1, GUIBUX_ACT_MOVE_WS, ws);
	}
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Left,
		GUIBUX_ACT_MOVE_MON_LEFT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Right,
		GUIBUX_ACT_MOVE_MON_RIGHT, 0);
	keybind_add(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_q,
		GUIBUX_ACT_QUIT, 0);
}

// parse "Mod+Shift+q: action[:arg]" (at least one modifier required)
static bool parse_keybind(struct guibux_server *server, const char *value) {
	char *copy = strdup(value);
	if (copy == NULL) {
		return false;
	}
	char *colon = strchr(copy, ':');
	if (colon == NULL) {
		wlr_log(WLR_ERROR, "config: bad keybind '%s' (expected 'MODS+key: action')", value);
		free(copy);
		return false;
	}
	*colon = '\0';
	const char *action_str = colon + 1;
	while (*action_str == ' ' || *action_str == '\t') {
		action_str++;
	}
	char *keyspec = copy;

	char *plus = strrchr(keyspec, '+');
	xkb_keysym_t sym;
	uint32_t mods = 0;
	if (plus != NULL) {
		*plus = '\0';
		sym = xkb_keysym_from_name(plus + 1, XKB_KEYSYM_NO_FLAGS);
		if (sym == XKB_KEY_NoSymbol) {
			wlr_log(WLR_ERROR, "config: bad keybind '%s' (unknown key '%s')",
				value, plus + 1);
			free(copy);
			return false;
		}
		char *save = NULL;
		for (char *tok = strtok_r(keyspec, "+", &save); tok != NULL;
				tok = strtok_r(NULL, "+", &save)) {
			if (!strcmp(tok, "Mod") || !strcmp(tok, "Super")) {
				mods |= WLR_MODIFIER_LOGO;
			} else if (!strcmp(tok, "Shift")) {
				mods |= WLR_MODIFIER_SHIFT;
			} else if (!strcmp(tok, "Alt")) {
				mods |= WLR_MODIFIER_ALT;
			} else if (!strcmp(tok, "Ctrl")) {
				mods |= WLR_MODIFIER_CTRL;
			} else {
				wlr_log(WLR_ERROR, "config: bad keybind '%s' (unknown modifier '%s')",
					value, tok);
				free(copy);
				return false;
			}
		}
	} else {
		sym = xkb_keysym_from_name(keyspec, XKB_KEYSYM_NO_FLAGS);
		if (sym == XKB_KEY_NoSymbol) {
			wlr_log(WLR_ERROR, "config: bad keybind '%s' (unknown key)", value);
			free(copy);
			return false;
		}
	}
	if (mods == 0) {
		wlr_log(WLR_ERROR, "config: bad keybind '%s' (at least one modifier required)", value);
		free(copy);
		return false;
	}

	int arg = 0;
	char *argcolon = strchr((char *)action_str, ':');
	if (argcolon != NULL) {
		*argcolon = '\0';
		arg = atoi(argcolon + 1);
	}
	enum guibux_action action;
	if (!strcmp(action_str, "terminal")) {
		action = GUIBUX_ACT_TERMINAL;
	} else if (!strcmp(action_str, "close")) {
		action = GUIBUX_ACT_CLOSE;
	} else if (!strcmp(action_str, "fullscreen")) {
		action = GUIBUX_ACT_FULLSCREEN;
	} else if (!strcmp(action_str, "tile")) {
		action = GUIBUX_ACT_TILE;
	} else if (!strcmp(action_str, "launcher")) {
		action = GUIBUX_ACT_LAUNCHER;
	} else if (!strcmp(action_str, "focus-next")) {
		action = GUIBUX_ACT_FOCUS_NEXT;
	} else if (!strcmp(action_str, "quit")) {
		action = GUIBUX_ACT_QUIT;
	} else if (!strcmp(action_str, "workspace")) {
		if (arg < 1 || arg > NUM_WORKSPACES) {
			wlr_log(WLR_ERROR, "config: bad keybind '%s' (workspace %d out of range 1..%d)",
				value, arg, NUM_WORKSPACES);
			free(copy);
			return false;
		}
		action = GUIBUX_ACT_SWITCH_WS;
	} else if (!strcmp(action_str, "move-workspace")) {
		if (arg < 1 || arg > NUM_WORKSPACES) {
			wlr_log(WLR_ERROR, "config: bad keybind '%s' (move-workspace %d out of range 1..%d)",
				value, arg, NUM_WORKSPACES);
			free(copy);
			return false;
		}
		action = GUIBUX_ACT_MOVE_WS;
	} else if (!strcmp(action_str, "move-monitor-left")) {
		action = GUIBUX_ACT_MOVE_MON_LEFT;
	} else if (!strcmp(action_str, "move-monitor-right")) {
		action = GUIBUX_ACT_MOVE_MON_RIGHT;
	} else {
		wlr_log(WLR_ERROR, "config: bad keybind '%s' (unknown action '%s')",
			value, action_str);
		free(copy);
		return false;
	}

	wlr_log(WLR_INFO, "config: keybind '%s' -> %s", value, action_str);
	free(copy);
	keybind_add(server, mods, sym, action, arg);
	return true;
}

// parse "#rrggbb"
static bool parse_color(const char *value, uint32_t *out) {
	uint32_t c;
	if (sscanf(value, "#%x", &c) != 1 || c > 0xFFFFFF) {
		return false;
	}
	*out = c;
	return true;
}

static void load_config(struct guibux_server *server, const char *path) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		if (errno != ENOENT) {
			wlr_log(WLR_ERROR, "config: cannot open %s: %m", path);
		}
		return;
	}
	wlr_log(WLR_INFO, "config: loading %s", path);
	char line[512];
	int lineno = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0' || *p == '#' || *p == '\n') {
			continue;
		}
		char *eq = strchr(p, '=');
		if (eq == NULL) {
			wlr_log(WLR_ERROR, "config: %s:%d: expected 'key = value'", path, lineno);
			continue;
		}
		*eq = '\0';
		char *key = p;
		char *val = eq + 1;
		char *end = key + strlen(key);
		while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
			*--end = '\0';
		}
		while (*val == ' ' || *val == '\t') {
			val++;
		}
		end = val + strlen(val);
		while (end > val && (end[-1] == ' ' || end[-1] == '\t' ||
				end[-1] == '\n' || end[-1] == '\r')) {
			*--end = '\0';
		}
		if (*key == '\0' || *val == '\0') {
			wlr_log(WLR_ERROR, "config: %s:%d: empty key or value", path, lineno);
			continue;
		}

		if (!strcmp(key, "term")) {
			free(server->term_cmd);
			server->term_cmd = strdup(val);
			wlr_log(WLR_INFO, "config: term = %s", val);
		} else if (!strcmp(key, "xkb_layout")) {
			free(server->xkb_layout);
			server->xkb_layout = strdup(val);
			wlr_log(WLR_INFO, "config: xkb_layout = %s", val);
		} else if (!strcmp(key, "xkb_variant")) {
			free(server->xkb_variant);
			server->xkb_variant = strdup(val);
			wlr_log(WLR_INFO, "config: xkb_variant = %s", val);
		} else if (!strcmp(key, "xkb_options")) {
			free(server->xkb_options);
			server->xkb_options = strdup(val);
			wlr_log(WLR_INFO, "config: xkb_options = %s", val);
		} else if (!strcmp(key, "keybind")) {
			parse_keybind(server, val);
		} else if (!strcmp(key, "color_bg")) {
			if (parse_color(val, &server->color_bg)) {
				wlr_log(WLR_INFO, "config: color_bg = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "color_border")) {
			if (parse_color(val, &server->color_border)) {
				wlr_log(WLR_INFO, "config: color_border = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "color_highlight")) {
			if (parse_color(val, &server->color_highlight)) {
				wlr_log(WLR_INFO, "config: color_highlight = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "color_text")) {
			if (parse_color(val, &server->color_text)) {
				wlr_log(WLR_INFO, "config: color_text = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "color_dim")) {
			if (parse_color(val, &server->color_dim)) {
				wlr_log(WLR_INFO, "config: color_dim = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "topbar_bg")) {
			if (parse_color(val, &server->color_topbar_bg)) {
				wlr_log(WLR_INFO, "config: topbar_bg = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "topbar_text")) {
			if (parse_color(val, &server->color_topbar_text)) {
				wlr_log(WLR_INFO, "config: topbar_text = %s", val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else {
			wlr_log(WLR_ERROR, "config: %s:%d: unknown key '%s'", path, lineno, key);
		}
	}
	fclose(f);
}

// test hook: GUIBUX_TEST_KEYBIND="g" sends Mod+g through the keybind table
// shortly after start and checks the launcher opened
static int keybind_test_run(void *data) {
	struct guibux_server *server = data;
	const char *key = getenv("GUIBUX_TEST_KEYBIND");
	if (key == NULL) {
		return 0;
	}
	xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL unknown key '%s'", key);
		return 0;
	}
	bool handled = handle_keybinding(server, sym, WLR_MODIFIER_LOGO);
	if (!handled) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL Mod+%s not in keybind table", key);
		return 0;
	}
	if (!server->launcher.active) {
		wlr_log(WLR_ERROR, "keybind-test: FAIL launcher not active after Mod+%s", key);
		return 0;
	}
	launcher_hide(server);
	wlr_log(WLR_INFO, "keybind-test: OK (Mod+%s opened the launcher)", key);
	return 0;
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);

	char *term_cmd = NULL;
	char *xkb_layout = NULL;
	char *config_path = NULL;
	int c;
	while ((c = getopt(argc, argv, "t:k:c:h")) != -1) {
		switch (c) {
		case 't':
			term_cmd = optarg;
			break;
		case 'k':
			xkb_layout = optarg;
			break;
		case 'c':
			config_path = optarg;
			break;
		default:
			printf("Usage: %s [-t terminal command] [-k keyboard layout] [-c config file]\n", argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-t terminal command] [-k keyboard layout] [-c config file]\n", argv[0]);
		return 1;
	}

	struct guibux_server server = {0};
	server.color_bg = DEFAULT_COLOR_BG;
	server.color_border = DEFAULT_COLOR_BORDER;
	server.color_highlight = DEFAULT_COLOR_HIGHLIGHT;
	server.color_text = DEFAULT_COLOR_TEXT;
	server.color_dim = DEFAULT_COLOR_DIM;
	server.color_topbar_bg = DEFAULT_COLOR_TOPBAR_BG;
	server.color_topbar_text = DEFAULT_COLOR_TOPBAR_TEXT;
	keybinds_defaults(&server);

	// config file: -c flag > GUIBUX_CONFIG env > ~/.config/guibuxwm/config
	if (config_path == NULL) {
		config_path = getenv("GUIBUX_CONFIG");
	}
	if (config_path == NULL) {
		const char *home = getenv("HOME");
		if (home != NULL) {
			static char default_config[PATH_MAX];
			snprintf(default_config, sizeof(default_config),
				"%s/.config/guibuxwm/config", home);
			config_path = default_config;
		}
	}
	if (config_path != NULL) {
		load_config(&server, config_path);
	}

	// env vars fill what the config file did not set
	if (server.term_cmd == NULL) {
		const char *env_term = getenv("GUIBUX_TERM");
		server.term_cmd = strdup(env_term ? env_term : "gnome-terminal");
	}
	if (server.xkb_layout == NULL) {
		const char *env_layout = getenv("GUIBUX_XKB_LAYOUT")
			? getenv("GUIBUX_XKB_LAYOUT") : getenv("XKB_DEFAULT_LAYOUT");
		if (env_layout != NULL) {
			server.xkb_layout = strdup(env_layout);
		}
	}
	// command-line flags override everything
	if (term_cmd != NULL) {
		free(server.term_cmd);
		server.term_cmd = strdup(term_cmd);
	}
	if (xkb_layout != NULL) {
		free(server.xkb_layout);
		server.xkb_layout = strdup(xkb_layout);
	}
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

	// test hook: GUIBUX_TEST_TILE_MODE sets the tile mode of all outputs
	const char *tile_test_mode = getenv("GUIBUX_TEST_TILE_MODE");
	if (tile_test_mode != NULL) {
		server.tile_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			tile_test_run, &server);
		wl_event_source_timer_update(server.tile_test_timer, 500);
	}

	// topbar clock: refresh the date/time on every bar once per second
	server.topbar_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server.wl_display),
		topbar_tick, &server);
	wl_event_source_timer_update(server.topbar_timer, 1000);

	// test hook: GUIBUX_TEST_TOPBAR verifies the topbars shortly after start
	const char *topbar_test = getenv("GUIBUX_TEST_TOPBAR");
	if (topbar_test != NULL) {
		server.topbar_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			topbar_test_run, &server);
		wl_event_source_timer_update(server.topbar_test_timer, 500);
	}

	// test hook: GUIBUX_TEST_WORKSPACES=N exercises the workspace state
	// machine (late enough for a test client to map windows)
	const char *workspace_test = getenv("GUIBUX_TEST_WORKSPACES");
	if (workspace_test != NULL) {
		test_seat_add_keyboard(&server);
		server.workspace_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			workspace_test_run, &server);
		wl_event_source_timer_update(server.workspace_test_timer, 2000);
	}

	// test hook: GUIBUX_TEST_KEYBIND="key" sends Mod+key through the
	// keybind table shortly after start
	const char *keybind_test = getenv("GUIBUX_TEST_KEYBIND");
	if (keybind_test != NULL) {
		server.keybind_test_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(server.wl_display),
			keybind_test_run, &server);
		wl_event_source_timer_update(server.keybind_test_timer, 500);
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

	if (server.topbar_timer != NULL) {
		wl_event_source_remove(server.topbar_timer);
	}
	if (server.topbar_test_timer != NULL) {
		wl_event_source_remove(server.topbar_test_timer);
	}
	if (server.workspace_test_timer != NULL) {
		wl_event_source_remove(server.workspace_test_timer);
	}
	if (server.keybind_test_timer != NULL) {
		wl_event_source_remove(server.keybind_test_timer);
	}

	launcher_hide(&server);
	launcher_free_commands(&server.launcher);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	// destroy the backend before the scene tree: destroying it destroys the
	// outputs, whose destroy handler tears down each output's topbar scene
	// node and re-renders the remaining bars. the scene tree and the
	// renderer/allocator/layout the topbars rely on must still be alive.
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_output_layout_destroy(server.output_layout);
	wlr_allocator_destroy(server.allocator);
	if (server.launcher.shm_alloc != NULL) {
		wlr_allocator_destroy(server.launcher.shm_alloc);
	}
	if (server.launcher.ft != NULL) {
		FT_Done_FreeType(server.launcher.ft);
	}
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.wl_display);
	free(server.term_cmd);
	free(server.xkb_layout);
	free(server.xkb_variant);
	free(server.xkb_options);
	return 0;
}
