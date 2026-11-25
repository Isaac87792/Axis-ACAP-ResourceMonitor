/* 1.3.3: sending out over MQTT and to InfluxDB, on its own thread (never holds up the 1 s reading) */
#ifndef SENDOUT_H
#define SENDOUT_H
#include <glib.h>
#include "cJSON.h"

/* log: one line in the app log (what changed, in plain words). Device facts for the payloads and defaults. */
void   sendout_init(void (*log)(const char* what), const char* serial, const char* model, const char* os, const char* appVer);
cJSON* sendout_status_json(void);             /* {mqtt:{state, text}, influx:{state, text, waiting}} for Settings */
gchar* sendout_test(const char* what);        /* "mqtt" or "influx": try the saved settings now; plain words (to free) */
void   sendout_stop(void);                    /* say "offline" and close, on stop */
/* 1.5.0 Alerts out (main loop). MQTT: the alert's state, published retained to <prefix>/alert/<id> on every change and
 * on each connect (and a Home Assistant binary_sensor with discovery on). keep: the ids that go to MQTT now; any other
 * is taken back (empty retained state and discovery). InfluxDB: one point resource_monitor_alert per change, through
 * the same queue as the metrics, sent within a second. Nothing is sent while that way out is off in Settings. */
void   sendout_alert_mqtt(const char* id, const char* name, int active, const char* context);
void   sendout_alert_keep(const char* const* ids, int n);
void   sendout_alert_influx(const char* id, const char* name, int active, const char* context);
#endif
