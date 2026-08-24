#ifndef GUIBUX_OUTPUTS_CONFIG_H
#define GUIBUX_OUTPUTS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define OUTPUTS_CONFIG_MAX_ENTRIES 16

/* one entry of the `outputs` config spec: NAME@XxY[:WxH[:ROT]] or NAME@off */
struct guibux_output_entry {
	char name[64];
	int x, y;
	int mode_w, mode_h;
	int transform;  /* -1 = unset, else WL_OUTPUT_TRANSFORM_* (0..3) */
	bool disabled;
};

/* spec: NAME@XxY[:WxH[:ROT]],NAME@off,... into entries; entry order is
 * preserved. 0 on success, -1 on OOM or more than cap entries */
int outputs_spec_parse(const char *spec, struct guibux_output_entry *arr,
		int cap, int *num);
/* entries back into a spec string (order preserved) */
void outputs_spec_format(const struct guibux_output_entry *arr, int num,
		char *buf, size_t n);
/* read the `outputs` value from a config file; 1 = found */
int outputs_config_read(const char *path, char *buf, size_t n);
/* replace the `outputs` line, or append it when absent; 0 on success */
int outputs_config_write(const char *path, const char *spec);
const char *outputs_transform_name(int t);
int outputs_transform_from_name(const char *s);

#endif
