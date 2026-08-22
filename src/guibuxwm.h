#ifndef GUIBUXWM_H
#define GUIBUXWM_H

#include <cairo.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>
#include <wlr/util/edges.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <dbus/dbus.h>

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

#define LAUNCHER_BOX_W 480
#define LAUNCHER_BOX_H 40
#define LAUNCHER_FONT_PX 20
#define LAUNCHER_LINE_H 28

struct output_placement {
	char name[64];
	int x, y;
	int transform;
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
	GUIBUX_ACT_SNAP_LEFT,
	GUIBUX_ACT_SNAP_RIGHT,
};

struct guibux_keybind {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	enum guibux_action action;
	int arg;
};

struct guibux_server;

/* sysinfo.c */
struct guibux_sysinfo {
    DBusConnection *system_bus;
    bool nm_available;
    char network[64];
    char battery[32];
};

void sysinfo_init(struct guibux_server *server);
void sysinfo_destroy(struct guibux_server *server);
void sysinfo_update(struct guibux_sysinfo *si);
struct guibux_toplevel;

// declared in wlr/render/allocator/shm.h (not installed with our wlroots build)
struct wlr_allocator *wlr_shm_allocator_create(void);

struct guibux_output {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_output *wlr_output;
	int tile_mode;
	int current_workspace;
	struct wlr_scene_buffer *topbar_node;
	struct wlr_buffer *topbar_buffer;
	int topbar_number;
    char topbar_right[64];
    char topbar_network[64];
    char topbar_battery[32];
    int topbar_net_x;
    int topbar_net_w;
    int topbar_ws_x[NUM_WORKSPACES + 1];
	int topbar_ws_cell_w;
#define TOPBAR_WIN_W 100
#define TOPBAR_WIN_GAP 8
#define TOPBAR_WIN_MAX 64

	int topbar_win_x[TOPBAR_WIN_MAX];
	int topbar_win_w[TOPBAR_WIN_MAX];
	char topbar_win_titles[TOPBAR_WIN_MAX][64];
	int topbar_win_count;
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
	int workspace;
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
	struct wl_listener request_set_primary_selection;
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
	struct wl_event_source *psel_test_timer;
	bool psel_test_enter_sent;
	struct guibux_launcher launcher;
	struct guibux_sysinfo sysinfo;
	uint32_t last_topbar_click_time;
	struct guibux_toplevel *last_topbar_click_win;
};

/* output.c */
struct wlr_output *output_at_cursor(struct guibux_server *server);
struct guibux_output *guibux_output_for(struct guibux_server *server, struct wlr_output *wlr_output);
struct wlr_output *toplevel_output_for(struct guibux_toplevel *toplevel);
void topbar_raise_all(struct guibux_server *server);
bool toplevel_visible(struct guibux_toplevel *toplevel);
void server_new_output(struct wl_listener *listener, void *data);
void output_frame(struct wl_listener *listener, void *data);
void output_request_state(struct wl_listener *listener, void *data);
void output_destroy(struct wl_listener *listener, void *data);
void topbar_create(struct guibux_output *o);
void topbar_destroy(struct guibux_output *o);
void topbar_renumber(struct guibux_server *server);
int outputs_sorted_by_x(struct guibux_server *server, struct wlr_output **sorted, struct wlr_box *boxes, int cap);

struct guibux_toplevel *topbar_win_at(struct guibux_output *o, double lx, double ly);
bool topbar_network_at(struct guibux_server *server, double lx, double ly);

/* toplevel.c */
void focus_toplevel(struct guibux_toplevel *toplevel);
void set_fullscreen(struct guibux_toplevel *toplevel, bool fullscreen);
void begin_interactive(struct guibux_toplevel *toplevel, enum guibux_cursor_mode mode, uint32_t edges);
void server_new_xdg_toplevel(struct wl_listener *listener, void *data);
void xdg_toplevel_map(struct wl_listener *listener, void *data);
void xdg_toplevel_unmap(struct wl_listener *listener, void *data);
void xdg_toplevel_commit(struct wl_listener *listener, void *data);
void xdg_toplevel_destroy(struct wl_listener *listener, void *data);
void xdg_toplevel_request_move(struct wl_listener *listener, void *data);
void xdg_toplevel_request_resize(struct wl_listener *listener, void *data);
void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data);
void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data);
struct guibux_toplevel *desktop_toplevel_at(struct guibux_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);

/* window-layout.c */
void retile_output(struct guibux_output *output);
void switch_workspace(struct guibux_output *output, int ws);
void move_toplevel_to_workspace(struct guibux_toplevel *toplevel, int ws);
void place_toplevel(struct guibux_toplevel *toplevel);
void move_toplevel_to_output(struct guibux_toplevel *toplevel, struct wlr_output *output);
void move_toplevel_to_adjacent_output(struct guibux_server *server, struct guibux_toplevel *toplevel, int dir);
void snap_toplevel_left(struct guibux_toplevel *toplevel);
void snap_toplevel_right(struct guibux_toplevel *toplevel);

/* topbar.c */
void topbar_render(struct guibux_output *o);
bool topbar_workspace_at(struct guibux_server *server, double lx, double ly, struct guibux_output **output, int *ws);
int topbar_tick(void *data);
int topbar_test_run(void *data);
int guibux_text_width(FT_Face face, const char *text);
int launcher_draw_text_on_surface(cairo_surface_t *cs, FT_Face face,
	const char *text, int x, int baseline, uint32_t color);
void set_color(cairo_t *cr, uint32_t c);

/* launcher.c */
void launcher_init(struct guibux_server *server);
void launcher_show(struct guibux_server *server);
void launcher_hide(struct guibux_server *server);
bool launcher_handle_key(struct guibux_server *server, xkb_keysym_t sym);
int launcher_test_run(void *data);
void launcher_free_commands(struct guibux_launcher *l);
void launcher_filter(struct guibux_launcher *l);

/* keyboard.c */
void server_new_input(struct wl_listener *listener, void *data);
void server_new_keyboard(struct guibux_server *server, struct wlr_input_device *device);
void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
void keyboard_handle_key(struct wl_listener *listener, void *data);
void keyboard_handle_destroy(struct wl_listener *listener, void *data);
bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym, uint32_t modifiers);
void do_action(struct guibux_server *server, enum guibux_action action, int arg, struct guibux_toplevel *toplevel);
void keybinds_defaults(struct guibux_server *server);
void keybind_add(struct guibux_server *server, uint32_t modifiers, xkb_keysym_t keysym, enum guibux_action action, int arg);
void spawn_terminal(struct guibux_server *server);
void spawn_network_info(struct guibux_server *server);

/* config.c */
void load_config(struct guibux_server *server, const char *path);
bool parse_keybind(struct guibux_server *server, const char *value);
bool parse_color(const char *value, uint32_t *out);
void parse_output_placements(struct guibux_server *server);

/* cursor.c */
void reset_cursor_mode(struct guibux_server *server);
void process_cursor_motion(struct guibux_server *server, uint32_t time);
void server_cursor_motion(struct wl_listener *listener, void *data);
void server_cursor_motion_absolute(struct wl_listener *listener, void *data);
void server_cursor_button(struct wl_listener *listener, void *data);
void server_cursor_axis(struct wl_listener *listener, void *data);
void server_cursor_frame(struct wl_listener *listener, void *data);
void seat_request_cursor(struct wl_listener *listener, void *data);
void seat_pointer_focus_change(struct wl_listener *listener, void *data);
void seat_request_set_selection(struct wl_listener *listener, void *data);
void seat_request_set_primary_selection(struct wl_listener *listener, void *data);

/* popup.c */
void server_new_xdg_popup(struct wl_listener *listener, void *data);
void xdg_popup_commit(struct wl_listener *listener, void *data);
void xdg_popup_destroy(struct wl_listener *listener, void *data);

/* wm-internal.c */
void clear_keyboard_focus(struct guibux_server *server);
void end_seat_grabs(struct guibux_server *server);

/* wm-test.c */
void test_seat_add_keyboard(struct guibux_server *server);
int workspace_test_run(void *data);
int tile_test_run(void *data);
int keybind_test_run(void *data);
int psel_test_run(void *data);

#endif /* GUIBUXWM_H */
