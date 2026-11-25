/* 1.4.0 Alerts: a reading over (or under) a limit for a while sends a stateful camera event a VMS can use (1.5.0: and,
 * as each alert chooses, MQTT, InfluxDB and the Prometheus page),
 * like AXIS Object Analytics (active is the only state). Optional "needs approval": the event waits until a person
 * approves or rejects it. Kept in localdata/alerts.json; activity in localdata/alerts-log.json. */
#ifndef ALERTS_H
#define ALERTS_H
#include <glib.h>
#include "cJSON.h"

typedef void (*AlertsNeed)(unsigned groups);          /* the reading groups the enabled alerts need (sampler.h G_*) */
void   alerts_init(AlertsNeed need);                 /* main loop, after ACAP_Init */
void   alerts_tick(void);                            /* main loop, every second, after the reading */
void   alerts_stop(void);                            /* main loop: Off for every triggered alert, write the log */
cJSON* alerts_json(void);                            /* {metrics, alerts (with status), log} for the page */
/* Save the whole list (admin). NULL when saved, else why not (to free). */
gchar* alerts_save(const cJSON* in, const char* who);
/* approve, reject or test one alert. NULL when done, else why not (to free). */
gchar* alerts_act(const char* id, const char* action, const char* who);
/* 1.5.0: gauges resource_monitor_alert_active for the alerts that chose Prometheus (to free; NULL when none) */
char*  alerts_prom(void);
/* 1.6.3: what an alert's Context would send now (the Context editor's preview) */
cJSON* alerts_preview(const cJSON* in);
/* 1.7.0 Copy settings to other cameras (cfgsections.c) */
cJSON* alerts_saved(void);                           /* the list as saved, ids included, no status (to delete) */
int    alerts_metric_known(const char* key);         /* 1 when key is a metric an alert can watch */
#define ALERTS_MAX 20
/* Checks a list as alerts_save would and rewrites it as it would be saved. Items without an id get one.
 * NULL when it may be saved, else why not (to free). Save it with alerts_save. */
gchar* alerts_normalize(cJSON* list);
#endif
