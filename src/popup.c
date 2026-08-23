#include "guibuxwm.h"

void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct guibux_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct guibux_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	struct guibux_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent == NULL || parent->data == NULL) {
		/* parent is not a scene-managed surface (e.g. popup of a
		 * destroyed toplevel): the popup stays functional but
		 * unrendered, instead of crashing the compositor */
		wlr_log(WLR_ERROR, "popup: no parent scene tree, popup will not render");
	} else {
		struct wlr_scene_tree *parent_tree = parent->data;
		xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	}

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}
