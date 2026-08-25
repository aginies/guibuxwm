#include "guibuxwm.h"
#include <strings.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

bool parse_color(const char *value, uint32_t *out) {
	uint32_t c;
	if (sscanf(value, "#%x", &c) != 1 || c > 0xFFFFFF) {
		return false;
	}
	*out = c;
	return true;
}

static int topbar_item_by_name(const char *name) {
	if (!strcmp(name, "network")) return TOPBAR_ITEM_NETWORK;
	if (!strcmp(name, "volume")) return TOPBAR_ITEM_VOLUME;
	if (!strcmp(name, "mic")) return TOPBAR_ITEM_MIC;
	if (!strcmp(name, "battery")) return TOPBAR_ITEM_BATTERY;
	if (!strcmp(name, "notifications")) return TOPBAR_ITEM_NOTIFICATIONS;
	if (!strcmp(name, "clock")) return TOPBAR_ITEM_CLOCK;
	return -1;
}

static void parse_topbar_items(struct guibux_server *server,
		const char *value, const char *path, int lineno) {
	char *copy = strdup(value);
	if (copy == NULL) {
		return;
	}
	server->topbar_item_count = 0;
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
			tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ' || *tok == '\t') {
			tok++;
		}
		char *end = tok + strlen(tok);
		while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
			*--end = '\0';
		}
		if (*tok == '\0') {
			continue;
		}
		int id = topbar_item_by_name(tok);
		if (id < 0) {
			wlr_log(WLR_ERROR, "config: %s:%d: unknown topbar item '%s'",
				path, lineno, tok);
			continue;
		}
		bool dup = false;
		for (int i = 0; i < server->topbar_item_count; i++) {
			if (server->topbar_items[i] == id) {
				dup = true;
				break;
			}
		}
		if (dup) {
			continue;
		}
		if (server->topbar_item_count >= TOPBAR_ITEMS_MAX) {
			wlr_log(WLR_ERROR, "config: %s:%d: too many topbar items, '%s' dropped",
				path, lineno, tok);
			break;
		}
		server->topbar_items[server->topbar_item_count++] = id;
	}
	free(copy);
	if (server->topbar_item_count == 0) {
		/* empty or all-invalid list: fall back to the full default set */
		server->topbar_items[0] = TOPBAR_ITEM_NETWORK;
		server->topbar_items[1] = TOPBAR_ITEM_VOLUME;
		server->topbar_items[2] = TOPBAR_ITEM_MIC;
		server->topbar_items[3] = TOPBAR_ITEM_BATTERY;
		server->topbar_items[4] = TOPBAR_ITEM_NOTIFICATIONS;
		server->topbar_items[5] = TOPBAR_ITEM_CLOCK;
		server->topbar_item_count = TOPBAR_ITEMS_MAX;
	}
}

bool parse_keybind(struct guibux_server *server, const char *value) {
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
	} else if (!strcmp(action_str, "switch-ws-left")) {
		action = GUIBUX_ACT_SWITCH_WS_LEFT;
	} else if (!strcmp(action_str, "switch-ws-right")) {
		action = GUIBUX_ACT_SWITCH_WS_RIGHT;
	} else if (!strcmp(action_str, "snap-left")) {
		action = GUIBUX_ACT_SNAP_LEFT;
	} else if (!strcmp(action_str, "snap-right")) {
		action = GUIBUX_ACT_SNAP_RIGHT;
	} else if (!strcmp(action_str, "snap-top")) {
		action = GUIBUX_ACT_SNAP_TOP;
	} else if (!strcmp(action_str, "snap-bottom")) {
		action = GUIBUX_ACT_SNAP_BOTTOM;
	} else if (!strcmp(action_str, "show-help")) {
		action = GUIBUX_ACT_SHOW_HELP;
	} else if (!strcmp(action_str, "volume-up")) {
		action = GUIBUX_ACT_VOLUME_UP;
	} else if (!strcmp(action_str, "volume-down")) {
		action = GUIBUX_ACT_VOLUME_DOWN;
	} else if (!strcmp(action_str, "mute")) {
		action = GUIBUX_ACT_MUTE;
	} else if (!strcmp(action_str, "mic-up")) {
		action = GUIBUX_ACT_MIC_UP;
	} else if (!strcmp(action_str, "mic-down")) {
		action = GUIBUX_ACT_MIC_DOWN;
	} else if (!strcmp(action_str, "mic-mute")) {
		action = GUIBUX_ACT_MIC_MUTE;
	} else if (!strcmp(action_str, "brightness-up")) {
		action = GUIBUX_ACT_BRIGHTNESS_UP;
	} else if (!strcmp(action_str, "brightness-down")) {
		action = GUIBUX_ACT_BRIGHTNESS_DOWN;
	} else if (!strcmp(action_str, "outputs-apply")) {
		action = GUIBUX_ACT_OUTPUTS_APPLY;
	} else if (!strcmp(action_str, "outputs-panel")) {
		action = GUIBUX_ACT_OUTPUTS_PANEL;
	} else if (!strcmp(action_str, "power")) {
		action = GUIBUX_ACT_POWER;
	} else if (!strcmp(action_str, "reload-config")) {
		action = GUIBUX_ACT_RELOAD_CONFIG;
	} else if (!strcmp(action_str, "topbar-items")) {
		action = GUIBUX_ACT_TOPBAR_ITEMS;
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

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = '\0';
	}
	return s;
}

// preferred_appN = Name;command : split at the first ';', both
// parts must be non-empty after trimming
static void parse_preferred_app(struct guibux_server *server, int idx,
		const char *val, const char *icon_val, const char *path, int lineno) {
	char *copy = strdup(val);
	if (copy == NULL) {
		return;
	}
	char *semi = strchr(copy, ';');
	if (semi == NULL) {
		wlr_log(WLR_ERROR, "config: %s:%d: bad preferred_app%d '%s' (expected 'Name;command')",
			path, lineno, idx + 1, val);
		free(copy);
		return;
	}
	*semi = '\0';
	char *name = trim(copy);
	char *exec = trim(semi + 1);
	char *icon_raw = NULL;
	if (icon_val && icon_val[0] != '\0') {
		icon_raw = strdup(icon_val);
	} else if (exec && exec[0] != '\0' && strchr(exec, ' ') == NULL) {
		/* no explicit icon: use the exec name as icon name;
		 * resolved to a theme path in launcher_init */
		icon_raw = strdup(exec);
	}
	if (name[0] == '\0' || exec[0] == '\0') {
		wlr_log(WLR_ERROR, "config: %s:%d: bad preferred_app%d '%s' (empty name or command)",
			path, lineno, idx + 1, val);
		free(copy);
		free(icon_raw);
		return;
	}
	struct guibux_launcher *l = &server->launcher;
	free(l->preferred[idx].name);
	free(l->preferred[idx].exec);
	l->preferred[idx].name = strdup(name);
	l->preferred[idx].exec = strdup(exec);
	snprintf(l->preferred[idx].icon_path,
		sizeof(l->preferred[idx].icon_path), "%s", icon_raw ? icon_raw : "");
	if (idx + 1 > l->num_preferred) {
		l->num_preferred = idx + 1;
	}
	wlr_log(WLR_INFO, "config: preferred_app%d = %s;%s", idx + 1, name, exec);
	free(copy);
	free(icon_raw);
}

void load_config(struct guibux_server *server, const char *path) {
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
		} else if (!strcmp(key, "term_app_id")) {
			free(server->term_app_id);
			server->term_app_id = strdup(val);
			wlr_log(WLR_INFO, "config: term_app_id = %s", val);
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
		} else if (!strcmp(key, "topbar_height")) {
			server->topbar_height = atoi(val);
			/* an absurd height would push the fullscreen height to
			 * zero or below and crash the size assert */
			if (server->topbar_height <= 0 || server->topbar_height > 200) {
				wlr_log(WLR_ERROR, "config: %s:%d: topbar_height %d out of range 1..200, using default",
					path, lineno, server->topbar_height);
				server->topbar_height = DEFAULT_TOPBAR_H;
			}
			wlr_log(WLR_INFO, "config: topbar_height = %d", server->topbar_height);
		} else if (!strcmp(key, "topbar_font_size")) {
			server->topbar_font_size = atoi(val);
			if (server->topbar_font_size <= 0) {
				server->topbar_font_size = DEFAULT_TOPBAR_FONT_SIZE;
			}
			wlr_log(WLR_INFO, "config: topbar_font_size = %d", server->topbar_font_size);
		} else if (!strcmp(key, "topbar_win_pad")) {
			server->topbar_win_pad = atoi(val);
			if (server->topbar_win_pad < 0) {
				server->topbar_win_pad = DEFAULT_TOPBAR_WIN_PAD;
			}
			wlr_log(WLR_INFO, "config: topbar_win_pad = %d", server->topbar_win_pad);
		} else if (!strcmp(key, "topbar_items")) {
			parse_topbar_items(server, val, path, lineno);
			wlr_log(WLR_INFO, "config: topbar_items = %s", val);
		} else if (!strcmp(key, "background")) {
			free(server->background_path);
			server->background_path = strdup(val);
			wlr_log(WLR_INFO, "config: background = %s", val);
		} else if (!strncmp(key, "background", 10) &&
				key[10] >= '1' && key[10] <= '0' + NUM_WORKSPACES &&
				key[11] == '\0') {
			int ws = key[10] - '0';
			free(server->bg_paths[ws - 1]);
			server->bg_paths[ws - 1] = strdup(val);
			wlr_log(WLR_INFO, "config: background%d = %s", ws, val);
		} else if (!strcmp(key, "background_scale")) {
			if (!strcmp(val, "stretch")) {
				server->background_scale = BG_STRETCH;
			} else if (!strcmp(val, "fit")) {
				server->background_scale = BG_FIT;
			} else if (!strcmp(val, "fill")) {
				server->background_scale = BG_FILL;
			} else if (!strcmp(val, "tile")) {
				server->background_scale = BG_TILE;
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad background_scale '%s' (expected stretch|fit|fill|tile)", path, lineno, val);
			}
			wlr_log(WLR_INFO, "config: background_scale = %s", val);
		} else if (!strcmp(key, "outputs")) {
			free(server->outputs_spec);
			server->outputs_spec = strdup(val);
			wlr_log(WLR_INFO, "config: outputs = %s", val);
		} else if (!strcmp(key, "screensaver_timeout")) {
			screensaver_set_timeout(&server->screensaver, atoi(val));
			wlr_log(WLR_INFO, "config: screensaver_timeout = %d", server->screensaver.timeout);
		} else if (!strcmp(key, "focus_follow_mouse")) {
			server->focus_follow_mouse = !strcmp(val, "true");
			wlr_log(WLR_INFO, "config: focus_follow_mouse = %s", val);
		} else if (!strcmp(key, "effects")) {
			server->effects_enabled = !strcmp(val, "true");
			wlr_log(WLR_INFO, "config: effects = %s", val);
		} else if (!strcmp(key, "restore_positions")) {
			server->restore_positions = !strcmp(val, "true");
			wlr_log(WLR_INFO, "config: restore_positions = %s", val);
		} else if (!strcmp(key, "effects_duration_ms")) {
			server->effects_duration_ms = atoi(val);
			if (server->effects_duration_ms < 0 ||
					server->effects_duration_ms > 1000) {
				wlr_log(WLR_ERROR, "config: %s:%d: effects_duration_ms %d out of range 0..1000, using default",
					path, lineno, server->effects_duration_ms);
				server->effects_duration_ms = 200;
			}
			wlr_log(WLR_INFO, "config: effects_duration_ms = %d", server->effects_duration_ms);
		} else if (!strcmp(key, "window_open_effect")) {
			if (!strcmp(val, "scale")) {
				server->window_open_effect = OPEN_EFFECT_SCALE;
			} else if (!strcmp(val, "slide")) {
				server->window_open_effect = OPEN_EFFECT_SLIDE;
			} else if (!strcmp(val, "none")) {
				server->window_open_effect = OPEN_EFFECT_NONE;
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad window_open_effect '%s' (expected scale|slide|none)", path, lineno, val);
			}
			wlr_log(WLR_INFO, "config: window_open_effect = %s", val);
		} else if (!strcmp(key, "notify_effect")) {
			if (!strcmp(val, "slide")) {
				server->notify_effect_slide = true;
			} else if (!strcmp(val, "none")) {
				server->notify_effect_slide = false;
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad notify_effect '%s' (expected slide|none)", path, lineno, val);
			}
			wlr_log(WLR_INFO, "config: notify_effect = %s", val);
		} else if (!strcmp(key, "osd")) {
			server->osd_enabled = !strcmp(val, "true");
			wlr_log(WLR_INFO, "config: osd = %s", val);
		} else if (!strcmp(key, "osd_timeout_ms")) {
			server->osd_timeout_ms = atoi(val);
			if (server->osd_timeout_ms < 0 ||
					server->osd_timeout_ms > 10000) {
				wlr_log(WLR_ERROR, "config: %s:%d: osd_timeout_ms %d out of range 0..10000, using default",
					path, lineno, server->osd_timeout_ms);
				server->osd_timeout_ms = 1500;
			}
			wlr_log(WLR_INFO, "config: osd_timeout_ms = %d", server->osd_timeout_ms);
		} else if (!strcmp(key, "renderer")) {
			if (strcmp(val, "auto") != 0 && strcmp(val, "gles2") != 0 &&
					strcmp(val, "vulkan") != 0 && strcmp(val, "pixman") != 0) {
				wlr_log(WLR_ERROR, "config: %s:%d: bad renderer '%s' (expected auto|gles2|vulkan|pixman), using auto", path, lineno, val);
			} else {
				free(server->renderer_name);
				server->renderer_name = strdup(val);
			}
			wlr_log(WLR_INFO, "config: renderer = %s", server->renderer_name);
		} else if (!strcmp(key, "overview_workspace_colors")) {
			server->overview.ws_colors_enabled = !strcmp(val, "true");
			wlr_log(WLR_INFO, "config: overview_workspace_colors = %s", val);
		} else if (!strncmp(key, "overview_ws_color", 17) &&
				key[17] >= '1' && key[17] <= '0' + NUM_WORKSPACES &&
				key[18] == '\0') {
			int ws = key[17] - '0';
			if (parse_color(val, &server->overview.ws_colors[ws - 1])) {
				wlr_log(WLR_INFO, "config: overview_ws_color%d = %s", ws, val);
			} else {
				wlr_log(WLR_ERROR, "config: %s:%d: bad color '%s' (expected #rrggbb)", path, lineno, val);
			}
		} else if (!strcmp(key, "icon_theme")) {
			snprintf(server->launcher.icon_theme,
				sizeof(server->launcher.icon_theme), "%s", val);
			wlr_log(WLR_INFO, "config: icon_theme = %s", val);
		} else if (!strncmp(key, "preferred_app", 13) &&
				key[13] >= '1' && key[13] <= '0' + LAUNCHER_MAX_PREFERRED &&
				key[14] == '\0') {
			char *copy = strdup(val);
			char *icon_val = NULL;
			if (copy) {
				char *last_semi = strrchr(copy, ';');
				if (last_semi) {
					*last_semi = '\0';
					icon_val = trim(last_semi + 1);
					if (icon_val[0] == '\0') icon_val = NULL;
				}
			}
			parse_preferred_app(server, key[13] - '1', copy ? copy : val,
				icon_val, path, lineno);
			free(copy);
		} else {
			wlr_log(WLR_ERROR, "config: %s:%d: unknown key '%s'", path, lineno, key);
		}
	}
	fclose(f);
}

void config_reload(struct guibux_server *server) {
	if (server->config_path == NULL) {
		wlr_log(WLR_ERROR, "config: no config file to reload");
		return;
	}
	wlr_log(WLR_INFO, "config: reloading %s", server->config_path);

	/* snapshot the values that need explicit re-application when they
	 * change (string compare for paths, enum for scale) */
	char old_bg[512] = "";
	if (server->background_path != NULL) {
		snprintf(old_bg, sizeof(old_bg), "%s", server->background_path);
	}
	char old_bg_ws[NUM_WORKSPACES][512];
	for (int i = 0; i < NUM_WORKSPACES; i++) {
		old_bg_ws[i][0] = '\0';
		if (server->bg_paths[i] != NULL) {
			snprintf(old_bg_ws[i], sizeof(old_bg_ws[i]), "%s",
				server->bg_paths[i]);
		}
	}
	enum guibux_bg_scale old_scale = server->background_scale;
	int old_ss_timeout = server->screensaver.timeout;
	char *old_outputs = server->outputs_spec;
	bool old_outputs_present = old_outputs != NULL;

	/* keybinds: reset, re-add defaults, then the file's keybinds
	 * (a config keybind with the same mods+key replaces the default,
	 * same as at startup) */
	keybinds_reset(server);
	keybinds_defaults(server);
	load_config(server, server->config_path);

	/* backgrounds: reload the images when any path or the scale
	 * changed; the per-output bg nodes pick the new surfaces on their
	 * next render */
	bool bg_changed = strcmp(old_bg,
			server->background_path != NULL ? server->background_path : "") != 0
		|| server->background_scale != old_scale;
	for (int i = 0; i < NUM_WORKSPACES; i++) {
		const char *cur = server->bg_paths[i] != NULL
			? server->bg_paths[i] : "";
		if (strcmp(old_bg_ws[i], cur) != 0) {
			bg_changed = true;
		}
	}
	if (bg_changed) {
		background_destroy_images(server);
		background_load_images(server);
	}

	/* screensaver timeout: re-arm the timer when it changed */
	if (server->screensaver.timeout != old_ss_timeout) {
		screensaver_set_timeout(&server->screensaver,
			server->screensaver.timeout);
	}

	/* outputs: re-apply when the file has an `outputs` line (the
	 * outputs panel and the guibuxwm-output tool edit it live) */
	if (old_outputs_present || server->outputs_spec != NULL) {
		outputs_apply(server);
	}

	/* launcher: rebuild the icon theme dirs when icon_theme changed and
	 * re-resolve the preferred apps' icons (preferred_appN is reloadable
	 * too) */
	launcher_rebuild_icon_dirs(&server->launcher);
	launcher_rebuild_preferred(&server->launcher);
	/* colors, topbar height/font/pad: picked up on the next render;
	 * mark every bar dirty so the change shows without other activity */
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		o->topbar_dirty = true;
	}

	/* not reloadable: the renderer is already created and the
	 * keyboards are already configured with the old xkb settings */
	wlr_log(WLR_INFO,
		"config: reload done (renderer and xkb_* require a restart)");
}

int config_signal_readable(int fd, uint32_t mask, void *data) {
	struct guibux_server *server = data;
	struct signalfd_siginfo info;
	while (read(fd, &info, sizeof(info)) == (ssize_t)sizeof(info)) {
		if (info.ssi_signo == SIGHUP) {
			wlr_log(WLR_INFO, "config: SIGHUP, reloading config");
			config_reload(server);
		}
	}
	return 1;
}

/* Re-read just the `outputs` value from a config file (live re-apply:
 * the guibuxwm-output tool edits the file while the compositor runs).
 * Returns a malloc'd string, or NULL when the file or key is missing. */
char *config_read_outputs_line(const char *path) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return NULL;
	}
	char line[512];
	char *result = NULL;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0' || *p == '#' || *p == '\n') {
			continue;
		}
		char *eq = strchr(p, '=');
		if (eq == NULL) {
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
		if (!strcmp(key, "outputs")) {
			result = strdup(val);
			break;
		}
	}
	fclose(f);
	return result;
}

void parse_output_placements_to(struct output_placement *arr, int cap,
		const char *spec, int *num) {
	*num = 0;
	if (spec == NULL || spec[0] == '\0' || !strcmp(spec, "auto")) {
		return;
	}
	char *copy = strdup(spec);
	if (copy == NULL) {
		return;
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
			tok = strtok_r(NULL, ",", &save)) {
		char *at = strchr(tok, '@');
		if (at == NULL) {
			wlr_log(WLR_ERROR, "outputs: bad entry '%s' (expected NAME@XxY[:WxH[:ROT]] or NAME@off)", tok);
			continue;
		}
		*at = '\0';
		char *pos = at + 1;
		bool off = strcasecmp(pos, "off") == 0;
		int x = 0, y = 0;
		if (!off && sscanf(pos, "%dx%d", &x, &y) != 2) {
			wlr_log(WLR_ERROR, "outputs: bad position in '%s'", tok);
			continue;
		}
		/* sections after the position: WxH (mode) and normal|90|180|270
		 * (rotation), in that order */
		int transform = -1;
		int mode_w = 0, mode_h = 0;
		bool bad = false;
		char *sec = strchr(pos, ':');
		while (sec != NULL) {
			sec[0] = '\0';
			sec++;
			int w = 0, h = 0;
			if (sscanf(sec, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				mode_w = w;
				mode_h = h;
			} else if (!strcmp(sec, "normal")) {
				transform = WL_OUTPUT_TRANSFORM_NORMAL;
			} else if (!strcmp(sec, "90")) {
				transform = WL_OUTPUT_TRANSFORM_90;
			} else if (!strcmp(sec, "180")) {
				transform = WL_OUTPUT_TRANSFORM_180;
			} else if (!strcmp(sec, "270")) {
				transform = WL_OUTPUT_TRANSFORM_270;
			} else {
				wlr_log(WLR_ERROR, "outputs: bad section '%s' (expected WxH or normal|90|180|270)", sec);
				bad = true;
				break;
			}
			sec = strchr(sec, ':');
		}
		if (bad) {
			continue;
		}
		if (*num >= cap) {
			wlr_log(WLR_ERROR, "outputs: too many outputs (max %d)", cap);
			break;
		}
		if (strlen(tok) >= sizeof(arr[0].name)) {
			wlr_log(WLR_ERROR, "outputs: output name too long (max %zu chars) in '%s'",
				sizeof(arr[0].name) - 1, tok);
			continue;
		}
		struct output_placement *p = &arr[(*num)++];
		snprintf(p->name, sizeof(p->name), "%s", tok);
		p->x = x;
		p->y = y;
		p->transform = transform;
		p->mode_w = mode_w;
		p->mode_h = mode_h;
		p->disabled = off;
		if (off) {
			wlr_log(WLR_INFO, "outputs: %s disabled", p->name);
		} else {
			wlr_log(WLR_INFO, "outputs: %s at %dx%d mode %dx%d transform %d",
				p->name, x, y, mode_w, mode_h, transform);
		}
	}
	free(copy);
}

void parse_output_placements(struct guibux_server *server, const char *spec) {
	parse_output_placements_to(server->placements, MAX_OUTPUT_PLACEMENTS,
		spec, &server->num_placements);
}
