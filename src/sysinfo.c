#define _GNU_SOURCE
#include "guibuxwm.h"
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/*
 * sysinfo - system information via D-Bus
 *
 * Queries:
 *   NetworkManager (org.freedesktop.NetworkManager)
 *     - Devices for interface name, SSID, signal
 *   UPower (org.freedesktop.UPower) - extensible for battery
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
 * Send a Properties.Get call and return the reply, or NULL on error.
 * Caller must unref the reply.
 */
static DBusMessage *
dbus_properties_get(DBusConnection *conn, const char *object_path,
                    const char *interface, const char *property,
                    DBusError *err)
{
    DBusMessage *msg = dbus_message_new_method_call(
        NM_SERVICE, object_path,
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
dbus_get_property_uint32(DBusConnection *conn, const char *object_path,
                          const char *interface, const char *property,
                          DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, object_path,
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
 * Get a D-Bus property as string.
 */
static char *
dbus_get_property_string(DBusConnection *conn, const char *object_path,
                          const char *interface, const char *property,
                          DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, object_path,
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
dbus_get_property_byte_array(DBusConnection *conn, const char *object_path,
                              const char *interface, const char *property,
                              DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, object_path,
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
 * Get an array of object paths property (e.g. Devices, ActiveConnections).
 * Returns newly allocated array of strings (NULL-terminated), or NULL.
 * Caller must free each string and the array itself.
 */
static char **
dbus_get_property_path_array(DBusConnection *conn, const char *object_path,
                              const char *interface, const char *property,
                              DBusError *err)
{
    DBusMessage *reply = dbus_properties_get(conn, object_path,
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
        int count = 0;
        int cap = 8;
        result = malloc(cap * sizeof(char *));
        if (!result) {
            dbus_message_unref(reply);
            return NULL;
        }
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
            const char *path = NULL;
            dbus_message_iter_get_basic(&arr, &path);
            /* keep room for the NUL terminator after every write */
            if (count + 1 >= cap) {
                int ncap = cap * 2;
                char **tmp = realloc(result, ncap * sizeof(char *));
                if (!tmp) {
                    free_path_array(result);
                    result = NULL;
                    break;
                }
                result = tmp;
                cap = ncap;
            }
            result[count++] = strdup(path);
            dbus_message_iter_next(&arr);
        }
        if (result && count > 0) {
            result[count] = NULL;
        }
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
        si->system_bus, NM_PATH, NM_IFACE, "Devices", &err);
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
            si->system_bus, dev_path,
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
            si->system_bus, dev_path,
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
        char *iface = dbus_get_property_string(si->system_bus, dev_path,
            NM_DEVICE_IFACE, "Interface", &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
            iface = NULL;
        }

        if (dev_type == 2) {
            /* WiFi: get SSID and signal */
            char *ssid = dbus_get_property_byte_array(si->system_bus, dev_path,
                NM_WIFI_IFACE, "Ssid", &err);
            if (dbus_error_is_set(&err)) {
                dbus_error_free(&err);
                ssid = NULL;
            }

            dbus_uint32_t strength = dbus_get_property_uint32(
                si->system_bus, dev_path,
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
 * Extensible: battery via UPower (future).
 */
static void
sysinfo_update_battery(struct guibux_sysinfo *si)
{
    /* TODO: Query UPower for battery percentage */
    pthread_mutex_lock(&si->lock);
    si->battery[0] = '\0';
    pthread_mutex_unlock(&si->lock);
}

static void
sysinfo_update(struct guibux_sysinfo *si)
{
    sysinfo_update_network(si);
    sysinfo_update_battery(si);
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
    }

    while (true) {
        bool running = false;
        bool nm = false;
        pthread_mutex_lock(&si->lock);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += SYSINFO_INTERVAL_SEC;
        pthread_cond_timedwait(&si->wake, &si->lock, &ts);
        running = si->worker_running;
        nm = si->nm_available;
        pthread_mutex_unlock(&si->lock);
        if (!running) {
            break;
        }
        if (nm && si->system_bus) {
            sysinfo_update(si);
        }
    }

    if (si->system_bus) {
        dbus_connection_close(si->system_bus);
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
    si->network[0] = '\0';
    si->battery[0] = '\0';
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
sysinfo_get(struct guibux_sysinfo *si, char *net, size_t net_size,
            char *bat, size_t bat_size)
{
    pthread_mutex_lock(&si->lock);
    snprintf(net, net_size, "%s", si->network);
    snprintf(bat, bat_size, "%s", si->battery);
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
