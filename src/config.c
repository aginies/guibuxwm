#include "guibuxwm.h"

bool parse_color(const char *value, uint32_t *out) {
	uint32_t c;
	if (sscanf(value, "#%x", &c) != 1 || c > 0xFFFFFF) {
		return false;
	}
	*out = c;
	return true;
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

void parse_output_placements(struct guibux_server *server) {
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
		if (strlen(tok) >= sizeof(server->placements[0].name)) {
			wlr_log(WLR_ERROR, "GUIBUX_OUTPUTS: output name too long (max %zu chars) in '%s'",
				sizeof(server->placements[0].name) - 1, tok);
			continue;
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
