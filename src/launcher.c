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
#define STBI_NO_GIF
#define STBI_NO_BMP
#define STBI_NO_HDR
#define STBI_NO_PSD
#include "stb_image.h"
#ifdef GUIBUX_HAS_RSVG
#include <librsvg-2.0/librsvg/rsvg.h>
#endif

#define LAUNCHER_ICON_SIZE 24
#define LAUNCHER_ICON_PAD 8
#define ICON_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ICON_MAX_DIRS 8

static char icon_dirs[ICON_MAX_DIRS][PATH_MAX];
static int num_icon_dirs;
static char icon_dirs_current_theme[64];  /* theme the dirs were built for */

/* name -> resolved path cache: the icon set is tiny (a few dozen names)
 * and resolve_icon used to cost up to ~150 access() syscalls per call,
 * 3-4x per icon per topbar render. Paths live in this static table so
 * callers get a const pointer with no malloc. Cleared when the theme
 * dirs are rebuilt (icon_theme config reload) */
#define ICON_RESOLVE_CACHE_SIZE 128
struct icon_resolve_entry {
	char name[64];
	char path[PATH_MAX + 64];
	bool miss;
};
static struct icon_resolve_entry icon_resolve_cache[ICON_RESOLVE_CACHE_SIZE];
static int icon_resolve_cache_count;

static void icon_resolve_cache_clear(void) {
	icon_resolve_cache_count = 0;
}

static const char *icon_sizes[] = {
	"24x24",
	"16x16",
	"scalable",
};

static const char *icon_contexts[] = {
	"apps",
	"mimetypes",
};

/* symbolic icon subdirectories (Adwaita ships status/device icons
 * as SVG only, no PNG variants) */
static const char *icon_symbolic_dirs[] = {
	"symbolic/status",
	"symbolic/devices",
	"symbolic/places",
	"symbolic/apps",
};

static void icon_add_dir(const char *dir) {
	for (int i = 0; i < num_icon_dirs; i++) {
		if (strcmp(icon_dirs[i], dir) == 0)
			return;
	}
	if (num_icon_dirs >= ICON_MAX_DIRS)
		return;
	snprintf(icon_dirs[num_icon_dirs], PATH_MAX, "%s", dir);
	num_icon_dirs++;
}

/* read gtk-icon-theme-name from the user's gtk settings.ini */
static void icon_detect_gtk_theme(char *out, size_t n) {
	const char *home = getenv("HOME");
	if (!home)
		return;
	const char *rel[2] = {
		".config/gtk-3.0/settings.ini",
		".config/gtk-4.0/settings.ini",
	};
	for (size_t i = 0; i < 2; i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", home, rel[i]);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = '\0';
			char *key = line;
			while (*key == ' ' || *key == '\t')
				key++;
			char *kend = eq - 1;
			while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t'))
				kend--;
			*(kend + 1) = '\0';
			if (strcmp(key, "gtk-icon-theme-name") != 0)
				continue;
			char *val = eq + 1;
			while (*val == ' ' || *val == '\t')
				val++;
			size_t len = strlen(val);
			while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r' ||
					val[len-1] == ' ' || val[len-1] == '\t'))
				val[--len] = '\0';
			if (val[0]) {
				snprintf(out, n, "%s", val);
				fclose(f);
				return;
			}
		}
		fclose(f);
	}
}

/* build the icon theme search path: user theme dir, XDG_DATA_DIRS
 * theme dirs, then Adwaita and hicolor as fallbacks */
static void icon_build_dirs(struct guibux_launcher *l) {
	char theme[64] = "Adwaita";
	if (l->icon_theme[0] != '\0') {
		snprintf(theme, sizeof(theme), "%s", l->icon_theme);
	} else {
		icon_detect_gtk_theme(theme, sizeof(theme));
	}
	/* nothing changed since the last build: keep the dirs (and the
	 * cached icons, which still point into them) */
	if (strcmp(theme, icon_dirs_current_theme) == 0) {
		return;
	}
	snprintf(icon_dirs_current_theme, sizeof(icon_dirs_current_theme),
		"%s", theme);
	num_icon_dirs = 0;
	icon_resolve_cache_clear();
	char dir[PATH_MAX];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(dir, sizeof(dir), "%s/.local/share/icons/%s", home, theme);
		icon_add_dir(dir);
	}
	const char *xdg = getenv("XDG_DATA_DIRS");
	if (xdg == NULL || xdg[0] == '\0')
		xdg = "/usr/local/share:/usr/share";
	char *xdg_copy = strdup(xdg);
	char *save = NULL;
	for (char *p = strtok_r(xdg_copy, ":", &save); p != NULL;
			p = strtok_r(NULL, ":", &save)) {
		if (*p == '\0')
			continue;
		snprintf(dir, sizeof(dir), "%s/share/icons/%s", p, theme);
		icon_add_dir(dir);
	}
	free(xdg_copy);
	icon_add_dir("/usr/share/icons/Adwaita");
	icon_add_dir("/usr/share/icons/hicolor");
	wlr_log(WLR_INFO, "launcher: icon theme '%s' (%d dirs)",
		theme, num_icon_dirs);
}

const char *resolve_icon(const char *icon_name) {
	if (!icon_name || icon_name[0] == '\0')
		return NULL;

	for (int i = 0; i < icon_resolve_cache_count; i++) {
		if (strcmp(icon_resolve_cache[i].name, icon_name) == 0) {
			return icon_resolve_cache[i].miss ? NULL :
				icon_resolve_cache[i].path;
		}
	}

	char path[PATH_MAX + 64];
	const char *found = NULL;
	for (int t = 0; t < num_icon_dirs; t++) {
		for (size_t s = 0; s < ICON_ARRAY_LEN(icon_sizes); s++) {
			for (size_t c = 0; c < ICON_ARRAY_LEN(icon_contexts); c++) {
				snprintf(path, sizeof(path), "%s/%s/%s/%s.png",
					icon_dirs[t], icon_sizes[s],
					icon_contexts[c], icon_name);
				if (access(path, F_OK) == 0) {
					found = path;
					break;
				}
			}
			if (found)
				break;
		}
		if (found)
			break;
		/* symbolic SVG icons (Adwaita status/device indicators) */
		for (size_t s = 0; s < ICON_ARRAY_LEN(icon_symbolic_dirs); s++) {
			snprintf(path, sizeof(path), "%s/%s/%s-symbolic.svg",
				icon_dirs[t], icon_symbolic_dirs[s], icon_name);
			if (access(path, F_OK) == 0) {
				found = path;
				break;
			}
		}
		if (found)
			break;
	}

	/* always cache the result: the returned pointer must stay valid
	 * (callers hold it across the render); evict the oldest entry when
	 * the table is full */
	struct icon_resolve_entry *e;
	if (icon_resolve_cache_count < ICON_RESOLVE_CACHE_SIZE) {
		e = &icon_resolve_cache[icon_resolve_cache_count++];
	} else {
		e = &icon_resolve_cache[0];
		memmove(&icon_resolve_cache[0], &icon_resolve_cache[1],
			(ICON_RESOLVE_CACHE_SIZE - 1) * sizeof(icon_resolve_cache[0]));
	}
	snprintf(e->name, sizeof(e->name), "%s", icon_name);
	if (found) {
		snprintf(e->path, sizeof(e->path), "%s", found);
		wlr_log(WLR_INFO, "launcher: icon '%s' -> %s",
			icon_name, found);
		return e->path;
	}
	e->miss = true;
	wlr_log(WLR_DEBUG, "launcher: icon '%s' not found", icon_name);
	return NULL;
}

static uint8_t *load_icon(const char *path, int *out_w, int *out_h) {
	if (!path || access(path, F_OK) != 0)
		return NULL;
	uint8_t *data = stbi_load(path, out_w, out_h, NULL, 4);
	if (!data)
		return NULL;
	/* cairo ARGB32 is premultiplied BGRA in memory on little-endian;
	 * stb returns straight RGBA: swap R/B and premultiply */
	int n = *out_w * *out_h;
	for (int i = 0; i < n; i++) {
		uint8_t a = data[i * 4 + 3];
		uint8_t r = data[i * 4 + 0], b = data[i * 4 + 2];
		data[i * 4 + 0] = (uint8_t)((b * a + 127) / 255);
		data[i * 4 + 1] = (uint8_t)((data[i * 4 + 1] * a + 127) / 255);
		data[i * 4 + 2] = (uint8_t)((r * a + 127) / 255);
	}
	return data;
}

static uint8_t *get_cached_icon(struct guibux_launcher *l,
		const char *path, int *out_w, int *out_h) {
	for (int i = 0; i < l->num_icons; i++) {
		if (strcmp(l->icon_cache[i].path, path) == 0) {
			*out_w = l->icon_cache[i].w;
			*out_h = l->icon_cache[i].h;
			return l->icon_cache[i].data;
		}
	}
	if (l->num_icons >= (int)ICON_ARRAY_LEN(l->icon_cache)) {
		/* cache full: evict the oldest entry */
		stbi_image_free(l->icon_cache[0].data);
		memmove(&l->icon_cache[0], &l->icon_cache[1],
			(size_t)(l->num_icons - 1) * sizeof(l->icon_cache[0]));
		l->num_icons--;
	}

	uint8_t *data = load_icon(path, out_w, out_h);
	if (!data)
		return NULL;

	snprintf(l->icon_cache[l->num_icons].path,
		sizeof(l->icon_cache[l->num_icons].path), "%s", path);
	l->icon_cache[l->num_icons].data = data;
	l->icon_cache[l->num_icons].w = *out_w;
	l->icon_cache[l->num_icons].h = *out_h;
	l->num_icons++;
	return data;
}

/* draw a cached icon at (pad, vertically centered in the line),
 * scaled to LAUNCHER_ICON_SIZE * box_scale; advance *tx past the
 * icon plus padding when drawn. PNG goes through the stb cache;
 * SVG is rendered with librsvg (no recolor: app icons are full-color) */
static void launcher_draw_icon(cairo_t *cr, struct guibux_launcher *l,
		const char *path, int ly, int lh, int pad, int *tx) {
	if (path[0] == '\0')
		return;
	int target = LAUNCHER_ICON_SIZE * l->box_scale;

#ifdef GUIBUX_HAS_RSVG
	if (strstr(path, ".svg") != NULL) {
		GError *err = NULL;
		RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
		if (handle == NULL) {
			if (err) {
				g_error_free(err);
			}
			return;
		}
		gdouble dw = 0, dh = 0;
		rsvg_handle_get_intrinsic_size_in_pixels(handle, &dw, &dh);
		if (dw <= 0 || dh <= 0) {
			g_object_unref(handle);
			return;
		}
		int th = (int)(dh * target / dw + 0.5);
		int iy = ly + (lh - th) / 2;
		/* render directly at the target size (no bilinear upscale) */
		cairo_surface_t *surf = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, target, th);
		cairo_t *sfc = cairo_create(surf);
		{
			RsvgRectangle r = {0, 0, dw, dh};
			cairo_scale(sfc, (double)target / dw, (double)th / dh);
			rsvg_handle_render_document(handle, sfc, &r, NULL);
		}
		cairo_destroy(sfc);
		g_object_unref(handle);
		cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
		cairo_save(cr);
		cairo_translate(cr, pad, iy);
		cairo_set_source(cr, pat);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_pattern_destroy(pat);
		cairo_surface_destroy(surf);
		*tx += target + LAUNCHER_ICON_PAD * l->box_scale;
		return;
	}
#endif

	int iw, ih;
	uint8_t *img = get_cached_icon(l, path, &iw, &ih);
	if (!img || iw <= 0 || ih <= 0)
		return;
	double s = (double)target / iw;
	int th = (int)(ih * s + 0.5);
	int iy = ly + (lh - th) / 2;
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		img, CAIRO_FORMAT_ARGB32, iw, ih, iw * 4);
	cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
	cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
	cairo_save(cr);
	cairo_translate(cr, pad, iy);
	cairo_scale(cr, s, s);
	cairo_set_source(cr, pat);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_pattern_destroy(pat);
	cairo_surface_destroy(surf);
	*tx += target + LAUNCHER_ICON_PAD * l->box_scale;
}

/* draw a theme icon centered at (cx, cy) logical, size logical px,
 * scaled by scale; returns the drawn width in logical px (0 = missing,
 * caller falls back to text-only). PNG icons go through the stb cache;
 * SVG symbolic icons are rendered with librsvg and recolored to `color`
 * (the topbar text color) so they match the theme */
int topbar_icon_draw(cairo_t *cr, struct guibux_launcher *l,
		const char *name, int cx, int cy, int size, int scale,
		uint32_t color) {
	const char *path = resolve_icon(name);
	if (path == NULL) {
		return 0;
	}
	int tw = size * scale;
	int ix = cx * scale - tw / 2;
	int iy = cy * scale - tw / 2;

#ifdef GUIBUX_HAS_RSVG
	if (strstr(path, ".svg") != NULL) {
		GError *err = NULL;
		RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
		if (handle == NULL) {
			if (err) {
				g_error_free(err);
			}
			return 0;
		}
		gdouble dw = 0, dh = 0;
		rsvg_handle_get_intrinsic_size_in_pixels(handle, &dw, &dh);
		if (dw <= 0 || dh <= 0) {
			g_object_unref(handle);
			return 0;
		}
		int th = (int)(dh * tw / dw + 0.5);
		iy = cy * scale - th / 2;

		/* render directly at the target size (no bilinear upscale:
		 * upsampling a 16px icon bleeds edge alpha into the topbar
		 * background and shifts the perceived color); the alpha channel
		 * is then used as a mask to paint the icon in the target color */
		cairo_surface_t *surf = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, tw, th);
		cairo_t *sfc = cairo_create(surf);
		{
			RsvgRectangle r = {0, 0, dw, dh};
			cairo_scale(sfc, (double)tw / dw, (double)th / dh);
			rsvg_handle_render_document(handle, sfc, &r, NULL);
		}
		cairo_destroy(sfc);
		g_object_unref(handle);

		cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
		cairo_save(cr);
		cairo_translate(cr, ix, iy);
		cairo_set_source_rgb(cr,
			((color >> 16) & 0xff) / 255.0,
			((color >> 8) & 0xff) / 255.0,
			(color & 0xff) / 255.0);
		cairo_mask(cr, pat);
		cairo_restore(cr);
		cairo_pattern_destroy(pat);
		cairo_surface_destroy(surf);
		return size;
	}
#endif

	int iw, ih;
	uint8_t *img = get_cached_icon(l, path, &iw, &ih);
	if (!img || iw <= 0 || ih <= 0) {
		return 0;
	}
	double s = (double)(size * scale) / iw;
	int th = (int)(ih * s + 0.5);
	ix = cx * scale - tw / 2;
	iy = cy * scale - th / 2;
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		img, CAIRO_FORMAT_ARGB32, iw, ih, iw * 4);
	cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
	cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
	cairo_save(cr);
	cairo_translate(cr, ix, iy);
	cairo_scale(cr, s, s);
	cairo_set_source(cr, pat);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_pattern_destroy(pat);
	cairo_surface_destroy(surf);
	return size;
}

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

static void launcher_add_entry(struct guibux_launcher *l, const char *name,
		const char *exec, const char *icon_path) {
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
	strncpy(l->entries[l->num_entries].icon_path,
		icon_path ? icon_path : "",
		sizeof(l->entries[l->num_entries].icon_path) - 1);
	l->entries[l->num_entries].icon_path[sizeof(l->entries[l->num_entries].icon_path) - 1] = '\0';
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
			launcher_add_entry(l, e->d_name, e->d_name, NULL);
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
	char *name = NULL, *exec = NULL, *flatpak_id = NULL, *icon = NULL;
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
		} else if (strncmp(line, "Icon=", 5) == 0) {
			val = line + 5;
		} else continue;

		int len = strlen(val);
		while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r'))
			val[--len] = '\0';

		if (val[0] == '\0') continue;
		if (strncmp(line, "Name=", 5) == 0) {
			if (!name) name = strdup(val);
		} else if (strncmp(line, "Exec=", 5) == 0) {
			if (!exec) exec = strdup(val);
		} else if (strncmp(line, "Icon=", 5) == 0) {
			if (!icon) icon = strdup(val);
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
		const char *ic = icon ? resolve_icon(icon) : NULL;
		launcher_add_entry(l, name, cmd, ic);
		free(cmd);
	} else if (name && exec) {
		char *cmd = desktop_exec_to_cmd(exec);
		const char *ic = icon ? resolve_icon(icon) : NULL;
		launcher_add_entry(l, name, cmd, ic);
		free(cmd);
	}
	free(name);
	free(exec);
	free(flatpak_id);
	free(icon);
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

void launcher_free_icons(struct guibux_launcher *l) {
	for (int i = 0; i < l->num_icons; i++) {
		stbi_image_free(l->icon_cache[i].data);
		l->icon_cache[i].path[0] = '\0';
	}
	l->num_icons = 0;
}

/* re-resolve the preferred apps' icons after a config reload: the
 * theme dirs may have changed (icon_theme) or the preferred_appN
 * entries themselves. Old cached icons for paths that are no longer
 * referenced are dropped */
void launcher_rebuild_preferred(struct guibux_launcher *l) {
	for (int i = 0; i < l->num_preferred; i++) {
		/* config stores the raw icon (theme name or absolute path); an
		 * absolute path is already final */
		if (l->preferred[i].icon_path[0] == '/' ||
				l->preferred[i].icon_path[0] == '\0') {
			continue;
		}
		const char *resolved = resolve_icon(l->preferred[i].icon_path);
		if (resolved) {
			snprintf(l->preferred[i].icon_path,
				sizeof(l->preferred[i].icon_path), "%s", resolved);
		} else {
			wlr_log(WLR_INFO, "launcher: preferred app '%s': icon '%s' not found",
				l->preferred[i].name, l->preferred[i].icon_path);
		}
	}
	/* drop cached icons whose path is not referenced by any preferred
	 * app or topbar indicator anymore (theme change) */
	static const char *topbar_icons[] = {
		"audio-volume-muted", "audio-volume-low", "audio-volume-medium",
		"audio-volume-high", "microphone-muted",
		"microphone-sensitivity-low", "microphone-sensitivity-high",
		"battery-full", "battery-good", "battery-caution", "battery-low",
		"battery-full-charging", "battery-charging", "network-wireless",
	};
	for (int i = 0; i < l->num_icons; i++) {
		bool used = false;
		for (int p = 0; p < l->num_preferred && !used; p++) {
			if (strcmp(l->icon_cache[i].path,
					l->preferred[p].icon_path) == 0) {
				used = true;
			}
		}
		for (size_t t = 0; t < ICON_ARRAY_LEN(topbar_icons) && !used; t++) {
			const char *resolved = resolve_icon(topbar_icons[t]);
			if (resolved != NULL) {
				if (strcmp(l->icon_cache[i].path, resolved) == 0) {
					used = true;
				}
			}
		}
		if (!used) {
			stbi_image_free(l->icon_cache[i].data);
			memmove(&l->icon_cache[i], &l->icon_cache[i + 1],
				(size_t)(l->num_icons - i - 1) * sizeof(l->icon_cache[0]));
			l->num_icons--;
			i--;
		}
	}
}

void launcher_rebuild_icon_dirs(struct guibux_launcher *l) {
	icon_build_dirs(l);
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
		int tx = pad;
		launcher_draw_icon(cr, l, l->preferred[i].icon_path, ly, lh, pad, &tx);
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (i == l->selection) ? server->color_text :
			server->color_dim;
		launcher_draw_text_on_surface(cs, l->face, l->preferred[i].name, tx, mb, mc);
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
		int tx = pad;
		launcher_draw_icon(cr, l, l->entries[l->matches[i]].icon_path, ly, lh, pad, &tx);
		int mb = ly + lh / 2 + font_px * 35 / 100;
		uint32_t mc = (np + i == l->selection) ? server->color_text :
			server->color_dim;
		launcher_draw_text_on_surface(cs, l->face, l->entries[l->matches[i]].name, tx, mb, mc);
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
	tooltip_hide(server);
	osd_hide(server);
	power_panel_hide(server);

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = guibux_scale_round(output->scale);

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
	launcher_free_icons(l);
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
	icon_build_dirs(l);
	for (int i = 0; i < l->num_preferred; i++) {
		/* config stores the raw icon (theme name or absolute path);
		 * resolve the name to a path now that the theme dirs exist */
		if (l->preferred[i].icon_path[0] == '\0' ||
				l->preferred[i].icon_path[0] == '/')
			continue;
		const char *resolved = resolve_icon(l->preferred[i].icon_path);
		if (resolved) {
			snprintf(l->preferred[i].icon_path,
				sizeof(l->preferred[i].icon_path), "%s", resolved);
		} else {
			wlr_log(WLR_INFO, "launcher: preferred app '%s': icon '%s' not found",
				l->preferred[i].name, l->preferred[i].icon_path);
		}
	}
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

	/* icon probe: type an app name and log the resolved icon of the
	 * selected match (informational; headless envs may lack themes) */
	launcher_show(server);
	for (const char *p = "firefox"; *p; p++) {
		launcher_handle_key(server, (xkb_keysym_t)(uint8_t)*p);
	}
	if (l->num_matches > 0) {
		int si = l->selection - l->num_preferred;
		if (si >= 0 && si < l->num_matches) {
			const char *ip = l->entries[l->matches[si]].icon_path;
			wlr_log(WLR_INFO, "launcher-test: ICON '%s' (selected '%s', exec '%s')",
				ip[0] ? ip : "(none)",
				l->entries[l->matches[si]].name,
				l->entries[l->matches[si]].exec);
		}
	} else {
		wlr_log(WLR_INFO, "launcher-test: ICON (no matches for 'firefox')");
	}
	launcher_handle_key(server, XKB_KEY_Escape);
	if (l->active) {
		wlr_log(WLR_ERROR, "launcher-test: FAIL icon probe escape (still active)");
		return 0;
	}

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
