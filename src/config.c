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
