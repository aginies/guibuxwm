// guibuxwm-output: configure the guibuxwm monitor layout live.
//
// Edits the `outputs` line of the guibuxwm config and signals the running
// compositor (SIGUSR1) to re-apply it, so changes take effect without a
// restart. The connected outputs (names, positions, current and available
// modes) are read from the state file the compositor maintains at
// $XDG_STATE_HOME/guibuxwm/outputs (or ~/.local/state/guibuxwm/outputs).
//
// Config entry format (same as the compositor):
//   NAME@XxY[:WxH[:ROT]]   place NAME at XxY, optional mode WxH,
//                          optional rotation normal|90|180|270
//   NAME@off               disable NAME
//
// Usage:
//   guibuxwm-output list
//   guibuxwm-output listall
//   guibuxwm-output set NAME X Y [--mode WxH] [--transform normal|90|180|270]
//   guibuxwm-output enable NAME
//   guibuxwm-output disable NAME
//   guibuxwm-output apply
//
// Options:
//   -c FILE      config file (default: GUIBUX_CONFIG or
//                ~/.config/guibuxwm/config)
//   --no-apply   save to the config without signaling the compositor

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "outputs-config.h"

#define MAX_ENTRIES 16
#define MAX_MODES 512

struct state_output {
	char name[64];
	int x, y, w, h;
	int mode_w, mode_h;
	int transform;
	int enabled;
	char modes[MAX_MODES];
};

static void die(const char *msg) {
	fprintf(stderr, "guibuxwm-output: %s\n", msg);
	exit(1);
}

static void usage(void) {
	fprintf(stderr,
		"Usage: guibuxwm-output [options] command\n"
		"\n"
		"Commands:\n"
		"  list                  connected outputs: name, position, mode, state\n"
		"  listall               all DRM connectors (connected + disconnected)\n"
		"  set NAME X Y          place output NAME at XxY (keeps mode/transform)\n"
		"  enable NAME           enable output NAME\n"
		"  disable NAME          disable output NAME\n"
		"  apply                 re-apply the config (no change)\n"
		"\n"
		"Options:\n"
		"  --mode WxH            resolution for 'set' (e.g. 1920x1080)\n"
		"  --transform T         rotation for 'set': normal|90|180|270\n"
		"  --no-apply            save to the config without signaling the compositor\n"
		"  -c FILE               config file (default: GUIBUX_CONFIG or\n"
		"                        ~/.config/guibuxwm/config)\n"
		"\n"
		"Examples:\n"
		"  guibuxwm-output list\n"
		"  guibuxwm-output set HDMI-A-1 1920 0 --mode 1920x1080\n"
		"  guibuxwm-output set DP-1 0 0 --transform 90\n"
		"  guibuxwm-output enable HDMI-A-1\n"
		"  guibuxwm-output disable HDMI-A-1\n"
		"  guibuxwm-output apply\n");
	exit(1);
}

static void state_path(char *buf, size_t n) {
	const char *state_home = getenv("XDG_STATE_HOME");
	if (state_home != NULL && state_home[0] != '\0') {
		snprintf(buf, n, "%s/guibuxwm/outputs", state_home);
		return;
	}
	const char *home = getenv("HOME");
	if (home != NULL) {
		snprintf(buf, n, "%s/.local/state/guibuxwm/outputs", home);
		return;
	}
	snprintf(buf, n, "/tmp/guibuxwm-outputs");
}

static void default_config_path(char *buf, size_t n) {
	const char *env = getenv("GUIBUX_CONFIG");
	if (env != NULL && env[0] != '\0') {
		snprintf(buf, n, "%s", env);
		return;
	}
	const char *home = getenv("HOME");
	if (home != NULL) {
		snprintf(buf, n, "%s/.config/guibuxwm/config", home);
		return;
	}
	die("no HOME and no GUIBUX_CONFIG: cannot locate the config");
}

/* state file: "# guibuxwm outputs: pid=N" header, then one line per
 * output: name x y w h mode_w mode_h transform enabled modes */
static int read_state(const char *path, int *pid, struct state_output *outs,
		int cap) {
	*pid = 0;
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	int n = 0;
	char line[2048];
	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '#') {
			char *p = strstr(line, "pid=");
			if (p != NULL) {
				*pid = atoi(p + 4);
			}
			continue;
		}
		if (n >= cap) {
			break;
		}
		struct state_output *o = &outs[n];
		int tx = 0, ty = 0, tw = 0, th = 0, mw = 0, mh = 0, tr = 0, en = 0;
		if (sscanf(line, " %63[^ ] %d %d %d %d %d %d %d %d %511[^\n]",
				o->name, &tx, &ty, &tw, &th, &mw, &mh, &tr, &en,
				o->modes) < 9) {
			continue;
		}
		o->x = tx;
		o->y = ty;
		o->w = tw;
		o->h = th;
		o->mode_w = mw;
		o->mode_h = mh;
		o->transform = tr;
		o->enabled = en;
		n++;
	}
	fclose(f);
	return n;
}

static struct state_output *state_find(const struct state_output *outs,
		int n, const char *name) {
	for (int i = 0; i < n; i++) {
		if (!strcmp(outs[i].name, name)) {
			return (struct state_output *)&outs[i];
		}
	}
	return NULL;
}

/* scan /sys/class/drm for all connectors (connected and disconnected),
 * merge with live state data when available */
static void listall(const struct state_output *outs, int nouts) {
	DIR *d = opendir("/sys/class/drm");
	if (d == NULL) {
		die("cannot open /sys/class/drm");
	}
	struct dirent *ent;
	int count = 0;
	while ((ent = readdir(d)) != NULL) {
		/* match cardN-CONNECTOR pattern, skip Writeback and card dirs */
		if (strncmp(ent->d_name, "card", 4) != 0) {
			continue;
		}
		char *dash = strchr(ent->d_name, '-');
		if (dash == NULL) {
			continue;
		}
		if (strstr(ent->d_name, "Writeback")) {
			continue;
		}
		const char *conn = dash + 1;
		char status_path[512];
		snprintf(status_path, sizeof(status_path),
			"/sys/class/drm/%s/status", ent->d_name);
		FILE *sf = fopen(status_path, "r");
		if (sf == NULL) {
			continue;
		}
		char status[32] = "";
		if (fgets(status, sizeof(status), sf) != NULL) {
			status[strcspn(status, "\n")] = '\0';
		}
		fclose(sf);
		bool connected = (strcmp(status, "connected") == 0);

		/* look up live state for this connector */
		struct state_output *so = state_find(outs, nouts, conn);

		char pos[32] = "-", mode[32] = "-", rot[16] = "-";
		const char *state_str = "-";
		if (so != NULL) {
			if (so->enabled) {
				snprintf(pos, sizeof(pos), "%dx%d", so->x, so->y);
				state_str = "on";
			} else {
				state_str = "off";
			}
			if (so->mode_w > 0) {
				snprintf(mode, sizeof(mode), "%dx%d", so->mode_w, so->mode_h);
			}
			if (so->transform >= 0) {
				snprintf(rot, sizeof(rot), "%s",
					outputs_transform_name(so->transform));
			}
		}
		printf("%-16s %-12s %-12s %-10s %-5s  %s\n",
			conn, pos, mode, rot, state_str,
			connected ? "yes" : "no");
		count++;
	}
	closedir(d);
	if (count == 0) {
		fprintf(stderr, "guibuxwm-output: no DRM connectors found\n");
	}
}

/* true when another enabled entry sits at the same position: two outputs
 * at one XxY mirror the screen instead of extending it */
static bool position_taken(const struct guibux_output_entry *arr, int num,
		const char *name, int x, int y) {
	for (int i = 0; i < num; i++) {
		if (arr[i].disabled || !strcmp(arr[i].name, name)) {
			continue;
		}
		if (arr[i].x == x && arr[i].y == y) {
			return true;
		}
	}
	return false;
}

/* effective width of an entry: the live box width from the state file
 * (rotation already applied), else the configured mode (width and height
 * swapped for 90/270 rotations), else 1920 */
static int entry_width(const struct guibux_output_entry *e,
		const struct state_output *outs, int nouts) {
	const struct state_output *so = state_find(outs, nouts, e->name);
	if (so != NULL && so->w > 0) {
		return so->w;
	}
	if (e->mode_w > 0 && e->mode_h > 0) {
		if (e->transform == 1 || e->transform == 3) {
			return e->mode_h;
		}
		return e->mode_w;
	}
	return 1920;
}

/* first free slot right of every enabled entry except name: extends the
 * row instead of mirroring; widths from the state file (rotation-aware),
 * falling back to the configured mode, then 1920 */
static void position_next(const struct guibux_output_entry *arr, int num,
		const char *name, const struct state_output *outs, int nouts,
		int *x, int *y) {
	int right = 0;
	for (int i = 0; i < num; i++) {
		if (arr[i].disabled || !strcmp(arr[i].name, name)) {
			continue;
		}
		int w = entry_width(&arr[i], outs, nouts);
		if (arr[i].x + w > right) {
			right = arr[i].x + w;
		}
	}
	*x = right;
	*y = 0;
}

/* the modes list is "WxH@refresh,WxH@refresh,...": exact token match, so
 * 1920x1080 does not match inside 1920x10800 or 11920x1080 */
static int mode_list_has(const char *modes, int w, int h) {
	char pat[32];
	snprintf(pat, sizeof(pat), "%dx%d", w, h);
	size_t plen = strlen(pat);
	const char *p = modes;
	while ((p = strstr(p, pat)) != NULL) {
		int left_ok = (p == modes) || (p[-1] == ',');
		const char *after = p + plen;
		int right_ok = (*after == '@' || *after == ',' || *after == '\0');
		if (left_ok && right_ok) {
			return 1;
		}
		p += plen;
	}
	return 0;
}

static int send_apply(int pid) {
	if (pid <= 0) {
		fprintf(stderr, "guibuxwm-output: compositor not running (no pid in "
			"the state file); saved to the config, applies on next start\n");
		return 0;
	}
	if (kill(pid, SIGUSR1) != 0) {
		fprintf(stderr, "guibuxwm-output: cannot signal compositor pid %d: %s\n",
			pid, strerror(errno));
		return 1;
	}
	return 0;
}

/* load the current config entries; returns the config spec string owner */
static int load_entries(const char *config, struct guibux_output_entry *arr,
		int *num, char *spec_buf, size_t spec_n) {
	if (!outputs_config_read(config, spec_buf, spec_n)) {
		spec_buf[0] = '\0';
		*num = 0;
		return 0;
	}
	if (outputs_spec_parse(spec_buf, arr, OUTPUTS_CONFIG_MAX_ENTRIES, num) < 0) {
		die("bad outputs spec in config");
	}
	return *num;
}

static struct guibux_output_entry *entry_upsert(struct guibux_output_entry *arr,
		int *num, const char *name) {
	for (int i = 0; i < *num; i++) {
		if (!strcmp(arr[i].name, name)) {
			return &arr[i];
		}
	}
	if (*num >= OUTPUTS_CONFIG_MAX_ENTRIES) {
		die("too many outputs (max 16)");
	}
	struct guibux_output_entry *e = &arr[(*num)++];
	snprintf(e->name, sizeof(e->name), "%s", name);
	e->x = 0;
	e->y = 0;
	e->mode_w = 0;
	e->mode_h = 0;
	e->transform = -1;
	e->disabled = false;
	return e;
}

int main(int argc, char *argv[]) {
	const char *config = NULL;
	int mode_w = 0, mode_h = 0;
	int transform = -1;
	int no_apply = 0;
	/* options may appear anywhere; the rest are positional (command,
	 * name, x, y) */
	int pos[8];
	int npos = 0;
	for (int j = 1; j < argc; j++) {
		if (!strcmp(argv[j], "-c")) {
			if (j + 1 >= argc) {
				usage();
			}
			config = argv[++j];
		} else if (!strcmp(argv[j], "--mode")) {
			if (j + 1 >= argc ||
					sscanf(argv[++j], "%dx%d", &mode_w, &mode_h) != 2 ||
					mode_w <= 0 || mode_h <= 0) {
				fprintf(stderr, "guibuxwm-output: bad --mode (expected WxH)\n");
				return 1;
			}
		} else if (!strcmp(argv[j], "--transform")) {
			if (j + 1 >= argc ||
					(transform = outputs_transform_from_name(argv[++j])) < 0) {
				fprintf(stderr,
					"guibuxwm-output: bad --transform (expected normal|90|180|270)\n");
				return 1;
			}
		} else if (!strcmp(argv[j], "--no-apply")) {
			no_apply = 1;
		} else {
			if (npos >= 8) {
				usage();
			}
			pos[npos++] = j;
		}
	}
	if (npos == 0) {
		usage();
	}
	const char *cmd = argv[pos[0]];

	char state_buf[PATH_MAX];
	state_path(state_buf, sizeof(state_buf));
	int pid = 0;
	struct state_output outs[MAX_ENTRIES];
	int nouts = read_state(state_buf, &pid, outs, MAX_ENTRIES);

	if (!strcmp(cmd, "list")) {
		if (nouts == 0) {
			fprintf(stderr, "guibuxwm-output: no state file at %s "
				"(compositor not running?)\n", state_buf);
			return 1;
		}
		printf("%-16s %-12s %-12s %-10s %-5s\n",
			"NAME", "POSITION", "MODE", "TRANSFORM", "STATE");
		for (int j = 0; j < nouts; j++) {
			const struct state_output *o = &outs[j];
			char pos[32] = "-", mode[32] = "-";
			if (o->enabled) {
				snprintf(pos, sizeof(pos), "%dx%d", o->x, o->y);
			}
			if (o->mode_w > 0) {
				snprintf(mode, sizeof(mode), "%dx%d", o->mode_w, o->mode_h);
			}
			printf("%-16s %-12s %-12s %-10s %-5s\n",
				o->name, pos, mode, outputs_transform_name(o->transform),
				o->enabled ? "on" : "off");
		}
		printf("\nAvailable modes:\n");
		for (int j = 0; j < nouts; j++) {
			printf("  %-16s %s\n", outs[j].name,
				outs[j].modes[0] != '\0' ? outs[j].modes : "(none)");
		}
		return 0;
	}

	if (!strcmp(cmd, "listall")) {
		printf("%-16s %-12s %-12s %-10s %-5s  %-9s\n",
			"NAME", "POSITION", "MODE", "TRANSFORM", "STATE", "CONNECTED");
		listall(outs, nouts);
		return 0;
	}

	if (!strcmp(cmd, "apply")) {
		if (no_apply) {
			return 0;
		}
		return send_apply(pid);
	}

	if (npos < 2) {
		fprintf(stderr, "guibuxwm-output: '%s' needs an output name\n", cmd);
		return 1;
	}
	const char *name = argv[pos[1]];
	if (strlen(name) >= 64) {
		die("output name too long (max 63 chars)");
	}

	char cfg_buf[PATH_MAX];
	if (config == NULL) {
		default_config_path(cfg_buf, sizeof(cfg_buf));
		config = cfg_buf;
	}
	char spec_buf[2048];
	struct guibux_output_entry entries[OUTPUTS_CONFIG_MAX_ENTRIES];
	int num = 0;
	load_entries(config, entries, &num, spec_buf, sizeof(spec_buf));
	struct guibux_output_entry *e = entry_upsert(entries, &num, name);

	if (!strcmp(cmd, "set")) {
		if (npos < 4) {
			fprintf(stderr, "guibuxwm-output: set needs NAME X Y\n");
			return 1;
		}
		e->x = atoi(argv[pos[2]]);
		e->y = atoi(argv[pos[3]]);
		if (npos > 4) {
			fprintf(stderr, "guibuxwm-output: unexpected argument '%s'\n",
				argv[pos[4]]);
			return 1;
		}
		e->disabled = false;
		if (position_taken(entries, num, name, e->x, e->y)) {
			fprintf(stderr,
				"guibuxwm-output: warning: %s: %dx%d is already used by "
				"another output; this mirrors, not extends\n",
				name, e->x, e->y);
		}
		if (mode_w > 0) {
			e->mode_w = mode_w;
			e->mode_h = mode_h;
		}
		if (transform >= 0) {
			e->transform = transform;
		}
	} else if (!strcmp(cmd, "enable")) {
		if (npos > 2) {
			fprintf(stderr, "guibuxwm-output: unexpected argument '%s'\n",
				argv[pos[2]]);
			return 1;
		}
		e->disabled = false;
		/* keep the configured position; a brand-new entry takes the
		 * output's current position when it is connected */
		if (e->x == 0 && e->y == 0) {
			struct state_output *so = state_find(outs, nouts, name);
			if (so != NULL && so->enabled) {
				e->x = so->x;
				e->y = so->y;
			}
		}
		if (position_taken(entries, num, name, e->x, e->y)) {
			int nx = 0, ny = 0;
			position_next(entries, num, name, outs, nouts, &nx, &ny);
			fprintf(stderr,
				"guibuxwm-output: %s: %dx%d is taken by another output; "
				"placing at %dx%d (extend, not mirror)\n",
				name, e->x, e->y, nx, ny);
			e->x = nx;
			e->y = ny;
		}
	} else if (!strcmp(cmd, "disable")) {
		if (npos > 2) {
			fprintf(stderr, "guibuxwm-output: unexpected argument '%s'\n",
				argv[pos[2]]);
			return 1;
		}
		e->disabled = true;
	} else {
		usage();
	}

	/* warn when the requested mode is not in the output's mode list */
	if (!e->disabled && e->mode_w > 0) {
		struct state_output *so = state_find(outs, nouts, name);
		if (so != NULL && so->modes[0] != '\0' &&
				!mode_list_has(so->modes, e->mode_w, e->mode_h)) {
			char pat[32];
			snprintf(pat, sizeof(pat), "%dx%d", e->mode_w, e->mode_h);
			fprintf(stderr, "guibuxwm-output: warning: %s does not "
				"list %s; the compositor will keep its current mode\n",
				name, pat);
		}
	}

	char spec[2048];
	outputs_spec_format(entries, num, spec, sizeof(spec));
	if (outputs_config_write(config, spec) != 0) {
		die("cannot write config");
	}
	printf("guibuxwm-output: saved to %s: %s\n", config, spec);
	if (!no_apply) {
		return send_apply(pid);
	}
	return 0;
}
