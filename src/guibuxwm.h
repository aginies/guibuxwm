#ifndef GUIBUXWM_H
#define GUIBUXWM_H

#include <cairo.h>
#include <cairo-svg.h>
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
#include <pthread.h>

#ifdef WLR_USE_UNSTABLE
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/xwayland.h>
#endif

#include "outputs-config.h"

#define CASCADE_STEP 40
#define CASCADE_MAX 6
#define MAX_OUTPUT_PLACEMENTS 8
#define LAUNCHER_MAX_MATCHES 8
#define LAUNCHER_MAX_PREFERRED 5
#define LAUNCHER_MAX_COMMANDS 4096
#define TOPBAR_H 24
#define DEFAULT_TOPBAR_H 24
#define DEFAULT_TOPBAR_FONT_SIZE 16
#define DEFAULT_TOPBAR_WIN_PAD 2
#define TOPBAR_PAD 8
#define TOPBAR_ITEMS_MAX 6
enum topbar_item_id {
	TOPBAR_ITEM_NETWORK = 0,
	TOPBAR_ITEM_VOLUME,
	TOPBAR_ITEM_MIC,
	TOPBAR_ITEM_BATTERY,
	TOPBAR_ITEM_NOTIFICATIONS,
	TOPBAR_ITEM_CLOCK,
};
#define NUM_WORKSPACES 4
#define OVERVIEW_WS_COL_W 72
#define NUM_KEYBINDS 64
#define MAX_WINDOWS 128
#define DEFAULT_COLOR_BG 0x1e1e2e
#define DEFAULT_COLOR_BORDER 0x45475a
#define DEFAULT_COLOR_HIGHLIGHT 0x3a3c55
#define DEFAULT_COLOR_TEXT 0xffffff
#define DEFAULT_COLOR_DIM 0x8888aa
#define DEFAULT_COLOR_TOPBAR_BG 0x8839ef
#define DEFAULT_COLOR_TOPBAR_TEXT 0x1e1e2e

#define LAUNCHER_BOX_W 480
#define LAUNCHER_BOX_H 40
#define LAUNCHER_FONT_PX 20
#define LAUNCHER_LINE_H 28

struct output_placement {
	char name[64];
	int x, y;
	int transform;
	int mode_w, mode_h;  /* 0 = preferred mode */
	bool disabled;  /* NAME@off: the output is intentionally turned off */
	bool used;      /* matched a connected output */
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

enum guibux_bg_scale {
	BG_STRETCH,
	BG_FIT,
	BG_FILL,
	BG_TILE,
};

/* effects.c - optional animations (build option 'effects', config 'effects') */
enum guibux_open_effect {
	OPEN_EFFECT_SCALE,
	OPEN_EFFECT_SLIDE,
	OPEN_EFFECT_NONE,
};

enum guibux_effect_kind {
	EFFECT_POS,
	EFFECT_SCALE_FACTOR,
	EFFECT_SCALE_TO,
};

#define EFFECTS_MAX_ANIMS 64

struct guibux_effects_anim {
	bool used;
	enum guibux_effect_kind kind;
	struct wlr_scene_node *node;
	struct wl_listener node_destroy;
	/* POS: from (fx,fy) to (tx,ty) */
	double fx, fy, tx, ty;
	/* SCALE_FACTOR: factor from f0 to f1 of the surface's logical size
	 * SCALE_TO: dest size from (sw,sh) to (tw,th) */
	double f0, f1;
	int sw, sh, tw, th;
	int64_t start_ms;
	int duration_ms;
	/* POS on a toplevel: size configure to send when the anim ends */
	int pw, ph;
	void (*done)(void *data);
	void *done_data;
};

struct guibux_effects {
	struct guibux_effects_anim anims[EFFECTS_MAX_ANIMS];
	int64_t last_tick_ms;
};

/* window-layout.c: absolute node position + logical size of one tile cell */
struct guibux_tile_target {
	struct guibux_toplevel *t;
	int x, y, w, h;
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
	GUIBUX_ACT_SNAP_TOP,
	GUIBUX_ACT_SNAP_BOTTOM,
	GUIBUX_ACT_SWITCH_WS_LEFT,
	GUIBUX_ACT_SWITCH_WS_RIGHT,
	GUIBUX_ACT_SHOW_HELP,
	GUIBUX_ACT_VOLUME_UP,
	GUIBUX_ACT_VOLUME_DOWN,
	GUIBUX_ACT_MUTE,
	GUIBUX_ACT_MIC_UP,
	GUIBUX_ACT_MIC_DOWN,
	GUIBUX_ACT_MIC_MUTE,
	GUIBUX_ACT_BRIGHTNESS_UP,
	GUIBUX_ACT_BRIGHTNESS_DOWN,
	GUIBUX_ACT_OUTPUTS_APPLY,
	GUIBUX_ACT_OUTPUTS_PANEL,
	GUIBUX_ACT_POWER,
	GUIBUX_ACT_RELOAD_CONFIG,
	GUIBUX_ACT_TOPBAR_ITEMS,
};

struct guibux_keybind {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	enum guibux_action action;
	int arg;
};

struct guibux_server;

/* sysinfo.c */
#define NET_IFACES_MAX 8
#define NET_STR_MAX 128

/* one connected network device: the label is exactly what the topbar
 * renders ("SSID 85%" / "eth0"); ip/dns/gw are empty when unknown;
 * is_wifi is true for NM device type 2 (WIFI) */
struct guibux_net_iface {
    char label[64];
    char ip[NET_STR_MAX];
    char dns[NET_STR_MAX];
    char gw[NET_STR_MAX];
    bool is_wifi;
};

struct guibux_sysinfo {
    DBusConnection *system_bus;
    bool nm_available;
    bool upower_available;
    char network[64];
    struct guibux_net_iface net_ifaces[NET_IFACES_MAX];
    int net_iface_count;
    char battery[32];
    /* UPower State: 0=unknown 1=charging 2=discharging 3=empty 4=full */
    int bat_state;
    /* TimeToEmpty (discharging) / TimeToFull (charging) in seconds;
     * 0 = UPower has no estimate */
    int bat_eta_sec;
    /* audio (pactl poll): -1 volume = unknown */
    bool audio_available;
    int volume;
    bool muted;
    int mic_volume;
    bool mic_muted;
    /* brightness (brightnessctl poll): -1 = unknown */
    bool brightness_available;
    int brightness;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool worker_running;
};

struct guibux_sysinfo_snapshot {
    char net[64];
    struct guibux_net_iface net_ifaces[NET_IFACES_MAX];
    int net_iface_count;
    char bat[32];
    int bat_state;
    int bat_eta_sec;
    bool audio_available;
    int volume;
    bool muted;
    int mic_volume;
    bool mic_muted;
    bool brightness_available;
    int brightness;
};

void sysinfo_init(struct guibux_server *server);
void sysinfo_destroy(struct guibux_server *server);
void sysinfo_get(struct guibux_sysinfo *si, struct guibux_sysinfo_snapshot *snap);
void sysinfo_audio_adjust(struct guibux_sysinfo *si, bool mic, int delta);
void sysinfo_audio_toggle_mute(struct guibux_sysinfo *si, bool mic);
void sysinfo_brightness_adjust(struct guibux_sysinfo *si, int delta);

/* notify.c - desktop notifications (org.freedesktop.Notifications) */
#define NOTIF_MAX 32
#define NOTIF_PANEL_MAX 10

struct guibux_notification {
    uint32_t id;
    char app_name[64];
    char summary[128];
    char body[256];
    int32_t expire;
    time_t created;
};

struct guibux_notify {
    DBusConnection *session_bus;
    bool daemon;
    uint32_t next_id;
    struct guibux_notification items[NOTIF_MAX];
    int count;
    bool dirty;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool worker_running;
};

struct guibux_notif_panel {
    bool active;
    /* slide-out in progress: the node and buffer are freed by the
     * animation completion callback, not by notify_panel_hide */
    bool hiding;
    struct wlr_output *output;
    struct wlr_scene_buffer *scene_node;
    struct wlr_buffer *buffer;
    /* panel is right-aligned: box_x is its left edge, logical px
     * relative to the output origin (hit tests must use it) */
    int box_x, box_w, box_h, box_scale;
    int box_rows;
    uint32_t row_ids[NOTIF_PANEL_MAX];
    int row_y[NOTIF_PANEL_MAX];
    int num_rows;
    int clear_x, clear_y, clear_w, clear_h;
};

/* tooltip.c - small floating text box; shown when hovering the topbar
 * battery indicator (time estimate) or a network interface segment
 * (IP / gateway / DNS); multi-line, one line per field */
enum guibux_tooltip_kind {
    TOOLTIP_BATTERY,
    TOOLTIP_NET,
};

struct guibux_tooltip {
    bool active;
    enum guibux_tooltip_kind kind;
    int net_idx;
    struct wlr_output *output;
    struct wlr_scene_buffer *scene_node;
    struct wlr_buffer *buffer;
    /* box position is logical px relative to the output origin */
    int box_x, box_y, box_w, box_h, box_scale;
    char text[128];
    /* hover arming: the output whose indicator is under the pointer,
     * which indicator, and when it was entered (ms, compared against
     * the monotonic clock by tooltip_tick) */
    struct guibux_output *hover_output;
    enum guibux_tooltip_kind hover_kind;
    int hover_net_idx;
    uint32_t hover_since;
};

/* window-preview.c - thumbnail of a topbar window-list item; shown
 * when hovering a window label so the content of a window on another
 * workspace is visible without switching to it. The preview is a CPU
 * snapshot of the window's buffer (avoids holding a lock on the live
 * buffer that would block xwayland damage updates) */
struct guibux_window_preview {
    bool active;
    struct wlr_output *output;
    struct wlr_scene_buffer *scene_node;
    struct wlr_buffer *buffer;
    struct guibux_toplevel *toplevel;
    /* box position is logical px relative to the output origin */
    int box_x, box_y, box_w, box_h;
    /* hover arming: the window under the pointer, the bar it was hovered
     * on, and when it was entered (ms, compared against the monotonic
     * clock by preview_tick) */
    struct guibux_toplevel *hover_toplevel;
    struct wlr_output *hover_output;
    uint32_t hover_since;
};

/* osd.c - on-screen display for volume/mic/brightness changes: a
 * centered box with label, value and a bar; auto-hides after a timeout */
enum guibux_osd_kind {
    OSD_VOLUME,
    OSD_MIC,
    OSD_BRIGHTNESS,
};

struct guibux_osd {
    bool active;
    struct wlr_output *output;
    struct wlr_scene_buffer *scene_node;
    struct wlr_buffer *buffer;
    /* box position is logical px relative to the output origin */
    int box_x, box_y, box_w, box_h, box_scale;
    enum guibux_osd_kind kind;
    int value;
    bool muted;
    uint32_t hide_at_ms;
};

/* window-restore.c: last known position per app, persisted across
 * WM restarts (state file under XDG_STATE_HOME) */
#define RESTORE_MAX_ENTRIES 128

struct guibux_window_pos {
	char output_name[64];
	/* layout box of the saved output: a monitor with the same name but
	 * a different box is a different physical monitor (replugged) and
	 * must not receive the restored window */
	int box_x, box_y;
	bool box_valid;
	int workspace;
	int x, y, w, h;
};

struct guibux_window_restore_entry {
	char app_id[256];
	struct guibux_window_pos pos;
};

struct guibux_window_restore {
	struct guibux_window_restore_entry entries[RESTORE_MAX_ENTRIES];
	int count;
};

enum restore_result {
	RESTORE_NONE,  /* no usable saved position: normal placement */
	RESTORE_TILE,  /* output+workspace resolved, tile layout places it */
	RESTORE_FREE,  /* position+size fully applied */
};

struct guibux_toplevel;

// declared in wlr/render/allocator/shm.h (not installed with our wlroots build)
struct wlr_allocator *wlr_shm_allocator_create(void);
struct wlr_buffer *wlr_allocator_create_buffer(struct wlr_allocator *alloc,
	int width, int height, const struct wlr_drm_format *format);

struct guibux_output {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;  /* created once, even when disabled */
	bool disabled;  /* placement @off: kept alive, out of the layout */
	int tile_modes[NUM_WORKSPACES + 1];  // 1-indexed, per-workspace persistence
	int tile_mode;                       // active (tile_modes[current_workspace])
	int current_workspace;
	struct wlr_scene_buffer *topbar_node;
	struct wlr_buffer *topbar_buffer;
	int topbar_buffer_w, topbar_buffer_h;
	int topbar_number;
    char topbar_right[64];
    char topbar_network[64];
    char topbar_battery[32];
    /* last rendered sysinfo values: the tick marks the bar dirty when
     * the worker thread publishes a change */
    bool topbar_audio_avail;
    int topbar_vol_pct;
    bool topbar_vol_muted;
    int topbar_mic_pct;
    bool topbar_mic_muted;
    int topbar_net_x[NET_IFACES_MAX];
    int topbar_net_w[NET_IFACES_MAX];
    int topbar_net_count;
    int topbar_vol_x;
    int topbar_vol_w;
    int topbar_mic_x;
    int topbar_mic_w;
    int topbar_bat_x;
    int topbar_bat_w;
    /* last rendered battery values: pct (-1 = none), UPower state */
    int topbar_bat_pct;
    int topbar_bat_state;
    /* occupancy dots: number of (non-fullscreen) windows per workspace,
     * rendered as small dots under the workspace number */
    int topbar_ws_dots[NUM_WORKSPACES + 1];
    int topbar_notif_x;
    int topbar_notif_w;
    int topbar_ws_x[NUM_WORKSPACES + 1];
    int topbar_ws_cell_w;
    /* last full render's window-region geometry (logical px): the
     * focus-only fast path redraws just this region and needs the
     * indicator/clock layout without recomputing it */
    int topbar_win_region_x;
    int topbar_win_region_end;
    int topbar_sep_gap;
    int topbar_cell_y;
    int topbar_cell_h;
    struct wlr_scene_buffer *bg_node;
    struct wlr_buffer *bg_ws_buffers[NUM_WORKSPACES];
    int bg_w, bg_h, bg_scale;
    struct wlr_scene_rect *overview_dim;
    struct wlr_scene_buffer *overview_ws_col_node;
    struct wlr_buffer *overview_ws_col_buf;
    int overview_ws_col_h;
#define TOPBAR_WIN_GAP 10
#define TOPBAR_WIN_MAX MAX_WINDOWS

    int topbar_win_x[TOPBAR_WIN_MAX];
    int topbar_win_w[TOPBAR_WIN_MAX];
    char topbar_win_titles[TOPBAR_WIN_MAX][72];
    int topbar_win_count;
    struct guibux_toplevel *topbar_wins[TOPBAR_WIN_MAX];
    bool topbar_dirty;
    /* focus-only change: only the active-window pill moved, the rest of
     * the bar is unchanged; topbar_render redraws just the window-pill
     * region instead of the whole surface */
    bool topbar_focus_dirty;
    time_t topbar_minute;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct guibux_toplevel {
	struct wl_list link;
	struct guibux_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener scene_destroy;
	bool is_fullscreen;
	bool managed;
	bool initial_commit;
	/* open animation not started yet: the window has no buffer content
	 * at map time, the effect starts on the first commit with a size */
	bool open_effect_pending;
	int workspace;
	/* the output the window is tiled on; position-based lookup is wrong
	 * while a workspace slide has the window off-screen */
	struct guibux_output *output;
	double saved_x, saved_y;
	int saved_w, saved_h;
	/* restored size to re-assert on the first xdg commit (0 = none) */
	int restore_w, restore_h;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	/* xwayland-only */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_activate;
	struct wl_listener request_close;
	struct wl_listener request_configure;
	struct wl_listener set_title;
	struct wl_listener ping_timeout;
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
	uint32_t pressed[64];
	int num_pressed;
};

struct launcher_entry {
	char *name;
	char *exec;
	char icon_path[256];
};

struct guibux_switcher {
	bool active;
	int selection;
	struct guibux_toplevel *wins[MAX_WINDOWS];
	int num_wins;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	int box_w, box_h, box_scale;
};

struct guibux_overview {
	bool active;
	struct guibux_toplevel *wins[MAX_WINDOWS];
	struct wlr_output *win_output[MAX_WINDOWS];
	int num_wins;
	double saved_x[MAX_WINDOWS], saved_y[MAX_WINDOWS];
	int32_t saved_w[MAX_WINDOWS], saved_h[MAX_WINDOWS];
	struct wlr_scene_buffer *label_node[MAX_WINDOWS];
	struct wlr_buffer *label_buf[MAX_WINDOWS];
	int label_w[MAX_WINDOWS], label_h[MAX_WINDOWS], label_scale[MAX_WINDOWS];
	char label_text[MAX_WINDOWS][128];
	uint32_t ws_colors[NUM_WORKSPACES];
	bool ws_colors_enabled;
	/* drag-to-move (GNOME-style): set on press over a window cell, the
	 * window is grabbed once the pointer passes a small threshold, and
	 * dropped on the workspace row under the cursor on release */
	struct guibux_toplevel *drag_toplevel;
	bool drag_active;
	double drag_press_x, drag_press_y;
	/* workspace column: the (output, ws) cell under the cursor while a
	 * drag is in progress; its column cell is highlighted */
	struct wlr_output *hover_output;
	int hover_ws;
};

struct guibux_help {
	bool active;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	int box_w, box_h, box_scale;
	char lines[NUM_KEYBINDS][128];
	int num_lines;
};

struct guibux_outputs_panel {
	bool active;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	int box_w, box_h, box_scale;
	struct guibux_output_entry entries[OUTPUTS_CONFIG_MAX_ENTRIES];
	int num_entries;
	int selected;
	char status[160];
};

/* power.c: system power menu (Mod+p). Lists suspend/hibernate/lock/
 * logout/restart/shutdown; keyboard- and mouse-driven */
enum guibux_power_action {
	POWER_SUSPEND,
	POWER_HIBERNATE,
	POWER_LOCK,
	POWER_LOGOUT,
	POWER_RESTART,
	POWER_SHUTDOWN,
	POWER_COUNT,
};

struct guibux_power_panel {
	bool active;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	int box_w, box_h, box_scale;
	int selected;
};

struct guibux_topbar_items_panel {
	bool active;
	struct wlr_output *output;
	struct wlr_scene_buffer *scene_node;
	struct wlr_buffer *buffer;
	int box_w, box_h, box_scale;
	int selected;
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
	char icon_theme[64];

	/* icon cache: loaded PNG pixel data */
	struct {
		char path[256];
		uint8_t *data;
		int w, h;
	} icon_cache[LAUNCHER_MAX_MATCHES + LAUNCHER_MAX_PREFERRED];
	int num_icons;

	struct launcher_entry *entries;
	int num_entries;
	struct launcher_entry preferred[LAUNCHER_MAX_PREFERRED];
	int num_preferred;
	int matches[LAUNCHER_MAX_MATCHES];
	int num_matches;
	int selection;
};

struct guibux_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wlr_screencopy_manager_v1 *screencopy;
	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener xdg_activation_request;
	struct wlr_xdg_output_manager_v1 *xdg_output_manager;
	struct wlr_xwayland *xwayland;
	struct wl_listener new_xwayland_surface;
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
	uint32_t button_consumed;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	int cascade;
	char *term_cmd;
	char *term_app_id;  /* Wayland app_id of the terminal; NULL = derive from term_cmd */
	char *xkb_layout;
	char *xkb_variant;
	char *xkb_options;
	int topbar_height;
	int topbar_font_size;
	int topbar_win_pad;
	int topbar_items[TOPBAR_ITEMS_MAX];  /* enabled indicator ids, layout order */
	int topbar_item_count;
	char *background_path;
	enum guibux_bg_scale background_scale;
	char *bg_paths[NUM_WORKSPACES];
	cairo_surface_t *bg_surfaces[NUM_WORKSPACES];
	struct guibux_keybind keybinds[NUM_KEYBINDS];
	int num_keybinds;
	uint32_t color_bg, color_border, color_highlight, color_text, color_dim;
	uint32_t color_topbar_bg, color_topbar_text;
	struct output_placement placements[MAX_OUTPUT_PLACEMENTS];
	int num_placements;
	char *outputs_spec;  /* config `outputs` key; NULL = GUIBUX_OUTPUTS env */
	char *outputs_env_spec;  /* effective spec at start (config, else env) */
	char *config_path;  /* resolved config file path; NULL = none */
	char *renderer_name;  /* config `renderer`: auto|gles2|vulkan|pixman */
	struct wl_event_source *outputs_signal_source;  /* SIGUSR1 = re-apply */
	struct wl_event_source *config_signal_source;   /* SIGHUP = reload config */

	struct wl_event_source *tile_test_timer;
	struct wl_event_source *topbar_timer;
	struct wl_event_source *topbar_test_timer;
	struct wl_event_source *audio_test_timer;
	struct wl_event_source *battery_test_timer;
	struct wl_event_source *scroll_test_timer;
	struct wl_event_source *altdrag_test_timer;
	struct wl_event_source *workspace_test_timer;
	struct wl_event_source *outputs_test_timer;
	struct wl_event_source *outputs_panel_test_timer;
	struct wl_event_source *keybind_test_timer;
	struct wl_event_source *overview_test_timer;
	struct wl_event_source *psel_test_timer;
	struct wl_event_source *resize_test_timer;
	struct wl_event_source *xmondrag_test_timer;
	struct wl_event_source *notify_test_timer;
	struct wl_event_source *tooltip_test_timer;
	struct wl_event_source *osd_test_timer;
	struct wl_event_source *power_test_timer;
	struct wl_event_source *topbar_items_test_timer;
	struct wl_event_source *quit_test_timer;
	struct wl_event_source *global_topbar_test_timer;
	bool psel_test_enter_sent;
struct guibux_launcher launcher;
    struct guibux_sysinfo sysinfo;
	struct guibux_switcher switcher;
	struct guibux_overview overview;
	struct guibux_help help;
	struct guibux_outputs_panel outputs_panel;
    struct guibux_notify notify;
    struct guibux_notif_panel notify_panel;
    struct guibux_tooltip tooltip;
    struct guibux_window_preview window_preview;
    struct guibux_osd osd;
    struct guibux_power_panel power_panel;
    struct guibux_topbar_items_panel topbar_items_panel;
    /* auto-hide: a new notification pops the panel (like an indicator
	 * click); it closes after a delay unless the user interacts with it.
	 * The D-Bus worker thread writes to notify_pipe to wake the main loop */
	struct wl_event_source *notify_autohide_timer;
	int notify_pipe[2];
	struct wl_event_source *notify_pipe_source;
	struct guibux_screensaver {
		struct guibux_server *server;
		int timeout;
		bool active;
		bool dpms_off;
		struct wl_event_source *timer;
		struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
		struct wl_listener inhibit_new;
		struct wlr_idle_notifier_v1 *idle_notify;
	} screensaver;
    uint32_t last_topbar_click_time;
    struct guibux_toplevel *last_topbar_click_win;
    struct guibux_output *cursor_topbar_output;
    bool focus_follow_mouse;
    struct guibux_toplevel *last_ffm_toplevel;

    /* effects (animations): enabled by default, off via config or the
     * 'effects' build option */
    bool effects_enabled;
    int effects_duration_ms;
    enum guibux_open_effect window_open_effect;
    bool notify_effect_slide;
    bool osd_enabled;
    int osd_timeout_ms;
    struct guibux_effects effects;
    struct wl_event_source *effects_test_timer;

    /* window position persistence: last position per app, saved on
     * unmap, restored on map; terminal windows are excluded */
    struct guibux_window_restore window_restore;
    char *terminal_app_id;
    bool restore_positions;
};

/* output.c */
/* an output's fractional scale (1.5) as an integer buffer multiplier;
 * wlroots has no public helper and a plain (int) cast truncates 1.5 to 1,
 * rendering 1x buffers on a 1.5x output (blurry). Round to nearest, min 1 */
static inline int guibux_scale_round(float scale) {
	int s = (int)(scale + 0.5f);
	return s > 1 ? s : 1;
}
/* wall-clock milliseconds (CLOCK_MONOTONIC); used as a synthetic event
 * time when the compositor itself generates pointer input (switcher,
 * xdg-activation cursor warp) */
static inline uint32_t guibux_now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
struct wlr_output *output_at_cursor(struct guibux_server *server);
struct guibux_output *guibux_output_for(struct guibux_server *server, struct wlr_output *wlr_output);
struct wlr_output *toplevel_output_for(struct guibux_toplevel *toplevel);
struct wlr_output *toplevel_output_at_position(struct guibux_toplevel *toplevel);
void topbar_raise_all(struct guibux_server *server);
bool toplevel_visible(struct guibux_toplevel *toplevel);
void server_new_output(struct wl_listener *listener, void *data);
void output_frame(struct wl_listener *listener, void *data);
void output_request_state(struct wl_listener *listener, void *data);
void output_destroy(struct wl_listener *listener, void *data);
/* move the toplevels of a removed output to a live one (unplug);
 * fallback NULL = no output left, the windows stay orphaned */
void output_rehome_toplevels(struct guibux_server *server,
	struct guibux_output *dead, struct guibux_output *fallback);
void topbar_create(struct guibux_output *o);
void topbar_destroy(struct guibux_output *o);
void topbar_renumber(struct guibux_server *server);
int outputs_sorted_by_x(struct guibux_server *server, struct wlr_output **sorted, struct wlr_box *boxes, int cap);
/* re-read the config `outputs` spec (else the startup spec) and apply it
 * to the connected outputs live: position, mode, transform, enable/off */
void outputs_apply(struct guibux_server *server);
/* publish the current outputs (name, box, mode, transform, enabled,
 * available modes) to the state file for the guibuxwm-output tool */
void outputs_state_write(struct guibux_server *server);
/* SIGUSR1 event source: the guibuxwm-output tool triggers a re-apply */
void outputs_apply_init(struct guibux_server *server);

struct guibux_toplevel *topbar_win_at(struct guibux_output *o, double lx, double ly);
bool topbar_network_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly);
int topbar_network_index_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly);
bool topbar_battery_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly);
/* 0 = none, 1 = volume, 2 = mic */
int topbar_audio_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly);

/* toplevel.c */
void focus_toplevel(struct guibux_toplevel *toplevel, bool raise);
void xdg_activation_handle_request(struct wl_listener *listener, void *data);
void set_fullscreen(struct guibux_toplevel *toplevel, bool fullscreen, struct wlr_output *output);
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
void server_new_xwayland_surface(struct wl_listener *listener, void *data);
struct wlr_surface *toplevel_get_surface(struct guibux_toplevel *toplevel);
const char *toplevel_get_title(struct guibux_toplevel *toplevel);
/* best-effort match of a D-Bus notification app_name to a window
 * (xdg app_id / xwayland WM_CLASS); NULL when nothing matches */
struct guibux_toplevel *toplevel_for_app(struct guibux_server *server, const char *app_name);
/* stable identity of a window (xdg app_id / xwayland WM_CLASS instance);
 * used to key persisted positions; may be NULL or empty */
const char *toplevel_app_id(struct guibux_toplevel *toplevel);
bool toplevel_is_xwayland(struct guibux_toplevel *toplevel);
void toplevel_set_size(struct guibux_toplevel *toplevel, int width, int height);
void toplevel_get_geometry(struct guibux_toplevel *toplevel, struct wlr_box *box);
void toplevel_close(struct guibux_toplevel *toplevel);
void toplevel_set_activated(struct guibux_toplevel *toplevel, bool activated);
void toplevel_set_fullscreen_state(struct guibux_toplevel *toplevel, bool fullscreen);
struct guibux_toplevel *desktop_toplevel_at(struct guibux_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy);

/* window-layout.c */
void retile_output(struct guibux_output *output);
int retile_compute(struct guibux_output *output, struct guibux_tile_target *out, int cap);
void switch_workspace(struct guibux_output *output, int ws);
void ws_switch_immediate(struct guibux_output *output, int ws);
/* state change only (current ws, background, grabs, focus, topbar);
 * scene node visibility is applied by the caller */
void ws_switch_state(struct guibux_output *output, int ws);
void move_toplevel_to_workspace(struct guibux_toplevel *toplevel, int ws);
void place_toplevel(struct guibux_toplevel *toplevel);
void move_toplevel_to_output(struct guibux_toplevel *toplevel, struct wlr_output *output);
void move_toplevel_to_adjacent_output(struct guibux_server *server, struct guibux_toplevel *toplevel, int dir);
void snap_toplevel_left(struct guibux_toplevel *toplevel);
void snap_toplevel_right(struct guibux_toplevel *toplevel);
void snap_toplevel_top(struct guibux_toplevel *toplevel);
void snap_toplevel_bottom(struct guibux_toplevel *toplevel);

/* effects.c */
void effects_init(struct guibux_server *server);
void effects_tick(struct guibux_server *server);
bool effects_active(struct guibux_server *server);
void effects_flush(struct guibux_server *server);
void effects_cancel_node(struct guibux_server *server, struct wlr_scene_node *node);
void effects_cancel_output(struct guibux_server *server, struct guibux_output *o);
/* the scene buffer showing the toplevel's own surface (for dest-size
 * scale animations); NULL before the first commit */
struct wlr_scene_buffer *toplevel_inner_buffer(struct guibux_toplevel *toplevel);
void effects_window_open(struct guibux_toplevel *toplevel);
void effects_window_open_start(struct guibux_toplevel *toplevel);
void effects_window_closed(struct guibux_toplevel *t, struct guibux_output *o);
void effects_retile(struct guibux_output *o);
void effects_notify_show(struct guibux_server *server, struct wlr_output *output);
/* true when the hide animation takes over freeing the panel node */
bool effects_notify_hide(struct guibux_server *server);

/* topbar.c */
void topbar_render(struct guibux_output *o);
void topbar_mark_dirty(struct guibux_output *o);
void topbar_win_remove(struct guibux_output *o, struct guibux_toplevel *toplevel);
bool topbar_workspace_at(struct guibux_server *server, double lx, double ly, struct guibux_output **output, int *ws);
int topbar_tick(void *data);
int topbar_test_run(void *data);
void topbar_seed_fake_toplevels(struct guibux_server *server);
int audio_test_run(void *data);
int battery_test_run(void *data);
int scroll_test_run(void *data);
int alt_drag_test_run(void *data);
int xmondrag_test_run(void *data);
int resize_test_run(void *data);
int guibux_text_width(FT_Face face, const char *text);
uint32_t utf8_next(const char **p);
void utf8_truncate(const char *src, char *dst, size_t dst_size, int max_cp);
int launcher_draw_text_on_surface(cairo_surface_t *cs, FT_Face face,
	const char *text, int x, int baseline, uint32_t color);
void set_color(cairo_t *cr, uint32_t c);
void topbar_rounded_rect(cairo_t *cr, double x, double y,
	double w, double h, double r);
bool topbar_notif_at(struct guibux_server *server, struct guibux_output *o, double lx, double ly);

/* tooltip.c */
void tooltip_hide(struct guibux_server *server);
bool tooltip_contains(struct guibux_server *server, double lx, double ly);
/* arm/disarm the hover from pointer motion; called on every move */
void tooltip_update_hover(struct guibux_server *server, uint32_t time);
/* show the armed tooltip once the hover delay has elapsed; called
 * from topbar_tick */
void tooltip_tick(struct guibux_server *server);
void tooltip_destroy(struct guibux_server *server);
int tooltip_test_run(void *data);

/* window-preview.c */
void preview_hide(struct guibux_server *server);
bool preview_contains(struct guibux_server *server, double lx, double ly);
/* arm/disarm the hover from pointer motion; called on every move */
void preview_update_hover(struct guibux_server *server, uint32_t time);
/* show the armed preview once the hover delay has elapsed; called
 * from topbar_tick */
void preview_tick(struct guibux_server *server);
/* hide the preview if it shows the given window (window unmap) */
void preview_on_unmap(struct guibux_server *server, struct guibux_toplevel *toplevel);
void preview_destroy(struct guibux_server *server);

/* osd.c */
void osd_show(struct guibux_server *server, enum guibux_osd_kind kind,
	int value, bool muted);
void osd_hide(struct guibux_server *server);
void osd_tick(struct guibux_server *server);
void osd_destroy(struct guibux_server *server);
int osd_test_run(void *data);

/* notify.c */
void notify_init(struct guibux_server *server);
void notify_destroy(struct guibux_server *server);
uint32_t notify_add(struct guibux_notify *n, const char *app_name,
	const char *summary, const char *body, int32_t expire);
void notify_close(struct guibux_notify *n, uint32_t id);
void notify_clear(struct guibux_notify *n);
bool notify_replace(struct guibux_notify *n, uint32_t id,
	const char *app_name, const char *summary, const char *body,
	int32_t expire);
int notify_count(struct guibux_notify *n);
int notify_snapshot(struct guibux_notify *n, struct guibux_notification *out, int max);
bool notify_get_by_id(struct guibux_notify *n, uint32_t id, struct guibux_notification *out);
bool notify_consume_dirty(struct guibux_notify *n);
int notify_indicator_width(FT_Face face, int scale, int count);
void notify_draw_indicator(cairo_surface_t *cs, cairo_t *cr, FT_Face face,
	struct guibux_launcher *launcher,
	int x, int baseline, int scale, int count, uint32_t color);
void notify_panel_show(struct guibux_server *server, struct wlr_output *output);
void notify_panel_hide(struct guibux_server *server);
/* free the panel node + buffer (also used when the panel's output is
 * destroyed and the slide-out cannot run) */
void notify_panel_free_node(struct guibux_server *server);
/* completion of the slide-out animation: frees the panel node */
void notify_panel_hide_done(void *data);
void notify_panel_render(struct guibux_server *server);
bool notify_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym);
uint32_t notify_panel_row_at(struct guibux_server *server, double lx, double ly);
bool notify_panel_clear_at(struct guibux_server *server, double lx, double ly);
bool notify_panel_contains(struct guibux_server *server, double lx, double ly);
/* auto-hide: (re)arm / cancel the close timer, its tick, and the main-loop
 * wake-up the D-Bus worker thread triggers for new notifications */
void notify_autohide_start(struct guibux_server *server);
void notify_autohide_cancel(struct guibux_server *server);
int notify_autohide_run(void *data);
int notify_new_readable(int fd, uint32_t mask, void *data);
int notify_test_run(void *data);

/* background.c */
void background_load_images(struct guibux_server *server);
void background_destroy_images(struct guibux_server *server);
void background_create(struct guibux_output *o);
void background_destroy(struct guibux_output *o);
void background_render(struct guibux_output *o);

/* launcher.c */
void launcher_init(struct guibux_server *server);
void launcher_show(struct guibux_server *server);
void launcher_hide(struct guibux_server *server);
bool launcher_handle_key(struct guibux_server *server, xkb_keysym_t sym);
int launcher_test_run(void *data);
void launcher_free_commands(struct guibux_launcher *l);
void launcher_rebuild_preferred(struct guibux_launcher *l);
/* rebuild the launcher icon theme search path (no-op when the theme
 * did not change); used by config reload for icon_theme */
void launcher_rebuild_icon_dirs(struct guibux_launcher *l);
void launcher_filter(struct guibux_launcher *l);
const char *resolve_icon(const char *icon_name);
/* draw a theme icon centered at (cx, cy) logical, size logical px,
 * scaled by scale; returns drawn width in logical px (0 = missing).
 * color is the fill for symbolic SVG icons (0 = use the file's own) */
int topbar_icon_draw(cairo_t *cr, struct guibux_launcher *l,
	const char *name, int cx, int cy, int size, int scale,
	uint32_t color);
void launcher_free_icons(struct guibux_launcher *l);

/* switcher.c */
void switcher_show(struct guibux_server *server);
void switcher_hide(struct guibux_server *server);
bool switcher_handle_key(struct guibux_server *server, xkb_keysym_t sym);
void switcher_on_modifier_release(struct guibux_server *server, uint32_t modifiers);
void switcher_on_unmap(struct guibux_server *server, struct guibux_toplevel *toplevel);

/* overview.c */
void overview_show(struct guibux_server *server);
void overview_hide(struct guibux_server *server);
bool overview_handle_key(struct guibux_server *server, xkb_keysym_t sym);
struct guibux_toplevel *overview_window_at(struct guibux_server *server, double lx, double ly);
void overview_click_empty(struct guibux_server *server, double lx, double ly);
void overview_button_release(struct guibux_server *server);
void overview_update_hover(struct guibux_server *server);

/* help.c */
void help_show(struct guibux_server *server);
void help_hide(struct guibux_server *server);
bool help_handle_key(struct guibux_server *server, xkb_keysym_t sym);

/* outputs-panel.c */
void outputs_panel_show(struct guibux_server *server);
void outputs_panel_hide(struct guibux_server *server);
bool outputs_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym);

/* power.c */
void power_panel_show(struct guibux_server *server);
void power_panel_hide(struct guibux_server *server);
bool power_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym);
/* returns the action index under (lx, ly), -1 when outside the panel */
int power_panel_action_at(struct guibux_server *server, double lx, double ly);
/* run the action (hides the panel first) */
void power_panel_select(struct guibux_server *server, int idx);
void power_panel_destroy(struct guibux_server *server);
int power_panel_test_run(void *data);

/* topbar-items.c */
void topbar_items_panel_show(struct guibux_server *server);
void topbar_items_panel_hide(struct guibux_server *server);
bool topbar_items_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym);
/* returns the item index under (lx, ly), -1 when outside the panel */
int topbar_items_panel_row_at(struct guibux_server *server, double lx, double ly);
/* toggle the item at idx (live: re-renders all topbars + the panel) */
void topbar_items_panel_toggle(struct guibux_server *server, int idx);
void topbar_items_panel_destroy(struct guibux_server *server);
int topbar_items_panel_test_run(void *data);

/* keyboard.c */
void server_new_input(struct wl_listener *listener, void *data);
void server_new_keyboard(struct guibux_server *server, struct wlr_input_device *device);
void keyboard_handle_modifiers(struct wl_listener *listener, void *data);
void keyboard_handle_key(struct wl_listener *listener, void *data);
void keyboard_handle_destroy(struct wl_listener *listener, void *data);
bool handle_keybinding(struct guibux_server *server, xkb_keysym_t sym, uint32_t modifiers);
void do_action(struct guibux_server *server, enum guibux_action action, int arg, struct guibux_toplevel *toplevel);
void keybinds_defaults(struct guibux_server *server);
void keybinds_reset(struct guibux_server *server);
void keybind_add(struct guibux_server *server, uint32_t modifiers, xkb_keysym_t keysym, enum guibux_action action, int arg);
void spawn_terminal(struct guibux_server *server);
void spawn_network_info(struct guibux_server *server);
void spawn_mixer(struct guibux_server *server);
void setup_session_environment(struct guibux_server *server);
void volume_change(struct guibux_server *server, bool mic, int delta_pct);
void volume_flush(struct guibux_server *server);
void volume_toggle_mute(struct guibux_server *server, bool mic);
void spawn_sigchld_handler(int sig);
void spawn_track(pid_t pid);

/* config.c */
void load_config(struct guibux_server *server, const char *path);
/* re-read the config file live: keybinds are reset+re-added, colors and
 * topbar settings apply on the next render, backgrounds are reloaded
 * when changed, the outputs spec is re-applied. renderer and xkb_* are
 * not reloadable (logged as a warning) */
void config_reload(struct guibux_server *server);
/* SIGHUP event source callback: re-reads the config file live */
int config_signal_readable(int fd, uint32_t mask, void *data);
bool parse_keybind(struct guibux_server *server, const char *value);
bool parse_color(const char *value, uint32_t *out);
/* spec: config `outputs` value or GUIBUX_OUTPUTS env; NULL/empty/"auto"
 * = arrange every connected output automatically. Entry:
 * NAME@XxY[:WxH[:ROT]] or NAME@off */
void parse_output_placements(struct guibux_server *server, const char *spec);
/* parse into a caller array (does not touch the server) */
void parse_output_placements_to(struct output_placement *arr, int cap,
	const char *spec, int *num);
/* re-read the `outputs` value from a config file; malloc'd, NULL if absent */
char *config_read_outputs_line(const char *path);
/* window-restore.c */
/* XDG state file path: $XDG_STATE_HOME/guibuxwm/<file> */
void guibux_state_path(char *path, size_t path_size, const char *file);
/* create the directory chain of a state file path */
void guibux_state_mkdir(const char *path);
void restore_derive_terminal_id(struct guibux_server *server);
void restore_load(struct guibux_server *server);
void restore_save(struct guibux_server *server, struct guibux_toplevel *toplevel);
void restore_save_all(struct guibux_server *server);
enum restore_result restore_apply(struct guibux_server *server, struct guibux_toplevel *toplevel);
void restore_free(struct guibux_server *server);

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
int outputs_test_run(void *data);
int outputs_panel_test_run(void *data);
int tile_test_run(void *data);
int overview_test_run(void *data);
int keybind_test_run(void *data);
int psel_test_run(void *data);
int effects_test_run(void *data);
int quit_test_run(void *data);
int global_topbar_test_run(void *data);

/* screensaver.c */
void screensaver_init(struct guibux_server *server);
void screensaver_destroy(struct guibux_server *server);
void screensaver_set_timeout(struct guibux_screensaver *ss, int seconds);
void screensaver_notify_activity(struct guibux_server *server);
void screensaver_turn_off(struct guibux_server *server);
void screensaver_turn_on(struct guibux_server *server);
void screensaver_update_inhibited(struct guibux_server *server);

#endif /* GUIBUXWM_H */
