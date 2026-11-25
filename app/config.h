/* Settings: kept in localdata/config.json (schema 1), checked before they are saved or used */
#ifndef CONFIG_H
#define CONFIG_H
#include <glib.h>
#include "cJSON.h"

/* 1.3.3: the ways out. Passwords and tokens are written only: never returned by config_json and never logged. */
#define CA_MAX 6144
typedef struct {
	int on, port, tls, checkName, ha, interval;          /* interval: seconds, 10..300 */
	char host[128], user[64], pass[128], clientId[64], prefix[128], ca[CA_MAX];   /* clientId, prefix: "" = the default */
} MqttCfg;
typedef struct {
	int on, version, checkName, interval;                /* version: 1 or 2 */
	char url[256], db[64], user[64], pass[128], org[64], bucket[64], token[256], ca[CA_MAX];
} InfluxCfg;

void   config_load(void);                    /* at start: read, check, hand to the sampler (a bad file gives the defaults) */
cJSON* config_json(void);                    /* {settings, status} for the Settings page (no passwords or tokens) */
/* Check and save new settings (the whole object). Returns NULL when saved, else why not (to free).
 * changes: what changed, in plain words, for the app log (to free; NULL when nothing changed). */
gchar* config_save(const cJSON* in, gchar** changes);
int    config_metrics(unsigned* send);          /* 1.3.2: 1 when the Prometheus page is on; *send: its groups */
/* 1.3.3: the MQTT and InfluxDB settings and the groups they send; returns a number that changes with every save */
int    config_out(MqttCfg* m, InfluxCfg* i, unsigned* send);
/* 1.7.0 Copy settings to other cameras: the settings in two parts, as a new object with passwords and token
 * (the caller deletes it). CFG_PART_MAIN: history, power, PoE ports, what to collect, Prometheus, temperature unit.
 * CFG_PART_OUT: the mqtt and influx blocks, without their CA certificates (they stay on each camera). */
enum { CFG_PART_MAIN = 0, CFG_PART_OUT = 1 };
cJSON* config_part(int part);
/* Checks a part as config_save would (keys not given keep the camera's value) and rewrites it as it would be saved.
 * NULL when it may be saved, else why not (to free). Save it with config_save. */
gchar* config_part_check(cJSON* value, int part);
#endif
