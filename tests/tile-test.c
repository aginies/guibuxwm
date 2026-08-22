// Tile mode test client for guibuxwm.
// Maps 3 toplevels (640x400 each) and verifies the configured sizes match
// the tile mode set via GUIBUX_TEST_TILE_MODE (0=free, 1=split, 2=main+stack).
// Focus order after mapping = [w2, w1, w0] (last mapped is focused).
// On a 1280x720 output (topbar reserves the top 24px, tile area = 696px):
//   split:     w2 640x348 (col0 row0), w1 640x696 (col1), w0 640x348 (col0 row1)
//   main+stack: w2 640x696 (left main), w1 640x348 (right row0), w0 640x348
//   free:      each keeps its own 640x400
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

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_output *output0;

#define NWINS 3

struct test_win {
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	int32_t last_w, last_h;
	int last_fs;
	int configured;
};
static struct test_win wins[NWINS];
static int mode;
static int phase = 1; // 1: all 3 windows, 2: w0 closed
static int result = 0;

static void frame_cb(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_cb };

static struct wl_buffer *make_buffer(int w, int h) {
	int size = w * h * 4;
	int fd = syscall(SYS_memfd_create, "tiletest", 0);
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

static void check_phase(void) {
	int32_t exp[NWINS][2] = { {0,0}, {0,0}, {0,0} };
	if (phase == 1) {
		if (mode == 1) { // split: order [w2, w1, w0]
			exp[0][0] = 640; exp[0][1] = 348;
			exp[1][0] = 640; exp[1][1] = 696;
			exp[2][0] = 640; exp[2][1] = 348;
		} else if (mode == 2) { // main+stack
			exp[0][0] = 640; exp[0][1] = 348;
			exp[1][0] = 640; exp[1][1] = 348;
			exp[2][0] = 640; exp[2][1] = 696;
		}
		// free: 0x0 (compositor never imposes a size)
	} else { // w0 closed, remaining focus order [w2, w1]
		if (mode == 1) { // split: one per column, full height
			exp[1][0] = 640; exp[1][1] = 696;
			exp[2][0] = 640; exp[2][1] = 696;
		} else if (mode == 2) { // main+stack: left + full right
			exp[1][0] = 640; exp[1][1] = 696;
			exp[2][0] = 640; exp[2][1] = 696;
		}
	}
	for (int i = 0; i < NWINS; i++) {
		if (phase == 2 && i == 0) {
			continue;
		}
		if (!wins[i].configured) {
			printf("PHASE%d WIN%d FAIL: never configured\n", phase, i);
			result = 1;
			continue;
		}
		if (wins[i].last_w == exp[i][0] && wins[i].last_h == exp[i][1]) {
			printf("PHASE%d WIN%d OK: %dx%d (mode %d)\n", phase, i,
				wins[i].last_w, wins[i].last_h, mode);
		} else {
			printf("PHASE%d WIN%d FAIL: got %dx%d, expected %dx%d (mode %d)\n",
				phase, i, wins[i].last_w, wins[i].last_h,
				exp[i][0], exp[i][1], mode);
			result = 1;
		}
	}
}

static void toplevel_configure(void *data, struct xdg_toplevel *t,
		int32_t w, int32_t h, struct wl_array *states) {
	for (int i = 0; i < NWINS; i++) {
		if (wins[i].toplevel == t) {
			wins[i].last_w = w;
			wins[i].last_h = h;
			wins[i].last_fs = 0;
			int32_t *s;
			wl_array_for_each(s, states) {
				if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) {
					wins[i].last_fs = 1;
				}
			}
			wins[i].configured = 1;
			break;
		}
	}
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
	for (int i = 0; i < NWINS; i++) {
		if (wins[i].xdg_surface == s) {
			commit_size(&wins[i],
				wins[i].last_w > 0 ? wins[i].last_w : 640,
				wins[i].last_h > 0 ? wins[i].last_h : 400);
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
	} else if (!strcmp(interface, "wl_output") && output0 == NULL) {
		output0 = wl_registry_bind(reg, name, &wl_output_interface, 4);
	}
}
static void registry_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

static void map_win(int i) {
	wins[i].surface = wl_compositor_create_surface(compositor);
	wins[i].xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, wins[i].surface);
	xdg_surface_add_listener(wins[i].xdg_surface, &xdg_listener, NULL);
	wins[i].toplevel = xdg_surface_get_toplevel(wins[i].xdg_surface);
	xdg_toplevel_add_listener(wins[i].toplevel, &toplevel_listener, NULL);
	xdg_surface_set_window_geometry(wins[i].xdg_surface, 0, 0, 640, 400);
	wl_surface_commit(wins[i].surface); // no buffer: wait for first configure
}

static void on_alarm(int sig) {
	printf("TIMEOUT\n");
	_exit(2);
}

int main(int argc, char *argv[]) {
	signal(SIGALRM, on_alarm);
	alarm(15);
	mode = argc > 1 ? atoi(argv[1]) : 1;

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

	for (int i = 0; i < NWINS; i++) {
		map_win(i);
		wl_display_roundtrip(display);
		usleep(50000);
	}
	// let the final retiling configures settle
	long start = now_ms();
	while (now_ms() - start < 500) {
		wl_display_roundtrip(display);
		usleep(20000);
	}
	check_phase();

	// phase 2: close w0, remaining windows must re-pack
	xdg_toplevel_destroy(wins[0].toplevel);
	xdg_surface_destroy(wins[0].xdg_surface);
	wl_surface_destroy(wins[0].surface);
	phase = 2;
	start = now_ms();
	while (now_ms() - start < 500) {
		wl_display_roundtrip(display);
		usleep(20000);
	}
	check_phase();

	// phase 3 (tiled modes): fullscreen w2, then leave fullscreen;
	// it must return to its tile slot (640x696 in both modes after w0 closed)
	if (mode != 0) {
		xdg_toplevel_set_fullscreen(wins[2].toplevel, output0);
		start = now_ms();
		while (now_ms() - start < 500) {
			wl_display_roundtrip(display);
			usleep(20000);
		}
		if (wins[2].last_fs == 1 && wins[2].last_w == 1280 &&
				wins[2].last_h == 720) {
			printf("PHASE3 FS-ON OK: %dx%d fs=%d (mode %d)\n",
				wins[2].last_w, wins[2].last_h, wins[2].last_fs, mode);
		} else {
			printf("PHASE3 FS-ON FAIL: got %dx%d fs=%d, expected 1280x720 fs=1\n",
				wins[2].last_w, wins[2].last_h, wins[2].last_fs);
			result = 1;
		}
		xdg_toplevel_unset_fullscreen(wins[2].toplevel);
		start = now_ms();
		while (now_ms() - start < 500) {
			wl_display_roundtrip(display);
			usleep(20000);
		}
		if (wins[2].last_fs == 0 && wins[2].last_w == 640 &&
				wins[2].last_h == 696) {
			printf("PHASE3 FS-OFF OK: back to tile slot %dx%d (mode %d)\n",
				wins[2].last_w, wins[2].last_h, mode);
		} else {
			printf("PHASE3 FS-OFF FAIL: got %dx%d fs=%d, expected 640x696 fs=0\n",
				wins[2].last_w, wins[2].last_h, wins[2].last_fs);
			result = 1;
		}
	}

	wl_display_disconnect(display);
	return result;
}
