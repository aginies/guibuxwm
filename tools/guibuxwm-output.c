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
//   guibuxwm-output set NAME X Y [--mode WxH] [--transform normal|90|180|270]
//   guibuxwm-output enable NAME
//   guibuxwm-output disable NAME
//   guibuxwm-output apply
//
// Options:
//   -c FILE      config file (default: GUIBUX_CONFIG or
//                ~/.config/guibuxwm/config)
//   --no-apply   save to the config without signaling the compositor

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_ENTRIES 16
#define MAX_MODES 512

struct entry {
	char name[64];
	int x, y;
	int mode_w, mode_h;
	int transform;  /* -1 = unset, else WL_OUTPUT_TRANSFORM_* (0..3) */
	bool disabled;
};

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
		"                        ~/.config/guibuxwm/config)\n");
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

static const char *transform_name(int t) {
	switch (t) {
	case 1: return "90";
	case 2: return "180";
	case 3: return "270";
	case 4: return "flipped";
	case 5: return "90 flipped";
	case 6: return "180 flipped";
	default: return "normal";
	}
}

static int transform_from_name(const char *s) {
	if (!strcmp(s, "normal")) return 0;
	if (!strcmp(s, "90")) return 1;
	if (!strcmp(s, "180")) return 2;
	if (!strcmp(s, "270")) return 3;
	return -1;
}

/* spec: NAME@XxY[:WxH[:ROT]],NAME@off,... into entries */
static int spec_parse(const char *spec, struct entry *arr, int cap, int *num) {
	*num = 0;
	if (spec == NULL || spec[0] == '\0' || !strcmp(spec, "auto")) {
		return 0;
	}
	char *copy = strdup(spec);
	if (copy == NULL) {
		die("out of memory");
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
			tok = strtok_r(NULL, ",", &save)) {
		char *at = strchr(tok, '@');
		if (at == NULL) {
			fprintf(stderr, "guibuxwm-output: bad entry '%s' in config, "
				"skipping\n", tok);
			continue;
		}
		*at = '\0';
		if (*tok == '\0' || strlen(tok) >= sizeof(arr[0].name)) {
			fprintf(stderr, "guibuxwm-output: bad name in '%s', skipping\n",
				tok);
			continue;
		}
		char *pos = at + 1;
		if (*num >= cap) {
			die("too many outputs in config (max 16)");
		}
		struct entry *e = &arr[(*num)++];
		snprintf(e->name, sizeof(e->name), "%s", tok);
		e->x = 0;
		e->y = 0;
		e->mode_w = 0;
		e->mode_h = 0;
		e->transform = -1;
		e->disabled = false;
		if (strcasecmp(pos, "off") == 0) {
			e->disabled = true;
			continue;
		}
		if (sscanf(pos, "%dx%d", &e->x, &e->y) != 2) {
			fprintf(stderr, "guibuxwm-output: bad position in '%s', "
				"skipping\n", tok);
			(*num)--;
			continue;
		}
		char *sec = strchr(pos, ':');
		while (sec != NULL) {
			sec[0] = '\0';
			sec++;
			int w = 0, h = 0;
			if (sscanf(sec, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				e->mode_w = w;
				e->mode_h = h;
			} else {
				int t = transform_from_name(sec);
				if (t < 0) {
					fprintf(stderr, "guibuxwm-output: bad section '%s' "
						"in config, ignoring rest\n", sec);
					break;
				}
				e->transform = t;
			}
			sec = strchr(sec, ':');
		}
	}
	free(copy);
	return *num;
}

static void spec_format(const struct entry *arr, int num, char *buf, size_t n) {
	buf[0] = '\0';
	for (int i = 0; i < num; i++) {
		const struct entry *e = &arr[i];
		/* bounded copy: -Wformat-truncation false-positives on e->name
		 * (tracks the whole entries array) */
		char nm[64];
		size_t nlen = strlen(e->name);
		if (nlen >= sizeof(nm)) {
			nlen = sizeof(nm) - 1;
		}
		memcpy(nm, e->name, nlen);
		nm[nlen] = '\0';
		char part[256];
		if (e->disabled) {
			snprintf(part, sizeof(part), "%s@off", nm);
		} else {
			int off = snprintf(part, sizeof(part), "%s@%dx%d", nm,
				e->x, e->y);
			if (e->mode_w > 0 && e->mode_h > 0) {
				off += snprintf(part + off, sizeof(part) - off, ":%dx%d",
					e->mode_w, e->mode_h);
			}
			if (e->transform >= 0) {
				off += snprintf(part + off, sizeof(part) - off, ":%s",
					transform_name(e->transform));
			}
		}
		if (i > 0) {
			strncat(buf, ",", n - strlen(buf) - 1);
		}
		strncat(buf, part, n - strlen(buf) - 1);
	}
}

/* read the `outputs` value from the config; 1 = found */
static int config_read_outputs(const char *path, char *buf, size_t n) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	char line[1024];
	int found = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0' || *p == '#' || *p == '\n') {
			continue;
		}
		char *eq = strchr(p, '=');
		if (eq == NULL) {
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
		if (!strcmp(key, "outputs")) {
			snprintf(buf, n, "%s", val);
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

/* replace the `outputs` line, or append it when absent */
static void config_write_outputs(const char *path, const char *spec) {
	FILE *f = fopen(path, "r");
	char *old = NULL;
	long size = 0;
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		old = malloc(size + 1);
		if (old != NULL) {
			size_t r = fread(old, 1, size, f);
			old[r] = '\0';
		}
		fclose(f);
	}
	char *out = malloc(size + 4096);
	if (out == NULL) {
		free(old);
		die("out of memory");
	}
	out[0] = '\0';
	if (old != NULL) {
		char *save = NULL;
		for (char *line = strtok_r(old, "\n", &save); line != NULL;
				line = strtok_r(NULL, "\n", &save)) {
			char *p = line;
			while (*p == ' ' || *p == '\t') {
				p++;
			}
			char *eq = (*p == '#' || *p == '\0') ? NULL : strchr(p, '=');
			int is_outputs = 0;
			if (eq != NULL) {
				/* compare without mutating the line: it is copied back
				 * verbatim for every non-outputs line */
				size_t klen = eq - p;
				while (klen > 0 && (p[klen - 1] == ' ' ||
						p[klen - 1] == '\t')) {
					klen--;
				}
				is_outputs = (klen == strlen("outputs") &&
					strncmp(p, "outputs", klen) == 0);
			}
			if (is_outputs) {
				continue;
			}
			strncat(out, line, 4096 + size - strlen(out));
			strncat(out, "\n", 4096 + size - strlen(out));
		}
	}
	snprintf(out + strlen(out), 4096 + size - strlen(out),
		"outputs = %s\n", spec);
	FILE *w = fopen(path, "w");
	if (w == NULL) {
		free(old);
		free(out);
		die("cannot write config");
	}
	fputs(out, w);
	fclose(w);
	free(old);
	free(out);
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
static int load_entries(const char *config, struct entry *arr, int *num,
		char *spec_buf, size_t spec_n) {
	if (!config_read_outputs(config, spec_buf, spec_n)) {
		spec_buf[0] = '\0';
		*num = 0;
		return 0;
	}
	return spec_parse(spec_buf, arr, MAX_ENTRIES, num);
}

static struct entry *entry_upsert(struct entry *arr, int *num,
		const char *name) {
	for (int i = 0; i < *num; i++) {
		if (!strcmp(arr[i].name, name)) {
			return &arr[i];
		}
	}
	if (*num >= MAX_ENTRIES) {
		die("too many outputs (max 16)");
	}
	struct entry *e = &arr[(*num)++];
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
					(transform = transform_from_name(argv[++j])) < 0) {
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
				o->name, pos, mode, transform_name(o->transform),
				o->enabled ? "on" : "off");
		}
		printf("\nAvailable modes:\n");
		for (int j = 0; j < nouts; j++) {
			printf("  %-16s %s\n", outs[j].name,
				outs[j].modes[0] != '\0' ? outs[j].modes : "(none)");
		}
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
	struct entry entries[MAX_ENTRIES];
	int num = 0;
	load_entries(config, entries, &num, spec_buf, sizeof(spec_buf));
	struct entry *e = entry_upsert(entries, &num, name);

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
	spec_format(entries, num, spec, sizeof(spec));
	config_write_outputs(config, spec);
	printf("guibuxwm-output: saved to %s: %s\n", config, spec);
	if (!no_apply) {
		return send_apply(pid);
	}
	return 0;
}
