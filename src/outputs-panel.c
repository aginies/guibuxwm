// Outputs panel: keyboard-driven monitor layout editor (Mod+m). Edits the
// `outputs` config spec live: position, mode, transform, enable/off. Every
// change rewrites the config line and re-applies it via outputs_apply, the
// same flow the guibuxwm-output tool uses without the signal round-trip.
// Connected outputs that are not in the config are seeded from their live
// state so the panel manages the whole layout.

#include "guibuxwm.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define PANEL_LINE_H 24
#define PANEL_PAD 12
#define PANEL_BOX_W 840

static struct wlr_output *panel_find_output(struct guibux_server *server,
		const char *name) {
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->name != NULL &&
				strcmp(o->wlr_output->name, name) == 0) {
			return o->wlr_output;
		}
	}
	return NULL;
}

/* sync the entries with the live state: positions of enabled outputs
 * follow the layout box, and a requested mode the backend rejected falls
 * back to the current mode. The panel node is anchored to its output:
 * it follows it when the layout moves or resizes it */
static void panel_refresh(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	for (int i = 0; i < p->num_entries; i++) {
		struct guibux_output_entry *e = &p->entries[i];
		struct wlr_output *wlr = panel_find_output(server, e->name);
		if (wlr == NULL) {
			continue;
		}
		if (!e->disabled) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, wlr, &box);
			e->x = box.x;
			e->y = box.y;
		}
		if (e->mode_w > 0 && wlr->current_mode != NULL) {
			e->mode_w = wlr->current_mode->width;
			e->mode_h = wlr->current_mode->height;
		}
	}
	if (p->output != NULL && p->scene_node != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, p->output, &box);
		int ew, eh;
		wlr_output_effective_resolution(p->output, &ew, &eh);
		wlr_scene_node_set_position(&p->scene_node->node,
			box.x + (ew - p->box_w) / 2, box.y + (eh - p->box_h) / 2);
	}
}

/* connected outputs missing from the config are auto-arranged; seed an
 * entry from the live state so the panel can place them explicitly */
static void panel_seed(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	struct guibux_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (p->num_entries >= OUTPUTS_CONFIG_MAX_ENTRIES) {
			break;
		}
		if (o->wlr_output->name == NULL) {
			continue;
		}
		int i;
		for (i = 0; i < p->num_entries; i++) {
			if (!strcmp(p->entries[i].name, o->wlr_output->name)) {
				break;
			}
		}
		if (i < p->num_entries) {
			continue;
		}
		struct guibux_output_entry *e = &p->entries[p->num_entries++];
		snprintf(e->name, sizeof(e->name), "%s", o->wlr_output->name);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		e->x = o->disabled ? 0 : box.x;
		e->y = o->disabled ? 0 : box.y;
		e->mode_w = o->wlr_output->current_mode ?
			o->wlr_output->current_mode->width : 0;
		e->mode_h = o->wlr_output->current_mode ?
			o->wlr_output->current_mode->height : 0;
		e->transform = -1;
		e->disabled = o->disabled;
	}
}

/* persist the entries to the config and apply them live */
static void panel_commit(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (server->config_path == NULL) {
		snprintf(p->status, sizeof(p->status), "no config file");
		return;
	}
	char spec[2048];
	outputs_spec_format(p->entries, p->num_entries, spec, sizeof(spec));
	if (outputs_config_write(server->config_path, spec) != 0) {
		snprintf(p->status, sizeof(p->status), "cannot write config");
		return;
	}
	outputs_apply(server);
	panel_refresh(server);
	snprintf(p->status, sizeof(p->status), "saved");
}

/* effective width of an entry: the configured mode (width and height
 * swapped for 90/270 rotations), the live layout box when no mode or no
 * rotation is set (the box already accounts for the live rotation),
 * 1920 when the output is not connected */
static int panel_entry_width(struct guibux_server *server,
		const struct guibux_output_entry *e) {
	if (e->mode_w > 0 && e->mode_h > 0 && e->transform >= 0) {
		if (e->transform == 1 || e->transform == 3) {
			return e->mode_h;
		}
		return e->mode_w;
	}
	struct wlr_output *wlr = panel_find_output(server, e->name);
	if (wlr != NULL) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, wlr, &box);
		if (box.width > 0) {
			return box.width;
		}
	}
	if (e->mode_w > 0) {
		return e->mode_w;
	}
	return 1920;
}

/* Left/Right: move the selected entry one step in that direction within
 * its row (same y, x order), then repack the row contiguously from its
 * left edge: each x is the previous one's x plus its effective width
 * (mode, rotation-aware), so differing resolutions never leave gaps or
 * overlaps. The list positions swap with the passed monitor, so the list
 * stays the screen order; the selection follows the moved monitor */
static void panel_move(struct guibux_server *server, int dir) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->num_entries == 0) {
		return;
	}
	struct guibux_output_entry *sel = &p->entries[p->selected];
	if (sel->disabled) {
		snprintf(p->status, sizeof(p->status), "entry is off");
		return;
	}
	/* the row: enabled entries on the selected y, in x order */
	int row[OUTPUTS_CONFIG_MAX_ENTRIES];
	int nrow = 0;
	int pos = -1;
	for (int i = 0; i < p->num_entries; i++) {
		if (p->entries[i].disabled || p->entries[i].y != sel->y) {
			continue;
		}
		int j = nrow;
		while (j > 0 && p->entries[row[j - 1]].x > p->entries[i].x) {
			row[j] = row[j - 1];
			j--;
		}
		row[j] = i;
		nrow++;
		if (i == p->selected) {
			pos = j;
		}
	}
	int from = pos + (dir < 0 ? -1 : 1);
	if (nrow < 2 || from < 0 || from >= nrow) {
		snprintf(p->status, sizeof(p->status),
			dir < 0 ? "no monitor to the left" :
			"no monitor to the right");
		return;
	}
	int anchor = row[from];
	/* from is adjacent to pos: swap the two slots in the row order */
	int t = row[pos];
	row[pos] = row[from];
	row[from] = t;
	/* repack the row from its left edge */
	int origin = p->entries[row[0]].x;
	for (int i = 1; i < nrow; i++) {
		if (p->entries[row[i]].x < origin) {
			origin = p->entries[row[i]].x;
		}
	}
	int x = origin;
	for (int i = 0; i < nrow; i++) {
		p->entries[row[i]].x = x;
		x += panel_entry_width(server, &p->entries[row[i]]);
	}
	/* keep the list in screen order: swap the moved entry with the one
	 * it passed; the selection follows the moved monitor */
	struct guibux_output_entry tmp = *sel;
	*sel = p->entries[anchor];
	p->entries[anchor] = tmp;
	p->selected = anchor;
	panel_commit(server);
}

static void panel_cycle_mode(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->num_entries == 0) {
		return;
	}
	struct guibux_output_entry *e = &p->entries[p->selected];
	if (e->disabled) {
		snprintf(p->status, sizeof(p->status), "entry is off");
		return;
	}
	struct wlr_output *wlr = panel_find_output(server, e->name);
	if (wlr == NULL) {
		snprintf(p->status, sizeof(p->status), "not connected");
		return;
	}
	struct wlr_output_mode *m, *next = NULL;
	bool found = false;
	wl_list_for_each(m, &wlr->modes, link) {
		if (found) {
			next = m;
			break;
		}
		if (e->mode_w > 0 && m->width == e->mode_w &&
				m->height == e->mode_h) {
			found = true;
		}
	}
	if (next == NULL) {
		wl_list_for_each(m, &wlr->modes, link) {
			next = m;
			break;
		}
	}
	if (next == NULL) {
		snprintf(p->status, sizeof(p->status), "no modes");
		return;
	}
	e->mode_w = next->width;
	e->mode_h = next->height;
	panel_commit(server);
}

static void panel_cycle_transform(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->num_entries == 0) {
		return;
	}
	struct guibux_output_entry *e = &p->entries[p->selected];
	if (e->disabled) {
		snprintf(p->status, sizeof(p->status), "entry is off");
		return;
	}
	e->transform = (e->transform + 1) % 4;
	panel_commit(server);
}

static void panel_toggle(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->num_entries == 0) {
		return;
	}
	struct guibux_output_entry *e = &p->entries[p->selected];
	e->disabled = !e->disabled;
	panel_commit(server);
}

static void panel_render(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->buffer == NULL || server->launcher.face == NULL) {
		return;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(p->buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_XRGB8888) {
		wlr_buffer_end_data_ptr_access(p->buffer);
		return;
	}
	int w = p->box_w * p->box_scale;
	int hgt = p->box_h * p->box_scale;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_RGB24, w, hgt, (int)stride);
	cairo_t *cr = cairo_create(cs);

	set_color(cr, server->color_bg);
	cairo_paint(cr);
	set_color(cr, server->color_border);
	cairo_set_line_width(cr, p->box_scale);
	cairo_rectangle(cr, p->box_scale / 2.0, p->box_scale / 2.0,
		w - p->box_scale, hgt - p->box_scale);
	cairo_stroke(cr);

	FT_Face face = server->launcher.face;
	int font_px = LAUNCHER_FONT_PX * p->box_scale;
	FT_Set_Pixel_Sizes(face, 0, font_px);

	for (int i = 0; i < p->num_entries; i++) {
		int ly = i * PANEL_LINE_H * p->box_scale;
		int lh = PANEL_LINE_H * p->box_scale;
		if (i == p->selected) {
			set_color(cr, server->color_highlight);
			cairo_rectangle(cr, 0, ly, w, lh);
			cairo_fill(cr);
		}
		const struct guibux_output_entry *e = &p->entries[i];
		uint32_t tc = (i == p->selected) ? server->color_bg :
			(e->disabled ? server->color_dim : server->color_text);
		char line[128];
		char pos[32] = "-", mode[32] = "-", rot[16] = "-";
		if (!e->disabled) {
			snprintf(pos, sizeof(pos), "%dx%d", e->x, e->y);
		}
		if (e->mode_w > 0) {
			snprintf(mode, sizeof(mode), "%dx%d", e->mode_w, e->mode_h);
		}
		if (e->transform >= 0) {
			snprintf(rot, sizeof(rot), "%s",
				outputs_transform_name(e->transform));
		}
		snprintf(line, sizeof(line), "%-16.16s %-12s %-12s %-10s %-5s",
			e->name, pos, mode, rot, e->disabled ? "off" : "on");
		int mb = ly + lh / 2 + font_px * 35 / 100;
		launcher_draw_text_on_surface(cs, face, line,
			PANEL_PAD * p->box_scale, mb, tc);
	}

	int sy = p->num_entries * PANEL_LINE_H * p->box_scale;
	int sh = PANEL_LINE_H * p->box_scale;
	set_color(cr, server->color_border);
	cairo_rectangle(cr, 0, sy, w, p->box_scale);
	cairo_fill(cr);
	int sb = sy + sh / 2 + font_px * 35 / 100;
	launcher_draw_text_on_surface(cs, face,
		p->status[0] != '\0' ? p->status :
		"up/dn select  left/right move  m mode  r rotate  d off  esc close",
		PANEL_PAD * p->box_scale, sb, server->color_dim);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wlr_buffer_end_data_ptr_access(p->buffer);
	if (p->scene_node != NULL) {
		wlr_scene_buffer_set_buffer(p->scene_node, p->buffer);
	}
	if (p->output != NULL) {
		wlr_output_schedule_frame(p->output);
	}
}

void outputs_panel_show(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (p->active) {
		return;
	}
	tooltip_hide(server);

	struct wlr_output *output = output_at_cursor(server);
	if (output == NULL) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	int ew, eh;
	wlr_output_effective_resolution(output, &ew, &eh);
	int scale = output->scale > 1 ? (int)output->scale : 1;

	/* start from the config spec (fallback: the startup spec), then seed
	 * connected outputs that are not in the config */
	char spec[2048];
	spec[0] = '\0';
	if (server->config_path != NULL) {
		outputs_config_read(server->config_path, spec, sizeof(spec));
	}
	if (spec[0] == '\0' && server->outputs_env_spec != NULL) {
		snprintf(spec, sizeof(spec), "%s", server->outputs_env_spec);
	}
	p->num_entries = 0;
	if (outputs_spec_parse(spec, p->entries, OUTPUTS_CONFIG_MAX_ENTRIES,
			&p->num_entries) < 0) {
		p->num_entries = 0;
	}
	panel_seed(server);
	p->selected = 0;
	p->status[0] = '\0';

	int bw = PANEL_BOX_W;
	if (bw > ew - 20) {
		bw = ew - 20;
	}
	p->box_w = bw;
	p->box_h = (p->num_entries + 1) * PANEL_LINE_H;
	p->box_scale = scale;
	p->output = output;

	uint64_t mods[] = { DRM_FORMAT_MOD_INVALID };
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_XRGB8888,
		.len = 1,
		.modifiers = mods,
	};
	p->buffer = wlr_allocator_create_buffer(server->launcher.shm_alloc,
		bw * scale, p->box_h * scale, &format);
	if (p->buffer == NULL) {
		return;
	}
	p->scene_node = wlr_scene_buffer_create(&server->scene->tree, p->buffer);
	wlr_scene_buffer_set_dest_size(p->scene_node, bw, p->box_h);
	wlr_scene_node_set_position(&p->scene_node->node,
		box.x + (ew - bw) / 2, box.y + (eh - p->box_h) / 2);

	p->active = true;
	panel_render(server);
}

void outputs_panel_hide(struct guibux_server *server) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (!p->active) {
		return;
	}
	p->active = false;
	p->num_entries = 0;
	p->selected = 0;
	p->output = NULL;
	if (p->scene_node != NULL) {
		wlr_scene_node_destroy(&p->scene_node->node);
		p->scene_node = NULL;
	}
	if (p->buffer != NULL) {
		wlr_buffer_drop(p->buffer);
		p->buffer = NULL;
	}
}

bool outputs_panel_handle_key(struct guibux_server *server, xkb_keysym_t sym) {
	struct guibux_outputs_panel *p = &server->outputs_panel;
	if (!p->active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		outputs_panel_hide(server);
		return true;
	case XKB_KEY_Up:
		if (p->num_entries > 0) {
			p->selected = (p->selected + p->num_entries - 1) %
				p->num_entries;
			panel_render(server);
		}
		return true;
	case XKB_KEY_Down:
		if (p->num_entries > 0) {
			p->selected = (p->selected + 1) % p->num_entries;
			panel_render(server);
		}
		return true;
	case XKB_KEY_Left:
		panel_move(server, -1);
		panel_render(server);
		return true;
	case XKB_KEY_Right:
		panel_move(server, 1);
		panel_render(server);
		return true;
	case XKB_KEY_m:
		panel_cycle_mode(server);
		panel_render(server);
		return true;
	case XKB_KEY_r:
		panel_cycle_transform(server);
		panel_render(server);
		return true;
	case XKB_KEY_d:
		panel_toggle(server);
		panel_render(server);
		return true;
	default:
		break;
	}
	return true;
}
