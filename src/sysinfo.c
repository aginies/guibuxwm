#define _GNU_SOURCE
#include "guibuxwm.h"
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
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

/*
 * Get a D-Bus property as uint32.
 */
static dbus_uint32_t
dbus_get_property_uint32(DBusConnection *conn, const char *service,
                          const char *object_path,
                          const char *interface, const char *property,
                          DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return 0;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    dbus_uint32_t val = 0;
    dbus_uint32_t type = dbus_message_iter_get_arg_type(&variant);
    if (type == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&variant, &val);
    } else if (type == DBUS_TYPE_BYTE) {
        unsigned char byte_val = 0;
        dbus_message_iter_get_basic(&variant, &byte_val);
        val = byte_val;
    } else if (type == DBUS_TYPE_UINT16) {
        dbus_uint16_t uint16_val = 0;
        dbus_message_iter_get_basic(&variant, &uint16_val);
        val = uint16_val;
    }

    dbus_message_unref(reply);
    return val;
}

/*
 * Get a D-Bus property as int64 (e.g. UPower TimeToEmpty/TimeToFull,
 * which newer UPower exposes as int64 instead of uint32).
 * Returns 0 on failure or type mismatch.
 */
static int64_t
dbus_get_property_int64(DBusConnection *conn, const char *service,
                         const char *object_path,
                         const char *interface, const char *property,
                         DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return 0;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    int64_t val = 0;
    dbus_uint32_t type = dbus_message_iter_get_arg_type(&variant);
    if (type == DBUS_TYPE_INT64) {
        dbus_int64_t v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    } else if (type == DBUS_TYPE_INT32) {
        dbus_int32_t v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    } else if (type == DBUS_TYPE_INT16) {
        dbus_int16_t v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    } else if (type == DBUS_TYPE_UINT32) {
        dbus_uint32_t v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    } else if (type == DBUS_TYPE_UINT16) {
        dbus_uint16_t v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    } else if (type == DBUS_TYPE_BYTE) {
        unsigned char v;
        dbus_message_iter_get_basic(&variant, &v);
        val = v;
    }

    dbus_message_unref(reply);
    return val;
}

/*
 * Get a D-Bus property as double (e.g. UPower Percentage).
 * Returns -1 on failure or type mismatch.
 */
static double
dbus_get_property_double(DBusConnection *conn, const char *service,
                          const char *object_path,
                          const char *interface, const char *property,
                          DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return -1.0;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    double val = -1.0;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_DOUBLE) {
        dbus_message_iter_get_basic(&variant, &val);
    }

    dbus_message_unref(reply);
    return val;
}

/*
 * Get a D-Bus property as string.
 */
static char *
dbus_get_property_string(DBusConnection *conn, const char *service,
                          const char *object_path,
                          const char *interface, const char *property,
                          DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return NULL;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    char *result = NULL;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&variant, &s);
        result = strdup(s);
    }

    dbus_message_unref(reply);
    return result;
}

/*
 * Get a D-Bus byte array property (e.g. SSID).
 * Returns newly allocated NUL-terminated string, or NULL.
 */
static char *
dbus_get_property_byte_array(DBusConnection *conn, const char *service,
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

    char *result = NULL;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&variant, &arr);
        char buf[256] = {0};
        int len = 0;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE &&
               len < 255) {
            unsigned char byte_val = 0;
            dbus_message_iter_get_basic(&arr, &byte_val);
            buf[len++] = byte_val;
            dbus_message_iter_next(&arr);
        }
        buf[len] = '\0';
        if (len > 0) {
            result = strdup(buf);
        }
    }

    dbus_message_unref(reply);
    return result;
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
 * Get a D-Bus property as object path (e.g. NM IP4Config).
 * Returns newly allocated string, or NULL.
 */
static char *
dbus_get_property_object_path(DBusConnection *conn, const char *service,
                               const char *object_path,
                               const char *interface, const char *property,
                               DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return NULL;
    }

    DBusMessageIter iter, variant;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    char *result = NULL;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_OBJECT_PATH) {
        const char *p = NULL;
        dbus_message_iter_get_basic(&variant, &p);
        if (p && p[0] != '\0') {
            result = strdup(p);
        }
    }

    dbus_message_unref(reply);
    return result;
}

/*
 * Get the first address from a D-Bus AddressData property (NM
 * IP4Config/IP6Config AddressData: aa{sv}, array of dicts with
 * "address" (s) and "prefix" (u)). Fills addr (4 or 16 bytes) and
 * sets *af to AF_INET or AF_INET6. Returns false on failure or empty
 * array.
 */
static bool
dbus_get_property_first_address(DBusConnection *conn, const char *service,
                                  const char *object_path,
                                  const char *interface, const char *property,
                                  void *addr, int *af, DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return false;
    }

    DBusMessageIter iter, variant, arr, dict, val;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    bool ok = false;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
        /* AddressData is aa{sv}: the variant holds an array whose first
         * element is the inner array of dict entries */
        dbus_message_iter_recurse(&variant, &arr);
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&arr, &arr);
        }
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&arr, &dict);
            while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
                const char *key = NULL;
                dbus_message_iter_get_basic(&dict, &key);
                dbus_message_iter_next(&dict);
                /* dict values are wrapped in a variant */
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

    dbus_message_unref(reply);
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
 * Get the default gateway from a D-Bus Gateway property (NM
 * IP4Config/IP6Config Gateway: a plain string, possibly empty).
 * Returns false on failure or empty string.
 */
static bool
dbus_get_property_gateway(DBusConnection *conn, const char *service,
                           const char *object_path,
                           const char *interface,
                           unsigned char *gw, int *af, DBusError *err)
{
    char *s = dbus_get_property_string(conn, service, object_path,
        interface, "Gateway", err);
    if (!s) {
        return false;
    }
    bool ok = false;
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, s, &a4) == 1) {
        memcpy(gw, &a4, 4);
        *af = AF_INET;
        ok = true;
    } else if (inet_pton(AF_INET6, s, &a6) == 1) {
        memcpy(gw, &a6, 16);
        *af = AF_INET6;
        ok = true;
    }
    free(s);
    return ok;
}

/*
 * Get the DNS servers from a D-Bus NameserverData property (NM
 * IP4Config/IP6Config NameserverData: aa{sv}, array of dicts with
 * "address" (s)). Fills out (NUL-terminated, comma-separated).
 * Returns false on failure or no DNS.
 */
static bool
dbus_get_property_dns(DBusConnection *conn, const char *service,
                        const char *object_path,
                        const char *interface, const char *property,
                        char *out, size_t out_size, DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, service, object_path,
        interface, property, err);
    if (!reply) {
        return false;
    }

    DBusMessageIter iter, variant, arr, dict, val;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_recurse(&iter, &variant);

    bool ok = false;
    out[0] = '\0';
    int len = 0;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&variant, &arr);
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&arr, &arr);
        }
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&arr, &dict);
            while (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_INVALID) {
                const char *key = NULL;
                dbus_message_iter_get_basic(&dict, &key);
                dbus_message_iter_next(&dict);
                /* dict values are wrapped in a variant */
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

    dbus_message_unref(reply);
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

        /* Get device type */
        dbus_uint32_t dev_type = dbus_get_property_uint32(
            si->system_bus, NM_SERVICE, dev_path,
            NM_DEVICE_IFACE, "DeviceType", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
            continue;
        }

        /* Skip loopback (type 32) */
        if (dev_type == 32) {
            continue;
        }

        /* Get device state - 110 = activated */
        dbus_uint32_t dev_state = dbus_get_property_uint32(
            si->system_bus, NM_SERVICE, dev_path,
            NM_DEVICE_IFACE, "State", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
            continue;
        }

        /* Only show devices that are not disconnected (state >= 50) */
        if (dev_state < 50) {
            continue;
        }

        any_connected = true;

        /* Get interface name */
        char *iface = dbus_get_property_string(si->system_bus, NM_SERVICE, dev_path,
            NM_DEVICE_IFACE, "Interface", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
            iface = NULL;
        }

        char label[64] = {0};
        if (dev_type == 2) {
            /* WiFi: get SSID and signal */
            char *ssid = dbus_get_property_byte_array(si->system_bus, NM_SERVICE, dev_path,
                NM_WIFI_IFACE, "Ssid", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                ssid = NULL;
            }

            dbus_uint32_t strength = dbus_get_property_uint32(
                si->system_bus, NM_SERVICE, dev_path,
                NM_WIFI_IFACE, "Strength", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                strength = 0;
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

            char *ipcfg = dbus_get_property_object_path(si->system_bus,
                NM_SERVICE, dev_path, NM_DEVICE_IFACE, "Ip4Config", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                ipcfg = NULL;
            }
            if (ipcfg == NULL) {
                ipcfg = dbus_get_property_object_path(si->system_bus,
                    NM_SERVICE, dev_path, NM_DEVICE_IFACE, "Ip6Config", &err);
                if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                    ipcfg = NULL;
                }
            }
            /* the config object exposes its data on the IP4Config/IP6Config
             * interface, not the base NetworkManager one */
            const char *cfg_iface = strstr(ipcfg, "IP6Config") ?
                "org.freedesktop.NetworkManager.IP6Config" :
                "org.freedesktop.NetworkManager.IP4Config";
            if (ipcfg != NULL) {
                unsigned char addr[16];
                int af = 0;
                if (dbus_get_property_first_address(si->system_bus,
                        NM_SERVICE, ipcfg, cfg_iface, "AddressData",
                        addr, &af, &err)) {
                    if (dbus_error_is_set(&err)) {
                        dbus_error_free(&err);
                    }
                    inet_ntop(af, addr, ni->ip, sizeof(ni->ip));
                } else if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                unsigned char gw_bytes[16];
                int gw_af = 0;
                if (dbus_get_property_gateway(si->system_bus,
                        NM_SERVICE, ipcfg, cfg_iface,
                        gw_bytes, &gw_af, &err)) {
                    if (dbus_error_is_set(&err)) {
                        dbus_error_free(&err);
                    }
                    inet_ntop(gw_af, gw_bytes, ni->gw, sizeof(ni->gw));
                } else if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                /* NM's Gateway is empty for tunnels / point-to-point links;
                 * fall back to the kernel route table */
                if (ni->gw[0] == '\0' && iface != NULL) {
                    route_gateway(iface, gw_bytes, &gw_af);
                    if (gw_af != 0) {
                        inet_ntop(gw_af, gw_bytes, ni->gw, sizeof(ni->gw));
                    }
                }
                dbus_get_property_dns(si->system_bus, NM_SERVICE, ipcfg,
                    cfg_iface, "NameserverData", ni->dns, sizeof(ni->dns), &err);
                if (dbus_error_is_set(&err)) {
                    dbus_error_free(&err);
                }
                free(ipcfg);
            }
            iface_count++;
        }

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

            dbus_uint32_t type = dbus_get_property_uint32(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "Type", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                continue;
            }
            if (type != UP_DEVICE_TYPE_BATTERY) {
                continue;
            }

            double d = dbus_get_property_double(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "Percentage", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                continue;
            }
            if (d < 0.0 || d > 100.0) {
                continue;
            }
            pct = (int)(d + 0.5);

            state = (int)dbus_get_property_uint32(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "State", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            int64_t tte = dbus_get_property_int64(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "TimeToEmpty", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            int64_t ttf = dbus_get_property_int64(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "TimeToFull", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            /* TimeToEmpty is only meaningful while discharging,
             * TimeToFull while charging; 0 = no estimate */
            if (state == 1) {
                eta = (int)ttf;
            } else if (state == 2) {
                eta = (int)tte;
            }
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
 * Update sink/source (mic) volume + mute via pactl. Works with
 * PulseAudio and PipeWire (pulse compat). A failed read leaves the
 * previous values untouched; audio_available stays false until the
 * first successful read (no audio on the system -> indicator hidden).
 */
static void
sysinfo_update_audio(struct guibux_sysinfo *si)
{
    int volume = -1, mic_volume = -1;
    bool muted = false, mic_muted = false;

    char *out = run_capture("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null");
    if (out) {
        volume = parse_percent(out);
        free(out);
    }
    if (volume >= 0) {
        out = run_capture("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
        if (out) {
            muted = (strstr(out, "Mute: yes") != NULL);
            free(out);
        }
    }

    out = run_capture("pactl get-source-volume @DEFAULT_SOURCE@ 2>/dev/null");
    if (out) {
        mic_volume = parse_percent(out);
        free(out);
    }
    if (mic_volume >= 0) {
        out = run_capture("pactl get-source-mute @DEFAULT_SOURCE@ 2>/dev/null");
        if (out) {
            mic_muted = (strstr(out, "Mute: yes") != NULL);
            free(out);
        }
    }

    int brightness = -1;
    out = run_capture("brightnessctl get 2>/dev/null");
    if (out) {
        brightness = parse_percent(out);
        free(out);
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
            pthread_mutex_unlock(&si->lock);
            wlr_log(WLR_INFO, "sysinfo: NetworkManager active");
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

    while (true) {
        bool running = false;
        bool nm = false;
        bool upower = false;
        pthread_mutex_lock(&si->lock);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += SYSINFO_INTERVAL_SEC;
        pthread_cond_timedwait(&si->wake, &si->lock, &ts);
        running = si->worker_running;
        nm = si->nm_available;
        upower = si->upower_available;
        pthread_mutex_unlock(&si->lock);
        if (!running) {
            break;
        }
        if (nm && si->system_bus) {
            sysinfo_update_network(si);
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
    return NULL;
}

void
sysinfo_init(struct guibux_server *server)
{
    struct guibux_sysinfo *si = &server->sysinfo;

    si->system_bus = NULL;
    si->nm_available = false;
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
