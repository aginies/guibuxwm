// Effects test client for guibuxwm.
// Maps A and B, destroys A when the compositor sends a close request
// (the compositor's GUIBUX_TEST_EFFECTS hook triggers it), maps C late,
// and idles. The compositor's hook verifies the close retile and the
// open scale-in. The compositor log is the verdict.
#include <signal.h>
#include <stdbool.h>
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

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;

#define NWINS 3

struct test_win {
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	int32_t last_w, last_h;
	bool destroyed;
};
static struct test_win wins[NWINS];

static void frame_cb(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_cb };

static struct wl_buffer *make_buffer(int w, int h) {
	int size = w * h * 4;
	int fd = syscall(SYS_memfd_create, "efftest", 0);
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

static void commit_size(struct test_win *win, int w, int h) {
	struct wl_buffer *b = make_buffer(w, h);
	wl_surface_attach(win->surface, b, 0, 0);
	wl_surface_damage(win->surface, 0, 0, w, h);
	wl_callback_add_listener(wl_surface_frame(win->surface), &frame_listener, NULL);
	wl_surface_commit(win->surface);
	wl_buffer_destroy(b);
}

static long now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void destroy_win(struct test_win *win) {
	if (win->destroyed) {
		return;
	}
	win->destroyed = true;
	xdg_toplevel_destroy(win->toplevel);
	xdg_surface_destroy(win->xdg_surface);
	wl_surface_destroy(win->surface);
}

static void toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	for (int i = 0; i < NWINS; i++) {
		if (wins[i].toplevel == t) {
			wins[i].last_w = w;
			wins[i].last_h = h;
			break;
		}
	}
}
/* the compositor's effects test closes window A: destroy it so the
 * compositor's unmap handler can start the close animation */
static void toplevel_close(void *data, struct xdg_toplevel *t) {
	struct test_win *win = data;
	destroy_win(win);
}
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
	// commit only after acking: a buffer committed before the first ack is
	// a protocol error (UNCONFIGURED_BUFFER)
	for (int i = 0; i < NWINS; i++) {
		if (wins[i].xdg_surface == s) {
			int w = wins[i].last_w > 0 ? wins[i].last_w : 640;
			int h = wins[i].last_h > 0 ? wins[i].last_h : 400;
			/* like a real client (GTK): keep the window geometry in sync
			 * with the logical size, or the WM resizes against a stale box */
			xdg_surface_set_window_geometry(s, 0, 0, w, h);
			commit_size(&wins[i], w, h);
			break;
		}
	}
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
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static void map_win(int i) {
	wins[i].surface = wl_compositor_create_surface(compositor);
	wins[i].xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, wins[i].surface);
	xdg_surface_add_listener(wins[i].xdg_surface, &xdg_listener, NULL);
	wins[i].toplevel = xdg_surface_get_toplevel(wins[i].xdg_surface);
	xdg_toplevel_add_listener(wins[i].toplevel, &toplevel_listener, &wins[i]);
	xdg_surface_set_window_geometry(wins[i].xdg_surface, 0, 0, 640, 400);
	wl_surface_commit(wins[i].surface); // no buffer: wait for first configure
}

static void on_alarm(int sig) {
	printf("TIMEOUT\n");
	_exit(2);
}

int main(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0); // survive a SIGTERM kill with progress visible
	signal(SIGALRM, on_alarm);
	alarm(30);

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

	map_win(0);
	wl_display_roundtrip(display);
	usleep(50000);
	map_win(1);
	wl_display_roundtrip(display);
	printf("effects-test: A and B mapped, idling\n");

	// idle: the compositor's effects test hook drives the close request;
	// C is mapped late so the hook can catch its open animation in flight
	long start = now_ms();
	bool mapped_c = false;
	while (now_ms() - start < 8000) {
		if (!mapped_c && now_ms() - start >= 1500) {
			map_win(2);
			mapped_c = true;
			printf("effects-test: C mapped\n");
		}
		/* wait for events (the compositor's close request) but bound the wait
		 * so the wall-clock check that maps C late still runs; dispatch_timeout
		 * also flushes the buffer commits the configure handler queues */
		struct timespec timeout = { 0, 20 * 1000 * 1000 };
		wl_display_dispatch_timeout(display, &timeout);
	}

	wl_display_disconnect(display);
	printf("effects-test: done\n");
	return 0;
}
