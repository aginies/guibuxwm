// `outputs` config spec: shared between the guibuxwm-output tool and the
// compositor's outputs panel. Format: NAME@XxY[:WxH[:ROT]],NAME@off,...
// Entry order in the spec is the user-managed screen order and is
// preserved by parse/format.

#include "outputs-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *outputs_transform_name(int t) {
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

int outputs_transform_from_name(const char *s) {
	if (!strcmp(s, "normal")) return 0;
	if (!strcmp(s, "90")) return 1;
	if (!strcmp(s, "180")) return 2;
	if (!strcmp(s, "270")) return 3;
	return -1;
}

int outputs_spec_parse(const char *spec, struct guibux_output_entry *arr,
		int cap, int *num) {
	*num = 0;
	if (spec == NULL || spec[0] == '\0' || !strcmp(spec, "auto")) {
		return 0;
	}
	char *copy = strdup(spec);
	if (copy == NULL) {
		return -1;
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, ",", &save); tok != NULL;
			tok = strtok_r(NULL, ",", &save)) {
		char *at = strchr(tok, '@');
		if (at == NULL) {
			fprintf(stderr, "outputs: bad entry '%s' in config, skipping\n",
				tok);
			continue;
		}
		*at = '\0';
		if (*tok == '\0' || strlen(tok) >= sizeof(arr[0].name)) {
			fprintf(stderr, "outputs: bad name in '%s', skipping\n", tok);
			continue;
		}
		char *pos = at + 1;
		if (*num >= cap) {
			free(copy);
			return -1;
		}
		struct guibux_output_entry *e = &arr[(*num)++];
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
			fprintf(stderr, "outputs: bad position in '%s', skipping\n", tok);
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
				int t = outputs_transform_from_name(sec);
				if (t < 0) {
					fprintf(stderr, "outputs: bad section '%s' in config, "
						"ignoring rest\n", sec);
					break;
				}
				e->transform = t;
			}
			sec = strchr(sec, ':');
		}
	}
	free(copy);
	return 0;
}

void outputs_spec_format(const struct guibux_output_entry *arr, int num,
		char *buf, size_t n) {
	buf[0] = '\0';
	for (int i = 0; i < num; i++) {
		const struct guibux_output_entry *e = &arr[i];
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
					outputs_transform_name(e->transform));
			}
		}
		if (i > 0) {
			strncat(buf, ",", n - strlen(buf) - 1);
		}
		strncat(buf, part, n - strlen(buf) - 1);
	}
}

int outputs_config_read(const char *path, char *buf, size_t n) {
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

int outputs_config_write(const char *path, const char *spec) {
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
		return -1;
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
		return -1;
	}
	fputs(out, w);
	fclose(w);
	free(old);
	free(out);
	return 0;
}
