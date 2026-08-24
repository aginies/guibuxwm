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
sysinfo_set_network(struct guibux_sysinfo *si, const char *s)
{
    pthread_mutex_lock(&si->lock);
    snprintf(si->network, sizeof(si->network), "%s", s);
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
        sysinfo_set_network(si, "NM");
        return;
    }

    DBusError err = DBUS_ERROR_INIT;

    /* Get all devices directly from NM */
    char **all_devices = dbus_get_property_path_array(
        si->system_bus, NM_SERVICE, NM_PATH, NM_IFACE, "Devices", &err);
    if (dbus_error_is_set(&err)) {
        wlr_log(WLR_INFO, "sysinfo: Devices failed: %s", err.message);
        dbus_error_free(&err);
        sysinfo_set_network(si, "NM");
        return;
    }

    if (!all_devices || !all_devices[0]) {
        free_path_array(all_devices);
        sysinfo_set_network(si, "No net");
        return;
    }

    /* Collect display strings for each connected non-loopback device */
    char display[128] = {0};
    int display_len = 0;
    bool any_connected = false;

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
                display_len = display_append(display, sizeof(display),
                    display_len, "%s%s %u%%",
                    display_len ? " " : "", ssid, strength);
            } else if (iface) {
                display_len = display_append(display, sizeof(display),
                    display_len, "%s%s",
                    display_len ? " " : "", iface);
            }

            free(ssid);
        } else if (iface) {
            display_len = display_append(display, sizeof(display),
                display_len, "%s%s",
                display_len ? " " : "", iface);
        }

        free(iface);
    }

    free_path_array(all_devices);

    if (!any_connected) {
        sysinfo_set_network(si, "No net");
    } else {
        sysinfo_set_network(si, display);
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
            dbus_uint32_t tte = dbus_get_property_uint32(
                si->system_bus, UPower_SERVICE, dev_path,
                UPower_DEVICE_IFACE, "TimeToEmpty", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
            }
            dbus_uint32_t ttf = dbus_get_property_uint32(
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

    /* test hook: seed a fake network name so tests can exercise the
     * topbar net indicator without NetworkManager (the first real poll
     * only happens 5s in) */
    const char *test_net = getenv("GUIBUX_TEST_NET");
    if (test_net != NULL) {
        pthread_mutex_lock(&si->lock);
        snprintf(si->network, sizeof(si->network), "%s", test_net);
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
    si->battery[0] = '\0';
    si->bat_state = 0;
    si->bat_eta_sec = 0;
    si->audio_available = false;
    si->volume = -1;
    si->muted = false;
    si->mic_volume = -1;
    si->mic_muted = false;
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
    snprintf(snap->bat, sizeof(snap->bat), "%s", si->battery);
    snap->bat_state = si->bat_state;
    snap->bat_eta_sec = si->bat_eta_sec;
    snap->audio_available = si->audio_available;
    snap->volume = si->volume;
    snap->muted = si->muted;
    snap->mic_volume = si->mic_volume;
    snap->mic_muted = si->mic_muted;
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
