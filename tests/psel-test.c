// Primary selection test client for guibuxwm.
// Maps 1 toplevel and waits for the compositor's
// GUIBUX_TEST_PRIMARY_SELECTION hook to deliver a pointer enter (which gives
// this client a valid seat serial). It then sets the primary selection with a
// text/plain source and reads the data back through the compositor. The
// client output is the verdict: "psel-test: OK" on success.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "primary-selection-client-protocol.h"

#define PS_TEXT "guibux primary selection test"

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct zwp_primary_selection_device_manager_v1 *psel_mgr;
static struct zwp_primary_selection_device_v1 *psel_device;

static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct zwp_primary_selection_source_v1 *psel_source;
static struct zwp_primary_selection_offer_v1 *psel_offer;

static uint32_t enter_serial;
static int selection_set;
static int read_fd = -1;
static char read_buf[256];
static size_t read_len;

static void frame_cb(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_cb };

static struct wl_buffer *make_buffer(int w, int h) {
	int size = w * h * 4;
	int fd = syscall(SYS_memfd_create, "pseltest", 0);
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

static int32_t last_w, last_h;

static void commit_buffer(void) {
	int w = last_w > 0 ? last_w : 640;
	int h = last_h > 0 ? last_h : 400;
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
	// commit only after acking: a buffer committed before the first ack is
	// a protocol error (UNCONFIGURED_BUFFER)
	commit_buffer();
}
static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	if (s != NULL) {
		enter_serial = serial;
	}
}
static void pointer_leave(void *data, struct wl_pointer *p,
		uint32_t serial, struct wl_surface *s) {}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
		uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t axis_source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps) {
	// the compositor's test hook grants the pointer capability late (the
	// headless backend has no devices); only then is get_pointer legal
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && pointer == NULL) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {}
static const struct wl_seat_listener seat_listener = {
	seat_capabilities,
	seat_name,
};

static void source_send(void *data, struct zwp_primary_selection_source_v1 *s,
		const char *mime_type, int32_t fd) {
	if (strcmp(mime_type, "text/plain") == 0) {
		ssize_t n = strlen(PS_TEXT);
		while (n > 0) {
			ssize_t w = write(fd, PS_TEXT, n);
			if (w < 0) {
				return;
			}
			n -= w;
		}
	}
	close(fd);
}
static void source_cancelled(void *data,
		struct zwp_primary_selection_source_v1 *s) {}
static const struct zwp_primary_selection_source_v1_listener source_listener = {
	source_send,
	source_cancelled,
};

static void offer_mime(void *data, struct zwp_primary_selection_offer_v1 *o,
		const char *mime_type) {}
static const struct zwp_primary_selection_offer_v1_listener offer_listener = {
	offer_mime,
};

static void device_data_offer(void *data,
		struct zwp_primary_selection_device_v1 *d,
		struct zwp_primary_selection_offer_v1 *offer) {
	zwp_primary_selection_offer_v1_add_listener(offer, &offer_listener, NULL);
}

static void device_selection(void *data,
		struct zwp_primary_selection_device_v1 *d,
		struct zwp_primary_selection_offer_v1 *id) {
	if (id == NULL) {
		return;
	}
	if (psel_offer != NULL) {
		zwp_primary_selection_offer_v1_destroy(psel_offer);
	}
	psel_offer = id;
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "psel-test: FAIL socketpair\n");
		exit(1);
	}
	read_fd = fds[0];
	fcntl(read_fd, F_SETFL, O_NONBLOCK);
	zwp_primary_selection_offer_v1_receive(id, "text/plain", fds[1]);
	close(fds[1]);
}
static const struct zwp_primary_selection_device_v1_listener device_listener = {
	device_data_offer,
	device_selection,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, "xdg_wm_base") && version >= 3) {
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 3);
	} else if (!strcmp(interface, "wl_compositor")) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, "wl_shm")) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, "wl_seat")) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(interface, "zwp_primary_selection_device_manager_v1")) {
		psel_mgr = wl_registry_bind(reg, name,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	}
}
static void registry_global_remove(void *data, struct wl_registry *reg,
		uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

// returns 1 = success, 2 = failure, 0 = keep waiting
static int check_data(void) {
	char tmp[128];
	for (;;) {
		ssize_t n = read(read_fd, tmp, sizeof tmp);
		if (n > 0) {
			if (read_len + n < sizeof read_buf) {
				memcpy(read_buf + read_len, tmp, n);
				read_len += n;
			}
			continue;
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0;
			}
			return 0;
		}
		break; // EOF
	}
	read_buf[read_len] = '\0';
	if (strcmp(read_buf, PS_TEXT) == 0) {
		printf("psel-test: OK\n");
		return 1;
	}
	printf("psel-test: FAIL data mismatch: '%s'\n", read_buf);
	return 2;
}

static void on_alarm(int sig) {
	printf("psel-test: FAIL timeout (enter=%u set=%d offer=%d)\n",
		enter_serial, selection_set, psel_offer != NULL);
	_exit(2);
}

int main(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0);
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

	if (!wm_base || !compositor || !shm || !seat) {
		fprintf(stderr, "psel-test: FAIL missing required globals\n");
		return 2;
	}
	if (!psel_mgr) {
		printf("psel-test: FAIL no zwp_primary_selection_device_manager_v1 global\n");
		return 1;
	}
	psel_device = zwp_primary_selection_device_manager_v1_get_device(
		psel_mgr, seat);
	zwp_primary_selection_device_v1_add_listener(psel_device,
		&device_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_surface_set_window_geometry(xdg_surface, 0, 0, 640, 400);
	wl_surface_commit(surface);
	wl_display_roundtrip(display);
	printf("psel-test: toplevel mapped, waiting for pointer enter\n");

	for (;;) {
		wl_display_dispatch(display);
		if (wl_display_flush(display) < 0) {
			printf("psel-test: FAIL display disconnected\n");
			return 1;
		}
		if (!selection_set && enter_serial != 0) {
			psel_source =
				zwp_primary_selection_device_manager_v1_create_source(
					psel_mgr);
			zwp_primary_selection_source_v1_add_listener(
				psel_source, &source_listener, NULL);
			zwp_primary_selection_source_v1_offer(psel_source,
				"text/plain");
			zwp_primary_selection_device_v1_set_selection(
				psel_device, psel_source, enter_serial);
			selection_set = 1;
			printf("psel-test: set_selection (serial %u)\n",
				enter_serial);
		}
		if (read_fd >= 0) {
			int r = check_data();
			if (r == 1) {
				return 0;
			}
			if (r == 2) {
				return 1;
			}
		}
		usleep(20000);
	}
}
