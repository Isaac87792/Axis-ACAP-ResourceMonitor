/* 1.7.0 Copy settings to other cameras: Resource Monitor's sections for kit_configio.c (the configio endpoint).
 *
 *   settings  "Settings"                  localdata/config.json without mqtt and influx
 *   alerts    "Alerts"                    localdata/alerts.json; ids and status stay on the camera,
 *                                         and alert counters and logs are not settings
 *   sendout   "Send to MQTT / InfluxDB"   the mqtt and influx blocks; the passwords and the token never leave
 *                                         the camera, and CA certificates stay on each camera
 *
 * Each check is the app's own save check (config.c, alerts.c) and each apply its own save path, so an import
 * is held to exactly what the Settings and Alerts pages are held to. Nothing here writes a file itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "cJSON.h"
#include "kit.h"
#include "config.h"
#include "alerts.h"
#include "cfgsections.h"

static const char* const MAIN_KEYS[] = { "historyDays", "powerSource", "poePorts", "collect", "prometheus", "tempUnit", NULL };
static const char* const OUT_KEYS[] = { "mqtt", "influx", NULL };
static const char* const MQTT_KEYS[] = { "on", "host", "port", "tls", "checkName", "user", "password", "clientId", "prefix", "haDiscovery", "interval", NULL };
static const char* const INFLUX_KEYS[] = { "on", "url", "version", "database", "user", "password", "org", "bucket", "token", "checkName", "interval", NULL };
static const char* const ALERT_KEYS[] = { "id", "name", "on", "metric", "op", "value", "for", "approval", "to", "context", "status", NULL };

static gboolean in_set(const char* const* set, const char* key) {
	for (int k = 0; key && set[k]; k++) if (strcmp(set[k], key) == 0) return TRUE;
	return FALSE;
}

/* Short and printable, for a name or key from a file in a note */
static gchar* shown(const char* s) {
	gchar* c = g_strndup(s ? s : "", 40);
	for (char* p = c; *p; p++) if ((unsigned char)*p < 32 || *p == '"') *p = '?';
	return c;
}

/* Takes out of obj every key not in keys; one note names them. quiet: keys taken out without a note. */
static void keep_only(cJSON* obj, const char* const* keys, const char* const* quiet, const char* where, cJSON* notes) {
	GString* gone = g_string_new("");
	cJSON* f = obj ? obj->child : NULL;
	while (f) {
		cJSON* next = f->next;
		if (!f->string || !in_set(keys, f->string)) {
			if (f->string && !(quiet && in_set(quiet, f->string))) {
				gchar* k = shown(f->string);
				g_string_append_printf(gone, "%s%s", gone->len ? ", " : "", k);
				g_free(k);
			}
			cJSON_Delete(cJSON_DetachItemViaPointer(obj, f));
		}
		f = next;
	}
	if (gone->len) kit_configio_note(notes, "%s: not used by this version, left out: %s.", where, gone->str);
	g_string_free(gone, TRUE);
}

/*----------------------------------------------------- settings */

static cJSON* Settings_Get(void) { return config_part(CFG_PART_MAIN); }

static gchar* Settings_Check(cJSON* v, cJSON* notes) {
	static const char* const quiet[] = { "schema", NULL };
	keep_only(v, MAIN_KEYS, quiet, "Settings", notes);
	return config_part_check(v, CFG_PART_MAIN);
}

static int Settings_Apply(cJSON* v) {
	gchar* changes = NULL;
	gchar* why = config_save(v, &changes);              /* checks again, writes config.json, starts using it */
	g_free(changes);                                    /* the import's own log line says what came in */
	int ok = why == NULL;
	g_free(why);
	return ok;
}

/*----------------------------------------------------- send to MQTT / InfluxDB */

static cJSON* Sendout_Get(void) { return config_part(CFG_PART_OUT); }

static gchar* Sendout_Check(cJSON* v, cJSON* notes) {
	keep_only(v, OUT_KEYS, NULL, "Send to MQTT / InfluxDB", notes);
	static const char* const quiet[] = { "passwordSet", "tokenSet", "ca", NULL };   /* the page's own fields; CA below */
	for (int w = 0; w < 2; w++) {
		cJSON* o = cJSON_GetObjectItemCaseSensitive(v, w ? "influx" : "mqtt");
		if (!cJSON_IsObject(o)) continue;
		const char* ca = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "ca"));
		if (ca && *ca) kit_configio_note(notes, "%s: the file's CA certificate was not used; this camera keeps its own.", w ? "InfluxDB" : "MQTT");
		keep_only(o, w ? INFLUX_KEYS : MQTT_KEYS, quiet, w ? "InfluxDB" : "MQTT", notes);
	}
	return config_part_check(v, CFG_PART_OUT);         /* keys not given (the CA among them) keep the camera's value */
}

static int Sendout_Apply(cJSON* v) { return Settings_Apply(v); }

/*----------------------------------------------------- alerts */

static cJSON* Alerts_Get(void) { return alerts_saved(); }

static int id_number(const char* id) {
	return id && g_str_has_prefix(id, "alert") ? atoi(id + 5) : 0;
}

static gchar* Alerts_Check(cJSON* v, cJSON* notes) {
	int n = cJSON_GetArraySize(v);
	if (n > ALERTS_MAX) return g_strdup_printf("this would make %d alerts, and a camera takes at most %d. Take some out of the file, or with Merge, remove some from this camera first.", n, ALERTS_MAX);
	gchar* why = NULL;
	cJSON* it;
	cJSON_ArrayForEach(it, v) {
		/* Names are not compared here: two alerts may share a name on the camera (alerts.c allows it, so an undo
		 * must be able to put such a list back); the kit takes such a file in Replace and refuses it in Merge.
		 * A name is shown trimmed, as alerts.c saves it. */
		const char* name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "name"));
		gchar* trimmed = g_strstrip(g_strdup(name ? name : ""));
		gchar* nm = shown(*trimmed ? trimmed : "(no name)");
		g_free(trimmed);
		const char* metric = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "metric"));
		if (!alerts_metric_known(metric)) {
			gchar* m = shown(metric ? metric : "nothing");
			why = g_strdup_printf("alert '%s' watches %s, which this version of Resource Monitor does not know. Update the app, or take the alert out of the file.", nm, m);
			g_free(m); g_free(nm);
			break;
		}
		gchar* where = g_strdup_printf("Alert '%s'", nm);
		keep_only(it, ALERT_KEYS, NULL, where, notes);
		g_free(where);
		g_free(nm);
	}
	if (why) return why;
	/* New alerts get numbers above any in use on the camera, so a removed alert's events and counter stay its own */
	int next = 1;
	cJSON* now = alerts_saved();
	cJSON_ArrayForEach(it, now) { int k = id_number(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "id"))); if (k >= next) next = k + 1; }
	cJSON_Delete(now);
	cJSON_ArrayForEach(it, v) { int k = id_number(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "id"))); if (k >= next) next = k + 1; }
	cJSON_ArrayForEach(it, v) {
		const char* id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "id"));
		if (id && *id) continue;
		gchar* nid = g_strdup_printf("alert%d", next++);
		cJSON_DeleteItemFromObjectCaseSensitive(it, "id");
		cJSON_AddStringToObject(it, "id", nid);
		g_free(nid);
	}
	return alerts_normalize(v);                          /* the same check as alerts_save */
}

static int Alerts_Apply(cJSON* v) {
	cJSON* in = cJSON_CreateObject();
	cJSON_AddItemToObject(in, "alerts", cJSON_Duplicate(v, 1));
	gchar* why = alerts_save(in, "Settings import");    /* takes alerts.c's lock itself, writes alerts.json */
	cJSON_Delete(in);
	int ok = why == NULL;
	g_free(why);
	return ok;
}

void cfgsections_init(void) {
	static const char* const outSecrets[] = { "mqtt.password", "influx.password", "influx.token", NULL };
	static const char* const alertStrip[] = { "id", "status", NULL };
	static const KitConfigSection settings = { "settings", "Settings", 0, NULL, NULL, Settings_Get, Settings_Check, Settings_Apply };
	static const KitConfigSection alerts = { "alerts", "Alerts", 1, NULL, alertStrip, Alerts_Get, Alerts_Check, Alerts_Apply };
	static const KitConfigSection sendout = { "sendout", "Send to MQTT / InfluxDB", 0, outSecrets, NULL, Sendout_Get, Sendout_Check, Sendout_Apply };
	kit_configio_section(&settings);
	kit_configio_section(&alerts);
	kit_configio_section(&sendout);
}
