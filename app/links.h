/* 1.9.0 Links to other devices: round trip, loss and match times, read from the Port Testing app (package
 * port_testing) on this camera while it is installed and running, about every 5 s, on a thread of its own.
 * Nothing is read, and nothing new shows, while Port Testing is missing or stopped, or while Settings ›
 * What to collect has the Links row neither shown nor sent (and no alert watches a link).
 *
 * Everything has a fixed most: LINK_MAX devices (a device not seen for 24 h gives its place to a new one), 24 h of
 * 30 s history points (set aside once, the first time Port Testing answers), LINK_MATCHES matches. */
#ifndef LINKS_H
#define LINKS_H
#include <glib.h>
#include "cJSON.h"

#define LINK_MAX     16
#define LINK_MATCHES 200
enum { LA_MISSING = 0, LA_STOPPED = 1, LA_RUNNING = 2 };   /* Port Testing: not installed, installed but not running, running */

void   links_init(void);                     /* starts the reading thread */
void   links_stop(void);                     /* stops it (on stop) */
void   links_set_app(int state);             /* main.c, from the camera's app list every 5 s (LA_*) */
int    links_app(void);
/* The latest round trip (ms) and loss (%) per device place, NAN where none; for a live reading. Returns the
 * number of places in use (0 when there is no fresh reading). */
int    links_now(float* rtt, float* loss);
/* History: the peaks per device place over the points in (t0, t1] (unix s), NAN where none. Returns 1 when any. */
int    links_hist(gint64 t0, gint64 t1, float* rtt, float* loss);
/* For the page: {app, ok, devs:[{name, address, state, rtt, p95, loss, refused, playing, game, current} or null per
 * place], matches:[{start, end (null while playing), game, camera}]} */
cJSON* links_json(void);
/* Alerts. which: 0 round trip, 1 loss. device: a device's name, or "" for any device (the highest when above, the
 * lowest when below). Returns 0 when Port Testing is not running (or not answering), else 1 with *v (NAN when that
 * device has no value now) and the device it is from. */
int    links_alert(int which, const char* device, int above, double* v, char* dev, size_t n);
cJSON* links_device_names(void);             /* the devices in the latest reading, for the alert's Device choice */
/* The latest values into a PromSnap (prom.h) for the Prometheus page, MQTT and InfluxDB */
void   links_snap(void* promSnap);
#endif
