#define _GNU_SOURCE
#include "guibuxwm.h"
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <arpa/inet.h>

/*
 * sysinfo - system information via D-Bus
 *
 * Queries:
 *   NetworkManager (org.freedesktop.NetworkManager)
 *     - Devices for interface name, SSID, signal
 *   UPower (org.freedesktop.UPower) - extensible for battery
 *   pactl (subprocess)
 *     - default sink/source volume + mute (PulseAudio / PipeWire)
 *
 * A worker thread owns a dedicated system-bus connection and polls
 * NetworkManager every few seconds; the main loop never blocks on
 * D-Bus. Results are published to si->network / si->battery under
 * si->lock; readers copy them out with sysinfo_get().
 *
 * Usage:
 *   sysinfo_init(&server)   - start the worker thread
 *   sysinfo_get(&server.sysinfo, ...) - read current values
 *   sysinfo_destroy(&server) - stop the worker, close the bus
 */

static const char *NM_SERVICE = "org.freedesktop.NetworkManager";
static const char *NM_PATH = "/org/freedesktop/NetworkManager";
static const char *NM_IFACE = "org.freedesktop.NetworkManager";
static const char *NM_DEVICE_IFACE = "org.freedesktop.NetworkManager.Device";
static const char *NM_WIFI_IFACE = "org.freedesktop.NetworkManager.Device.Wireless";

/* D-Bus timeout in milliseconds */
#define DBUS_TIMEOUT_MS 3000
#define SYSINFO_INTERVAL_SEC 5
/* how often to re-poll network state even without an NM change signal
 * (fallback for missed signals); battery/audio still use
 * SYSINFO_INTERVAL_SEC */
#define NET_POLL_FALLBACK_SEC 30

/*
 * Run a command through /bin/sh and capture its stdout.
 * Returns a newly allocated NUL-terminated string (possibly empty),
 * or NULL on spawn failure. Caller must free.
 * Runs in the worker thread only: blocking read is fine here.
 */
static char *
run_capture(const char *cmd)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char *buf = malloc(4096);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    size_t total = 0;
    while (total < 4095) {
        ssize_t n = read(fds[0], buf + total, 4095 - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
    buf[total] = '\0';
    return buf;
}

/*
 * Parse the first "NN%" in s (pactl volume output), -1 on failure.
 * Example: "Volume: front-left: 65536 / 100% / 0,00 dB" -> 100
 */
static int
parse_percent(const char *s)
{
    const char *p = strchr(s, '%');
    if (!p) {
        return -1;
    }
    /* start of the digit run immediately before '%' */
    const char *e = p;
    while (e > s && e[-1] >= '0' && e[-1] <= '9') {
        e--;
    }
    if (e == p) {
        return -1;
    }
    int v = 0;
    for (const char *q = e; q < p; q++) {
        v = v * 10 + (*q - '0');
    }
    return v;
}

/*
 * Send a Properties.Get call and return the reply, or NULL on error.
 * Caller must unref the reply.
 */
static DBusMessage *
dbus_properties_get(DBusConnection *conn, const char *service,
                    const char *object_path,
                    const char *interface, const char *property,
                    DBusError *err)
{
    DBusMessage *msg = dbus_message_new_method_call(
        service, object_path,
        "org.freedesktop.DBus.Properties", "Get");
    if (!msg) {
        return NULL;
    }

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &interface,
        DBUS_TYPE_STRING, &property,
        DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, DBUS_TIMEOUT_MS, err);
    dbus_message_unref(msg);
    return reply;
}

static void
free_path_array(char **arr)
{
    if (!arr) return;
    for (int i = 0; arr[i]; i++) {
        free(arr[i]);
    }
    free(arr);
}

/*
 * Copy an array of object paths from an iterator positioned at an
 * array element into a newly allocated NUL-terminated string array.
 * Returns NULL on allocation failure.
 */
static char **
copy_object_path_array(DBusMessageIter *arr)
{
    int count = 0;
    int cap = 8;
    char **result = malloc(cap * sizeof(char *));
    if (!result) {
        return NULL;
    }
    while (dbus_message_iter_get_arg_type(arr) == DBUS_TYPE_OBJECT_PATH) {
        const char *path = NULL;
        dbus_message_iter_get_basic(arr, &path);
        /* keep room for the NUL terminator after every write */
        if (count + 1 >= cap) {
            int ncap = cap * 2;
            char **tmp = realloc(result, ncap * sizeof(char *));
            if (!tmp) {
                free_path_array(result);
                return NULL;
            }
            result = tmp;
            cap = ncap;
        }
        result[count++] = strdup(path);
        dbus_message_iter_next(arr);
    }
    result[count] = NULL;
    return result;
}

/*
 * Get an array of object paths property (e.g. NetworkManager Devices).
 * Returns newly allocated array of strings (NULL-terminated), or NULL.
 * Caller must free each string and the array itself.
 */
static char **
dbus_get_property_path_array(DBusConnection *conn, const char *service,
                              const char *object_path,
                              const char *interface, const char *property,
                              DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return NULL;
    }

    DBusMessageIter iter, variant, arr;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    char **result = NULL;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&variant, &arr);
        result = copy_object_path_array(&arr);
    }

    dbus_message_unref(reply);
    return result;
}

/*
 * Call a no-argument method that returns an array of object paths
 * (e.g. UPower EnumerateDevices). Returns newly allocated array of
 * strings (NULL-terminated), or NULL. Caller must free each string and
 * the array itself.
 */
static char **
dbus_call_path_array(DBusConnection *conn, const char *service,
                     const char *object_path,
                     const char *interface, const char *method,
                     DBusError *err)
{
    DBusMessage *msg = dbus_message_new_method_call(
        service, object_path, interface, method);
    if (!msg) {
        return NULL;
    }

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, DBUS_TIMEOUT_MS, err);
    dbus_message_unref(msg);
    if (!reply) {
        return NULL;
    }

    DBusMessageIter iter, arr;
    dbus_message_iter_init(reply, &iter);

    char **result = NULL;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &arr);
        result = copy_object_path_array(&arr);
    }

    dbus_message_unref(reply);
    return result;
}

/*
 * Cache of all properties for one object, fetched with a single
 * Properties.GetAll call instead of one Properties.Get per property.
 * The variant iterators point into the reply message, which the cache
 * owns; prop_cache_free() unrefs it.
 */
struct prop_cache {
    struct {
        const char *name;
        DBusMessageIter variant;
    } *props;
    int count;
    DBusMessage *reply;
};

static void
prop_cache_free(struct prop_cache *c)
{
    if (!c->props) {
        return;
    }
    for (int i = 0; i < c->count; i++) {
        free((char *)c->props[i].name);
    }
    free(c->props);
    c->props = NULL;
    c->count = 0;
    if (c->reply) {
        dbus_message_unref(c->reply);
        c->reply = NULL;
    }
}

/*
 * Fetch every property of an object (optionally restricted to one
 * interface) in a single Properties.GetAll round-trip. Returns false
 * on D-Bus error; the cache is left empty.
 */
static bool
prop_cache_get_all(DBusConnection *conn, const char *service,
                   const char *object_path, const char *interface,
                   struct prop_cache *c, DBusError *err)
{
    memset(c, 0, sizeof(*c));
    DBusMessage *msg = dbus_message_new_method_call(
        service, object_path,
        "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg) {
        return false;
    }
    /* NULL interface = all interfaces; append_args takes the pointer
     * to the string, so a NULL value would crash the UTF-8 check */
    if (interface) {
        dbus_message_append_args(msg,
            DBUS_TYPE_STRING, &interface,
            DBUS_TYPE_INVALID);
    }
    c->reply = dbus_connection_send_with_reply_and_block(
        conn, msg, DBUS_TIMEOUT_MS, err);
    dbus_message_unref(msg);
    if (!c->reply) {
        return false;
    }

    DBusMessageIter iter, dict, val;
    dbus_message_iter_init(c->reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        prop_cache_free(c);
        return false;
    }
    dbus_message_iter_recurse(&iter, &iter);
    int count = 0;
    while (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_DICT_ENTRY) {
        count++;
        dbus_message_iter_next(&iter);
    }
    c->props = calloc(count ? count : 1, sizeof(*c->props));
    if (!c->props) {
        prop_cache_free(c);
        return false;
    }
    dbus_message_iter_init(c->reply, &iter);
    dbus_message_iter_recurse(&iter, &iter);
    int i = 0;
    while (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_DICT_ENTRY
           && i < count) {
        dbus_message_iter_recurse(&iter, &dict);
        const char *name = NULL;
        dbus_message_iter_get_basic(&dict, &name);
        dbus_message_iter_next(&dict);
        if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&dict, &val);
        } else {
            val = dict;
        }
        c->props[i].name = strdup(name ? name : "");
        c->props[i].variant = val;
        i++;
        dbus_message_iter_next(&iter);
    }
    c->count = i;
    return true;
}

/*
 * Find a property in the cache. Returns NULL if absent.
 */
static DBusMessageIter *
prop_cache_find(const struct prop_cache *c, const char *name)
{
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->props[i].name, name) == 0) {
            return &c->props[i].variant;
        }
    }
    return NULL;
}

static dbus_uint32_t
prop_cache_uint32(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return 0;
    }
    dbus_uint32_t val = 0;
    dbus_uint32_t type = dbus_message_iter_get_arg_type(v);
    if (type == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(v, &val);
    } else if (type == DBUS_TYPE_BYTE) {
        unsigned char b = 0;
        dbus_message_iter_get_basic(v, &b);
        val = b;
    } else if (type == DBUS_TYPE_UINT16) {
        dbus_uint16_t u16 = 0;
        dbus_message_iter_get_basic(v, &u16);
        val = u16;
    }
    return val;
}

static int64_t
prop_cache_int64(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return 0;
    }
    int64_t val = 0;
    dbus_uint32_t type = dbus_message_iter_get_arg_type(v);
    if (type == DBUS_TYPE_INT64) {
        dbus_int64_t x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    } else if (type == DBUS_TYPE_INT32) {
        dbus_int32_t x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    } else if (type == DBUS_TYPE_INT16) {
        dbus_int16_t x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    } else if (type == DBUS_TYPE_UINT32) {
        dbus_uint32_t x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    } else if (type == DBUS_TYPE_UINT16) {
        dbus_uint16_t x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    } else if (type == DBUS_TYPE_BYTE) {
        unsigned char x;
        dbus_message_iter_get_basic(v, &x);
        val = x;
    }
    return val;
}

static double
prop_cache_double(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return -1.0;
    }
    double val = -1.0;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_DOUBLE) {
        dbus_message_iter_get_basic(v, &val);
    }
    return val;
}

static char *
prop_cache_string(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return NULL;
    }
    char *result = NULL;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(v, &s);
        result = strdup(s);
    }
    return result;
}

static char *
prop_cache_byte_array(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return NULL;
    }
    char *result = NULL;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr = *v;
        dbus_message_iter_recurse(&arr, &arr);
        char buf[256] = {0};
        int len = 0;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE &&
               len < 255) {
            unsigned char b = 0;
            dbus_message_iter_get_basic(&arr, &b);
            buf[len++] = b;
            dbus_message_iter_next(&arr);
        }
        if (len > 0) {
            result = strdup(buf);
        }
    }
    return result;
}

static char *
prop_cache_object_path(const struct prop_cache *c, const char *name)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return NULL;
    }
    char *result = NULL;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_OBJECT_PATH) {
        const char *p = NULL;
        dbus_message_iter_get_basic(v, &p);
        if (p && p[0] != '\0') {
            result = strdup(p);
        }
    }
    return result;
}

/*
 * First address from a cached AddressData property (aa{sv} with
 * "address" (s) entries). Fills addr (4 or 16 bytes), sets *af.
 * Returns false on failure or empty array.
 */
static bool
prop_cache_first_address(const struct prop_cache *c, const char *name,
                         void *addr, int *af)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        return false;
    }
    bool ok = false;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr = *v;
        dbus_message_iter_recurse(&arr, &arr);
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&arr, &arr);
        }
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict, val;
            dbus_message_iter_recurse(&arr, &dict);
            while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
                const char *key = NULL;
                dbus_message_iter_get_basic(&dict, &key);
                dbus_message_iter_next(&dict);
                if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&dict, &val);
                } else {
                    val = dict;
                }
                if (key && strcmp(key, "address") == 0 &&
                        dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&val, &s);
                    struct in_addr a4;
                    struct in6_addr a6;
                    if (inet_pton(AF_INET, s, &a4) == 1) {
                        memcpy(addr, &a4, 4);
                        *af = AF_INET;
                        ok = true;
                    } else if (inet_pton(AF_INET6, s, &a6) == 1) {
                        memcpy(addr, &a6, 16);
                        *af = AF_INET6;
                        ok = true;
                    }
                    break;
                }
                dbus_message_iter_next(&dict);
            }
        }
    }
    return ok;
}

/*
 * DNS servers from a cached NameserverData property (aa{sv} with
 * "address" (s) entries). Fills out (NUL-terminated, comma-separated).
 * Returns false on failure or no DNS.
 */
static bool
prop_cache_dns(const struct prop_cache *c, const char *name,
               char *out, size_t out_size)
{
    DBusMessageIter *v = prop_cache_find(c, name);
    if (!v) {
        out[0] = '\0';
        return false;
    }
    bool ok = false;
    out[0] = '\0';
    int len = 0;
    if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr = *v;
        dbus_message_iter_recurse(&arr, &arr);
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&arr, &arr);
        }
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict, val;
            dbus_message_iter_recurse(&arr, &dict);
            while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
                const char *key = NULL;
                dbus_message_iter_get_basic(&dict, &key);
                dbus_message_iter_next(&dict);
                if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&dict, &val);
                } else {
                    val = dict;
                }
                if (key && strcmp(key, "address") == 0 &&
                        dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&val, &s);
                    len += snprintf(out + len, out_size - (size_t)len,
                        "%s%s", len ? ", " : "", s);
                    if (len >= (int)out_size - 1) {
                        len = (int)out_size - 1;
                        break;
                    }
                    ok = true;
                    break;
                }
                dbus_message_iter_next(&dict);
            }
            dbus_message_iter_next(&arr);
            if (len >= (int)out_size - 1) {
                break;
            }
        }
    }
    return ok;
}

/*
 * Get the gateway for an interface from the kernel route table via
 * `ip -4 -o route show dev <iface>`. Prefers the default route's "via"
 * address; falls back to the first "via" address of any route. Used when
 * NM's Gateway property is empty (e.g. tunnels, point-to-point links).
 * Returns false on failure or no gateway.
 */
static bool
route_gateway(const char *iface, unsigned char *gw, int *af)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "ip -4 -o route show dev %s 2>/dev/null", iface);
    char *out = run_capture(cmd);
    if (!out) {
        return false;
    }
    bool ok = false;
    char fallback[INET_ADDRSTRLEN] = {0};
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        const char *via = strstr(line, " via ");
        if (!via) {
            continue;
        }
        via += 5;
        char addr[INET_ADDRSTRLEN] = {0};
        int n = 0;
        while (via[n] && via[n] != ' ' && n < (int)sizeof(addr) - 1) {
            addr[n] = via[n];
            n++;
        }
        addr[n] = '\0';
        struct in_addr a4;
        if (inet_pton(AF_INET, addr, &a4) != 1) {
            continue;
        }
        if (strncmp(line, "default", 7) == 0) {
            memcpy(gw, &a4, 4);
            *af = AF_INET;
            ok = true;
            break;
        }
        if (fallback[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "%s", addr);
        }
    }
    if (!ok && fallback[0] != '\0') {
        struct in_addr a4;
        if (inet_pton(AF_INET, fallback, &a4) == 1) {
            memcpy(gw, &a4, 4);
            *af = AF_INET;
            ok = true;
        }
    }
    free(out);
    return ok;
}

/*
 * Append a formatted string to the display buffer, clamped so the
 * result always fits and stays NUL-terminated.
 */
static int
display_append(char *buf, size_t size, int len, const char *fmt, ...)
{
    if (len >= (int)size) {
        return (int)size - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int wr = vsnprintf(buf + len, size - (size_t)len, fmt, ap);
    va_end(ap);
    if (wr > 0) {
        len += wr;
    }
    if (len >= (int)size) {
        len = (int)size - 1;
    }
    return len;
}

static void
sysinfo_set_network(struct guibux_sysinfo *si, const char *s,
                    const struct guibux_net_iface *ifaces, int count)
{
    pthread_mutex_lock(&si->lock);
    snprintf(si->network, sizeof(si->network), "%s", s);
    si->net_iface_count = 0;
    for (int i = 0; i < count && i < NET_IFACES_MAX; i++) {
        memcpy(&si->net_ifaces[i], &ifaces[i], sizeof(struct guibux_net_iface));
        si->net_iface_count++;
    }
    pthread_mutex_unlock(&si->lock);
}

/*
 * Update network status from NetworkManager via D-Bus.
 * Shows all connected interfaces (except loopback).
 * WiFi: "SSID 85%", Ethernet: "eth0", etc.
 * One Properties.GetAll per object (device, wifi, ip config) instead
 * of one Properties.Get per property.
 */
static void
sysinfo_update_network(struct guibux_sysinfo *si)
{
    if (!si->nm_available || !si->system_bus) {
        sysinfo_set_network(si, "NM", NULL, 0);
        return;
    }

    DBusError err = DBUS_ERROR_INIT;

    /* Get all devices directly from NM */
    char **all_devices = dbus_get_property_path_array(
        si->system_bus, NM_SERVICE, NM_PATH, NM_IFACE, "Devices", &err);
    if (dbus_error_is_set(&err)) {
        wlr_log(WLR_INFO, "sysinfo: Devices failed: %s", err.message);
        dbus_error_free(&err);
        sysinfo_set_network(si, "NM", NULL, 0);
        return;
    }

    if (!all_devices || !all_devices[0]) {
        free_path_array(all_devices);
        sysinfo_set_network(si, "No net", NULL, 0);
        return;
    }

    /* Collect display strings + per-device details for each connected
     * non-loopback device */
    char display[128] = {0};
    int display_len = 0;
    bool any_connected = false;
    struct guibux_net_iface ifaces[NET_IFACES_MAX];
    int iface_count = 0;

    for (int i = 0; all_devices[i]; i++) {
        const char *dev_path = all_devices[i];

        struct prop_cache dev;
        if (!prop_cache_get_all(si->system_bus, NM_SERVICE, dev_path,
                NM_DEVICE_IFACE, &dev, &err)) {
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            continue;
        }

        dbus_uint32_t dev_type = prop_cache_uint32(&dev, "DeviceType");

        /* Skip loopback (type 32) */
        if (dev_type == 32) {
            prop_cache_free(&dev);
            continue;
        }

        /* Get device state - 110 = activated */
        dbus_uint32_t dev_state = prop_cache_uint32(&dev, "State");

        /* Only show devices that are not disconnected (state >= 50) */
        if (dev_state < 50) {
            prop_cache_free(&dev);
            continue;
        }

        any_connected = true;

        char *iface = prop_cache_string(&dev, "Interface");

        char label[64] = {0};
        if (dev_type == 2) {
            /* WiFi: get SSID and signal from the Wireless interface */
            struct prop_cache wifi;
            char *ssid = NULL;
            dbus_uint32_t strength = 0;
            if (prop_cache_get_all(si->system_bus, NM_SERVICE, dev_path,
                    NM_WIFI_IFACE, &wifi, &err)) {
                if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                ssid = prop_cache_byte_array(&wifi, "Ssid");
                strength = prop_cache_uint32(&wifi, "Strength");
                prop_cache_free(&wifi);
            } else {
                dbus_error_free(&err);
            }

            if (ssid && ssid[0] != '\0') {
                snprintf(label, sizeof(label), "%s %u%%", ssid, strength);
                display_len = display_append(display, sizeof(display),
                    display_len, "%s%s",
                    display_len ? " " : "", label);
            } else if (iface) {
                snprintf(label, sizeof(label), "%s", iface);
                display_len = display_append(display, sizeof(display),
                    display_len, "%s%s",
                    display_len ? " " : "", label);
            }

            free(ssid);
        } else if (iface) {
            snprintf(label, sizeof(label), "%s", iface);
            display_len = display_append(display, sizeof(display),
                display_len, "%s%s",
                display_len ? " " : "", label);
        }

        /* Per-device IP/DNS/GW from the IP config (v4 preferred, v6
         * fallback); unknown fields stay empty */
        if (iface_count < NET_IFACES_MAX && label[0] != '\0') {
            struct guibux_net_iface *ni = &ifaces[iface_count];
            memset(ni, 0, sizeof(*ni));
            snprintf(ni->label, sizeof(ni->label), "%s", label);
            ni->is_wifi = (dev_type == 2);

            char *ipcfg = prop_cache_object_path(&dev, "Ip4Config");
            const char *cfg_iface =
                "org.freedesktop.NetworkManager.IP4Config";
            if (ipcfg == NULL) {
                ipcfg = prop_cache_object_path(&dev, "Ip6Config");
                cfg_iface = "org.freedesktop.NetworkManager.IP6Config";
            }
            if (ipcfg != NULL) {
                struct prop_cache cfg;
                if (prop_cache_get_all(si->system_bus, NM_SERVICE, ipcfg,
                        cfg_iface, &cfg, &err)) {
                    if (dbus_error_is_set(&err)) {
                        dbus_error_free(&err);
                    }
                    unsigned char addr[16];
                    int af = 0;
                    if (prop_cache_first_address(&cfg, "AddressData",
                            addr, &af)) {
                        inet_ntop(af, addr, ni->ip, sizeof(ni->ip));
                    }
                    unsigned char gw_bytes[16];
                    int gw_af = 0;
                    char *gw = prop_cache_string(&cfg, "Gateway");
                    if (gw) {
                        struct in_addr a4;
                        struct in6_addr a6;
                        if (inet_pton(AF_INET, gw, &a4) == 1) {
                            memcpy(gw_bytes, &a4, 4);
                            gw_af = AF_INET;
                        } else if (inet_pton(AF_INET6, gw, &a6) == 1) {
                            memcpy(gw_bytes, &a6, 16);
                            gw_af = AF_INET6;
                        }
                    }
                    free(gw);
                    if (gw_af != 0) {
                        inet_ntop(gw_af, gw_bytes, ni->gw, sizeof(ni->gw));
                    }
                    /* NM's Gateway is empty for tunnels / point-to-point
                     * links; fall back to the kernel route table */
                    if (ni->gw[0] == '\0' && iface != NULL) {
                        route_gateway(iface, gw_bytes, &gw_af);
                        if (gw_af != 0) {
                            inet_ntop(gw_af, gw_bytes, ni->gw,
                                sizeof(ni->gw));
                        }
                    }
                    prop_cache_dns(&cfg, "NameserverData", ni->dns,
                        sizeof(ni->dns));
                    prop_cache_free(&cfg);
                } else {
                    dbus_error_free(&err);
                }
                free(ipcfg);
            }
            iface_count++;
        }

        prop_cache_free(&dev);
        free(iface);
    }

    free_path_array(all_devices);

    if (!any_connected) {
        sysinfo_set_network(si, "No net", NULL, 0);
    } else {
        sysinfo_set_network(si, display, ifaces, iface_count);
    }
}

/*
 * UPower battery query
 */
static const char *UPower_SERVICE = "org.freedesktop.UPower";
static const char *UPower_IFACE = "org.freedesktop.UPower";
static const char *UPower_PATH = "/org/freedesktop/UPower";
static const char *UPower_DEVICE_IFACE = "org.freedesktop.UPower.Device";
/* UPower device Type enum: 0=Unknown, 1=LinePower, 2=Battery, ... */
#define UP_DEVICE_TYPE_BATTERY 2

/*
 * Get battery percentage from UPower via D-Bus.
 * EnumerateDevices is a method (the root object has no Devices
 * property); find the first device whose Type is a battery (uint32
 * enum) and read its Percentage (double). Also reads State and the
 * TimeToEmpty / TimeToFull estimates for the topbar tooltip.
 * Publishes a formatted string like "85%" or empty on failure.
 */
static void
sysinfo_update_battery(struct guibux_sysinfo *si)
{
    if (!si->system_bus) {
        pthread_mutex_lock(&si->lock);
        si->battery[0] = '\0';
        si->bat_state = 0;
        si->bat_eta_sec = 0;
        pthread_mutex_unlock(&si->lock);
        return;
    }

    DBusError err = DBUS_ERROR_INIT;
    char **devices = dbus_call_path_array(
        si->system_bus, UPower_SERVICE, UPower_PATH,
        UPower_IFACE, "EnumerateDevices", &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
    }

    int pct = -1;
    int state = 0;
    int eta = 0;
    if (devices) {
        for (int i = 0; devices[i]; i++) {
            const char *dev_path = devices[i];

            struct prop_cache bat;
            if (!prop_cache_get_all(si->system_bus, UPower_SERVICE,
                    dev_path, UPower_DEVICE_IFACE, &bat, &err)) {
                if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                continue;
            }

            dbus_uint32_t type = prop_cache_uint32(&bat, "Type");
            if (type != UP_DEVICE_TYPE_BATTERY) {
                prop_cache_free(&bat);
                continue;
            }

            double d = prop_cache_double(&bat, "Percentage");
            if (d < 0.0 || d > 100.0) {
                prop_cache_free(&bat);
                continue;
            }
            pct = (int)(d + 0.5);

            state = (int)prop_cache_uint32(&bat, "State");
            int64_t tte = prop_cache_int64(&bat, "TimeToEmpty");
            int64_t ttf = prop_cache_int64(&bat, "TimeToFull");
            /* TimeToEmpty is only meaningful while discharging,
             * TimeToFull while charging; 0 = no estimate */
            if (state == 1) {
                eta = (int)ttf;
            } else if (state == 2) {
                eta = (int)tte;
            }
            prop_cache_free(&bat);
            break;
        }
    }
    free_path_array(devices);

    pthread_mutex_lock(&si->lock);
    if (pct >= 0) {
        snprintf(si->battery, sizeof(si->battery), "%d%%", pct);
        si->bat_state = state;
        si->bat_eta_sec = eta;
    } else {
        si->battery[0] = '\0';
        si->bat_state = 0;
        si->bat_eta_sec = 0;
    }
    pthread_mutex_unlock(&si->lock);
}

/*
 * Parse combined "pactl get-<sink|source>-volume X; pactl
 * get-<sink|source>-mute X" output for the volume percent and the
 * mute flag. The volume line carries "NN%", the mute line "Mute:
 * yes|no"; either may be left untouched when its value is absent.
 */
static void
parse_volume_output(const char *out, int *volume, bool *muted)
{
    if (volume) {
        int v = parse_percent(out);
        if (v >= 0) {
            *volume = v;
        }
    }
    if (muted) {
        const char *m = strstr(out, "Mute:");
        if (m) {
            *muted = (strstr(m, "yes") != NULL);
        }
    }
}

/*
 * PulseAudio / PipeWire (pulse compat) on the session bus:
 * org.freedesktop.PulseAudio1
 */
static const char *PULSE_SERVICE = "org.freedesktop.PulseAudio1";
static const char *PULSE_CORE_PATH = "/org/freedesktop/PulseAudio/Core";
static const char *PULSE_CORE_IFACE = "org.freedesktop.PulseAudio1.Core";
static const char *PULSE_SINK_IFACE = "org.freedesktop.PulseAudio1.Sink";
static const char *PULSE_SOURCE_IFACE = "org.freedesktop.PulseAudio1.Source";

/*
 * Find the default sink/source object path via Core.GetSinkInputList /
 * GetSourceInputList (each entry: index, name, is_default, ...).
 * kind: "Sink" or "Source". Returns newly allocated path, or NULL.
 */
static char *
pulse_default_path(DBusConnection *conn, const char *method,
                   const char *kind, DBusError *err)
{
    DBusMessage *msg = dbus_message_new_method_call(
        PULSE_SERVICE, PULSE_CORE_PATH, PULSE_CORE_IFACE, method);
    if (!msg) {
        return NULL;
    }
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, DBUS_TIMEOUT_MS, err);
    dbus_message_unref(msg);
    if (!reply) {
        return NULL;
    }

    char *result = NULL;
    DBusMessageIter iter, entry;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &iter);
        while (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&iter, &entry);
            /* entry: u index, s name, b is_default, ... */
            dbus_uint32_t index = 0;
            const char *name = NULL;
            dbus_bool_t is_default = FALSE;
            dbus_message_iter_get_basic(&entry, &index);
            dbus_message_iter_next(&entry);
            dbus_message_iter_get_basic(&entry, &name);
            dbus_message_iter_next(&entry);
            dbus_message_iter_get_basic(&entry, &is_default);
            if (is_default) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "/org/freedesktop/PulseAudio/Core/%s/%u", kind, index);
                result = strdup(buf);
                break;
            }
            dbus_message_iter_next(&iter);
        }
    }

    dbus_message_unref(reply);
    return result;
}

/*
 * Volume percent (0-100) + mute from a PulseAudio Sink/Source object:
 * one Properties.GetAll, Volume is an array of per-channel doubles
 * (take the mean), Mute is a bool.
 */
static bool
pulse_device_volume(DBusConnection *conn, const char *path,
                    const char *iface, int *volume, bool *muted,
                    DBusError *err)
{
    struct prop_cache c;
    if (!prop_cache_get_all(conn, PULSE_SERVICE, path, iface, &c, err)) {
        return false;
    }
    bool ok = false;
    DBusMessageIter *v = prop_cache_find(&c, "Volume");
    if (v && dbus_message_iter_get_arg_type(v) == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr = *v;
        dbus_message_iter_recurse(&arr, &arr);
        double sum = 0.0;
        int n = 0;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DOUBLE) {
            double d = 0.0;
            dbus_message_iter_get_basic(&arr, &d);
            sum += d;
            n++;
            dbus_message_iter_next(&arr);
        }
        if (n > 0) {
            *volume = (int)(sum / (double)n * 100.0 + 0.5);
            ok = true;
        }
    }
    DBusMessageIter *m = prop_cache_find(&c, "Mute");
    if (m && dbus_message_iter_get_arg_type(m) == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t b = FALSE;
        dbus_message_iter_get_basic(m, &b);
        *muted = (b == TRUE);
    }
    prop_cache_free(&c);
    return ok;
}

/*
 * Read screen brightness from /sys/class/backlight (first device with
 * a brightness file). Returns percent 0-100, or -1 when no backlight
 * is exposed (e.g. headless / external display).
 */
static int
brightness_from_sysfs(void)
{
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir) {
        return -1;
    }
    int result = -1;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path),
            "/sys/class/backlight/%s/brightness", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        int cur = 0;
        if (fscanf(f, "%d", &cur) != 1) {
            fclose(f);
            continue;
        }
        fclose(f);
        snprintf(path, sizeof(path),
            "/sys/class/backlight/%s/max_brightness", e->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        int max = 0;
        if (fscanf(f, "%d", &max) != 1 || max <= 0) {
            fclose(f);
            continue;
        }
        fclose(f);
        result = (cur * 100) / max;
        break;
    }
    closedir(dir);
    return result;
}

/*
 * Update sink/source (mic) volume + mute and screen brightness.
 * Audio: PulseAudio D-Bus on the session bus (works with PulseAudio
 * and PipeWire pulse compat); falls back to pactl subprocess when
 * PulseAudio1 is not on the bus. Brightness: /sys/class/backlight;
 * falls back to brightnessctl. A failed read leaves the previous
 * values untouched; audio_available stays false until the first
 * successful read (no audio on the system -> indicator hidden).
 */
static void
sysinfo_update_audio(struct guibux_sysinfo *si)
{
    int volume = -1, mic_volume = -1;
    bool muted = false, mic_muted = false;

    if (si->pulse_available && si->session_bus) {
        DBusError err = DBUS_ERROR_INIT;
        char *sink = pulse_default_path(si->session_bus,
            "GetSinkInputList", "Sink", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
        if (sink) {
            pulse_device_volume(si->session_bus, sink, PULSE_SINK_IFACE,
                &volume, &muted, &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            free(sink);
        }
        char *source = pulse_default_path(si->session_bus,
            "GetSourceInputList", "Source", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
        if (source) {
            pulse_device_volume(si->session_bus, source,
                PULSE_SOURCE_IFACE, &mic_volume, &mic_muted, &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            free(source);
        }
    } else {
        char *out = run_capture(
            "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null; "
            "pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
        if (out) {
            parse_volume_output(out, &volume, &muted);
            free(out);
        }

        out = run_capture(
            "pactl get-source-volume @DEFAULT_SOURCE@ 2>/dev/null; "
            "pactl get-source-mute @DEFAULT_SOURCE@ 2>/dev/null");
        if (out) {
            parse_volume_output(out, &mic_volume, &mic_muted);
            free(out);
        }
    }

    int brightness = brightness_from_sysfs();
    if (brightness < 0) {
        char *out = run_capture("brightnessctl get 2>/dev/null");
        if (out) {
            brightness = parse_percent(out);
            free(out);
        }
    }

    pthread_mutex_lock(&si->lock);
    if (volume >= 0) {
        si->volume = volume;
        si->muted = muted;
        si->audio_available = true;
    }
    if (mic_volume >= 0) {
        si->mic_volume = mic_volume;
        si->mic_muted = mic_muted;
        si->audio_available = true;
    }
    if (brightness >= 0) {
        si->brightness = brightness;
        si->brightness_available = true;
    }
    pthread_mutex_unlock(&si->lock);
}

static dbus_bool_t
sysinfo_net_filter(DBusConnection *conn, DBusMessage *msg, void *data)
{
    if (dbus_message_is_signal(msg,
            "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
        struct guibux_sysinfo *si = data;
        pthread_mutex_lock(&si->lock);
        si->net_dirty = true;
        pthread_mutex_unlock(&si->lock);
        pthread_cond_signal(&si->wake);
    }
    return TRUE;
}

static void *
sysinfo_worker(void *data)
{
    struct guibux_server *server = data;
    struct guibux_sysinfo *si = &server->sysinfo;
    DBusError err = DBUS_ERROR_INIT;

    si->system_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!si->system_bus) {
        wlr_log(WLR_ERROR, "sysinfo: cannot open system bus: %s",
            dbus_error_is_set(&err) ? err.message : "unknown");
        dbus_error_free(&err);
    } else {
        wlr_log(WLR_INFO, "sysinfo: system bus connected");
        if (dbus_bus_name_has_owner(si->system_bus, NM_SERVICE, &err)) {
            pthread_mutex_lock(&si->lock);
            si->nm_available = true;
            si->net_dirty = true;
            pthread_mutex_unlock(&si->lock);
            wlr_log(WLR_INFO, "sysinfo: NetworkManager active");
            /* poll network state only when NM reports a change, not
             * every interval: an idle desktop then does zero network
             * D-Bus work. The periodic loop below is the fallback for
             * missed signals. */
            const char *rule =
                "type='signal',"
                "interface='org.freedesktop.DBus.Properties',"
                "member='PropertiesChanged',"
                "path_namespace='/org/freedesktop/NetworkManager'";
             dbus_bus_add_match(si->system_bus, rule, &err);
             if (dbus_connection_add_filter(si->system_bus,
                     sysinfo_net_filter, si, NULL)) {
                 wlr_log(WLR_INFO, "sysinfo: NM change watch active");
             } else {
                if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                wlr_log(WLR_INFO,
                    "sysinfo: NM change watch failed, polling only");
            }
        } else {
            dbus_error_free(&err);
            wlr_log(WLR_INFO, "sysinfo: NetworkManager not found on D-Bus");
        }
        if (dbus_bus_name_has_owner(si->system_bus, UPower_SERVICE, &err)) {
            pthread_mutex_lock(&si->lock);
            si->upower_available = true;
            pthread_mutex_unlock(&si->lock);
            wlr_log(WLR_INFO, "sysinfo: UPower active");
        } else {
            dbus_error_free(&err);
            wlr_log(WLR_INFO, "sysinfo: UPower not found on D-Bus");
        }
    }

    /* dedicated private connection: the shared session bus is also
     * driven by the notify thread (main loop), and libdbus is not
     * thread-safe on one connection. dbus_bus_get_private guarantees a
     * private connection safe to dbus_connection_close; the shared
     * dbus_bus_get / dbus_connection_open on the well-known bus address
     * return a cached shared connection that aborts on close. */
    si->session_bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!si->session_bus) {
        if (dbus_error_is_set(&err)) {
            wlr_log(WLR_INFO, "sysinfo: cannot open session bus: %s",
                err.message);
            dbus_error_free(&err);
        }
    } else {
        wlr_log(WLR_INFO, "sysinfo: session bus connected");
        if (dbus_bus_name_has_owner(si->session_bus, PULSE_SERVICE, &err)) {
            pthread_mutex_lock(&si->lock);
            si->pulse_available = true;
            pthread_mutex_unlock(&si->lock);
            wlr_log(WLR_INFO, "sysinfo: PulseAudio active");
        } else {
            dbus_error_free(&err);
            wlr_log(WLR_INFO,
                "sysinfo: PulseAudio not found, falling back to pactl");
        }
    }

    /* test hook: seed a fake network name + one fake iface so tests can
     * exercise the topbar net indicator and its tooltip without
     * NetworkManager (the first real poll only happens 5s in) */
    const char *test_net = getenv("GUIBUX_TEST_NET");
    if (test_net != NULL) {
        struct guibux_net_iface ni;
        memset(&ni, 0, sizeof(ni));
        snprintf(ni.label, sizeof(ni.label), "%s", test_net);
        snprintf(ni.ip, sizeof(ni.ip), "10.0.0.5");
        snprintf(ni.dns, sizeof(ni.dns), "1.1.1.1");
        snprintf(ni.gw, sizeof(ni.gw), "10.0.0.1");
        pthread_mutex_lock(&si->lock);
        snprintf(si->network, sizeof(si->network), "%s", test_net);
        memcpy(&si->net_ifaces[0], &ni, sizeof(ni));
        si->net_iface_count = 1;
        pthread_mutex_unlock(&si->lock);
    }

    /* test hook: seed a fake battery so the tooltip test can run
     * without UPower (the test environment has no UPower, so the
     * first real poll never overwrites it) */
    const char *test_tooltip = getenv("GUIBUX_TEST_TOOLTIP");
    if (test_tooltip != NULL) {
        pthread_mutex_lock(&si->lock);
        snprintf(si->battery, sizeof(si->battery), "85%%");
        si->bat_state = 2; /* discharging */
        si->bat_eta_sec = 5400; /* 1h 30m */
        pthread_mutex_unlock(&si->lock);
    }

    time_t last_net_poll = 0;
    while (true) {
        bool running = false;
        bool nm = false;
        bool upower = false;
        bool net_dirty = false;
        pthread_mutex_lock(&si->lock);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += SYSINFO_INTERVAL_SEC;
        pthread_cond_timedwait(&si->wake, &si->lock, &ts);
        running = si->worker_running;
        nm = si->nm_available;
        upower = si->upower_available;
        net_dirty = si->net_dirty;
        if (net_dirty) {
            si->net_dirty = false;
        }
        pthread_mutex_unlock(&si->lock);
        if (!running) {
            break;
        }
        /* drain NM change signals that arrived during the sleep; the
         * filter marks net_dirty. Non-blocking: the 5s tick above is
         * the cadence, the 30s fallback below covers missed signals. */
        if (si->system_bus) {
            dbus_connection_read_write_dispatch(si->system_bus, 0);
        }
        /* network: poll when NM signalled a change, or when the
         * fallback interval elapsed (self-corrects missed signals) */
        time_t now = time(NULL);
        if (nm && si->system_bus &&
            (net_dirty || now - last_net_poll >= NET_POLL_FALLBACK_SEC)) {
            sysinfo_update_network(si);
            last_net_poll = now;
        }
        /* battery works without NetworkManager (UPower poll) */
        if (upower && si->system_bus) {
            sysinfo_update_battery(si);
        }
        /* audio works without NetworkManager (pactl poll) */
        sysinfo_update_audio(si);
    }

    if (si->system_bus) {
        /* dbus_bus_get returns the shared connection: unref only,
         * closing it aborts (libdbus) */
        dbus_connection_unref(si->system_bus);
        si->system_bus = NULL;
    }
    if (si->session_bus) {
        /* private connection: close it, not just unref */
        dbus_connection_close(si->session_bus);
        dbus_connection_unref(si->session_bus);
        si->session_bus = NULL;
    }
    return NULL;
}

void
sysinfo_init(struct guibux_server *server)
{
    struct guibux_sysinfo *si = &server->sysinfo;

    si->system_bus = NULL;
    si->session_bus = NULL;
    si->pulse_available = false;
    si->nm_available = false;
    si->net_dirty = false;
    si->upower_available = false;
    si->network[0] = '\0';
    si->net_iface_count = 0;
    si->battery[0] = '\0';
    si->bat_state = 0;
    si->bat_eta_sec = 0;
    si->audio_available = false;
    si->volume = -1;
    si->muted = false;
    si->mic_volume = -1;
    si->mic_muted = false;
    si->brightness = -1;
    si->brightness_available = false;
    si->worker = 0;

    pthread_mutex_init(&si->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    /* monotonic: system clock changes must not skip or stretch the
     * polling interval */
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&si->wake, &ca);
    pthread_condattr_destroy(&ca);
    si->worker_running = true;
    if (pthread_create(&si->worker, NULL, sysinfo_worker, server) != 0) {
        wlr_log(WLR_ERROR, "sysinfo: failed to start worker thread");
        si->worker_running = false;
        si->worker = 0;
    }
}

void
sysinfo_get(struct guibux_sysinfo *si, struct guibux_sysinfo_snapshot *snap)
{
    pthread_mutex_lock(&si->lock);
    snprintf(snap->net, sizeof(snap->net), "%s", si->network);
    snap->net_iface_count = si->net_iface_count;
    for (int i = 0; i < si->net_iface_count && i < NET_IFACES_MAX; i++) {
        memcpy(&snap->net_ifaces[i], &si->net_ifaces[i],
            sizeof(struct guibux_net_iface));
    }
    snprintf(snap->bat, sizeof(snap->bat), "%s", si->battery);
    snap->bat_state = si->bat_state;
    snap->bat_eta_sec = si->bat_eta_sec;
    snap->audio_available = si->audio_available;
    snap->volume = si->volume;
    snap->muted = si->muted;
    snap->mic_volume = si->mic_volume;
    snap->mic_muted = si->mic_muted;
    snap->brightness = si->brightness;
    snap->brightness_available = si->brightness_available;
    pthread_mutex_unlock(&si->lock);
}

/*
 * Optimistic local update: the WM itself just changed the volume via
 * pactl, so apply the change to the published snapshot immediately
 * instead of waiting for the next poll. A no-op until the first
 * successful audio read; the periodic poll self-corrects any drift
 * (e.g. another app changed the volume in between).
 */
void
sysinfo_audio_adjust(struct guibux_sysinfo *si, bool mic, int delta)
{
    pthread_mutex_lock(&si->lock);
    if (si->audio_available) {
        if (mic) {
            si->mic_volume += delta;
            if (si->mic_volume < 0) {
                si->mic_volume = 0;
            }
            if (si->mic_volume > 150) {
                si->mic_volume = 150;
            }
        } else {
            si->volume += delta;
            if (si->volume < 0) {
                si->volume = 0;
            }
            if (si->volume > 150) {
                si->volume = 150;
            }
        }
    }
    pthread_mutex_unlock(&si->lock);
}

void
sysinfo_audio_toggle_mute(struct guibux_sysinfo *si, bool mic)
{
    pthread_mutex_lock(&si->lock);
    if (si->audio_available) {
        if (mic) {
            si->mic_muted = !si->mic_muted;
        } else {
            si->muted = !si->muted;
        }
    }
    pthread_mutex_unlock(&si->lock);
}

/*
 * Optimistic local update for brightness: the WM just changed it via
 * brightnessctl, so apply the delta to the published snapshot now
 * instead of waiting for the next poll. No-op until the first
 * successful read; the periodic poll self-corrects any drift.
 */
void
sysinfo_brightness_adjust(struct guibux_sysinfo *si, int delta)
{
    pthread_mutex_lock(&si->lock);
    if (si->brightness_available) {
        si->brightness += delta;
        if (si->brightness < 0) {
            si->brightness = 0;
        }
        if (si->brightness > 100) {
            si->brightness = 100;
        }
    }
    pthread_mutex_unlock(&si->lock);
}

void
sysinfo_destroy(struct guibux_server *server)
{
    struct guibux_sysinfo *si = &server->sysinfo;
    if (si->worker) {
        pthread_mutex_lock(&si->lock);
        si->worker_running = false;
        pthread_cond_signal(&si->wake);
        pthread_mutex_unlock(&si->lock);
        pthread_join(si->worker, NULL);
        si->worker = 0;
    }
    pthread_cond_destroy(&si->wake);
    pthread_mutex_destroy(&si->lock);
}
