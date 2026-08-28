// Restore smoke-test client for guibuxwm.
// Maps a single toplevel with a fixed app_id, lets it settle, then exits.
// The compositor log is the verdict: on unmap it must log "restore: saved",
// and on a second run with the same app_id it must log "restore: '<app>' ->".
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define APP_ID "guibux-restore-test"

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static int32_t last_w, last_h;
static int configured;

static void frame_cb(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_cb };

static struct wl_buffer *make_buffer(int w, int h) {
	int size = w * h * 4;
	int fd = syscall(SYS_memfd_create, "restorertest", 0);
	ftruncate(fd, size);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) {
		p[i] = 0xFF3366CC;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	munmap(p, size);
	close(fd);
	return b;
}

static void commit_size(int w, int h) {
	struct wl_buffer *b = make_buffer(w, h);
	wl_surface_attach(surface, b, 0, 0);
	wl_surface_damage(surface, 0, 0, w, h);
	wl_callback_add_listener(wl_surface_frame(surface), &frame_listener, NULL);
	wl_surface_commit(surface);
	wl_buffer_destroy(b);
}

static void toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	last_w = w;
	last_h = h;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {}
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h) {}
static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t,
		struct wl_array *caps) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	toplevel_configure,
	toplevel_close,
	toplevel_configure_bounds,
	toplevel_wm_capabilities,
};

static void xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	configured = 1;
	commit_size(last_w > 0 ? last_w : 640, last_h > 0 ? last_h : 400);
}
static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, "xdg_wm_base") && version >= 3) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 3);
	} else if (!strcmp(interface, "wl_compositor")) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, "wl_shm")) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	}
}
static void registry_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static long now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void on_alarm(int sig) {
	printf("TIMEOUT\n");
	_exit(2);
}

int main(int argc, char *argv[]) {
	signal(SIGALRM, on_alarm);
	alarm(15);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "failed to connect to wayland display\n");
		return 2;
	}

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (!wm_base || !compositor || !shm) {
		fprintf(stderr, "missing required globals\n");
		return 2;
	}

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel,
		getenv("RESTORE_TEST_APP_ID") ? getenv("RESTORE_TEST_APP_ID") : APP_ID);
	xdg_surface_set_window_geometry(xdg_surface, 0, 0, 640, 400);
	wl_surface_commit(surface); // no buffer: wait for first configure

	// let the window map, configure, and settle
	long start = now_ms();
	while (now_ms() - start < 800) {
		wl_display_roundtrip(display);
		usleep(20000);
	}
	if (!configured) {
		printf("FAIL: never configured\n");
		return 1;
	}
	printf("MAPPED %s\n", APP_ID);

	if (argc > 1 && !strcmp(argv[1], "keep")) {
		// stay alive with the window mapped: the compositor exits first
		// (clean exit) and must save the position itself
		while (wl_display_dispatch(display) >= 0) {
			usleep(100000);
		}
		printf("DISCONNECTED\n");
		wl_display_disconnect(display);
		return 0;
	}

	// exit: the compositor sees the unmap and saves the position
	wl_display_disconnect(display);
	return 0;
}
