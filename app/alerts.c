/* 1.4.0 Alerts. See alerts.h.
 *
 * localdata/alerts.json, schema 1:
 *   {"schema":1, "alerts":[{"id":"alert1", "name":"CPU high", "on":true, "metric":"cpu", "op":"above",
 *                           "value":85, "for":30, "approval":false,
 *                           "to":{"event":true,"mqtt":false,"influx":false,"prom":false}}, ...]}
 *   1.5.0 "to": where the alert goes (Send to). A file without it means the camera event only.
 *
 * Each enabled alert declares one stateful event (tnsaxis:CameraApplicationPlatform/resource_monitor/<id>, shown
 * under "Resource Monitor" with the alert's name), active as the only state and a Context text as data, the way
 * AXIS Object Analytics does it, so video management systems take it.
 *
 * 1.5.0: each alert also goes, as it chooses, to MQTT (retained state, sendout.c), InfluxDB (one point per change,
 * sendout.c) and the Prometheus page (a gauge, alerts_prom). "Needs approval" holds back every way out alike.
 *
 * Threads: the page's endpoints (web thread) only read and change the lists behind `mu`, and leave requests
 * (declare again, approve, reject, test) for alerts_tick, which runs on the main loop and is the only place that
 * talks to the event system. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <syslog.h>
#include <axsdk/axevent.h>
#include "ACAP.h"
#include "alerts.h"
#include "sampler.h"
#include "prom.h"
#include "config.h"
#include "sendout.h"

#define APP_PACKAGE  "resource_monitor"
#define APP_NAME     "Resource Monitor"
#define ALERTS_FILE  "localdata/alerts.json"
#define LOG_FILE     "localdata/alerts-log.json"
#define MAX_ALERTS   20
#define LOG_MAX      200
#define CLEAR_MIN    5          /* a triggered alert clears only after this many seconds back to normal, at least */
#define TEST_MS      3000       /* Test: On, then Off this much later */
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

typedef struct { const char* key; const char* name; const char* unit; unsigned group; } Metric;
static const Metric MET[] = {
	{ "cpu",     "CPU busy",          "%",      G_CPU },
	{ "apps",    "Non-Axis apps CPU", "%",      G_CPU },
	{ "load",    "Load (1 min)",      "",       G_CPU },
	{ "mem",     "Memory used",       "%",      G_MEM },
	{ "storage", "Fullest drive",     "% full", G_STORAGE },
	{ "temp",    "Hottest sensor",    "°C",     G_TEMP },
	{ "power",   "Power draw",        "W",      G_POWER },
	{ "netin",   "Network in",        "Mbit/s", G_NET },
	{ "netout",  "Network out",       "Mbit/s", G_NET },
};
#define METRICS (int)(sizeof(MET) / sizeof(MET[0]))

enum { TO_EVENT = 1, TO_MQTT = 2, TO_INFLUX = 4, TO_PROM = 8 };      /* 1.5.0: Send to */
static const char* TO_WORD[] = { "Camera event", "MQTT", "InfluxDB", "Prometheus" };
enum { ST_OFF, ST_LISTEN, ST_TRIG, ST_WAIT, ST_REJECT, ST_PROBLEM };
static const char* ST_WORD[] = { "Disabled", "Listening", "Triggered", "Waiting", "Rejected", "Problem" };

typedef struct {
	/* saved */
	char id[16], name[64];
	int on, metric, above, forSec, approval;
	unsigned to;                 /* TO_*: where it goes */
	double value;
	/* running (main loop, behind mu) */
	guint declId;
	int declared, sent;          /* sent: last active value sent as a camera event, -1 none yet */
	int outActive;               /* the last state sent to the other ways out (MQTT, InfluxDB, Prometheus), -1 none */
	int state, raw, settled;     /* raw: the condition this second; settled: after the wait */
	gint64 rawSince;             /* monotonic µs the raw condition last changed */
	long long since;             /* unix seconds the state began */
	double now;                  /* latest reading, NAN when none */
	char who[64];                /* who approved or rejected */
	char problem[96];
	int testing;
	guint testTimer;
} Alert;

static GRecMutex mu;                /* recursive: an event callback may come while it is held */
static Alert* list[MAX_ALERTS];
static int count = 0;
static cJSON* logLines = NULL;   /* oldest first */
static gboolean logDirty = FALSE, needSync = TRUE;
static AXEventHandler* handler = NULL;
static AlertsNeed needFn = NULL;
typedef struct { char id[16]; int action; char who[64]; } Act;   /* action: 1 approve, 2 reject, 3 test */
static GQueue acts = G_QUEUE_INIT;

static const char* opw(const Alert* a) { return a->above ? "above" : "below"; }
static void fmt_val(char* out, size_t n, int metric, double v) {
	if (isnan(v)) { g_strlcpy(out, "no reading", n); return; }
	const char* u = MET[metric].unit;
	if (fabs(v) >= 100 || v == floor(v)) snprintf(out, n, "%.0f%s%s", v, *u ? " " : "", u);
	else snprintf(out, n, "%.1f%s%s", v, *u ? " " : "", u);
}

/*-----------------------------------------------------
 * Log: what came in and exactly what was sent out (event, topic, On/Off), and who
 *-----------------------------------------------------*/
static void log_line(const Alert* a, const char* what, int sentVal, unsigned mask, const char* who) {   /* behind mu */
	cJSON* e = cJSON_CreateObject();
	cJSON_AddNumberToObject(e, "t", (double)time(NULL));
	cJSON_AddStringToObject(e, "id", a->id);
	cJSON_AddStringToObject(e, "alert", a->name);
	cJSON_AddStringToObject(e, "what", what);
	if (sentVal >= 0 && mask) {
		if (mask & TO_EVENT) {
			gchar* ev = g_strdup_printf("%s › %s", APP_NAME, a->name);
			gchar* topic = g_strdup_printf("tnsaxis:CameraApplicationPlatform/%s/%s", APP_PACKAGE, a->id);
			cJSON_AddStringToObject(e, "event", ev);
			cJSON_AddStringToObject(e, "topic", topic);
			g_free(ev); g_free(topic);
		}
		cJSON* to = cJSON_AddArrayToObject(e, "to");           /* 1.5.0: every way out it went to */
		for (int i = 0; i < 4; i++) if (mask & (1u << i)) cJSON_AddItemToArray(to, cJSON_CreateString(TO_WORD[i]));
		cJSON_AddStringToObject(e, "state", sentVal ? "On" : "Off");
	}
	cJSON_AddStringToObject(e, "who", who && *who ? who : APP_NAME);
	cJSON_AddItemToArray(logLines, e);
	while (cJSON_GetArraySize(logLines) > LOG_MAX) cJSON_DeleteItemFromArray(logLines, 0);
	logDirty = TRUE;
}

/*-----------------------------------------------------
 * Events (main loop only)
 *-----------------------------------------------------*/
static void context_text(const Alert* a, int on, char* out, size_t n) {
	char v[32], lim[32];
	fmt_val(v, sizeof(v), a->metric, a->now);
	fmt_val(lim, sizeof(lim), a->metric, a->value);
	if (on) snprintf(out, n, "%s %s (%s %s for %d s)", MET[a->metric].name, v, opw(a), lim, a->forSec);
	else snprintf(out, n, "%s %s (back to normal)", MET[a->metric].name, v);
}

static int send_event(Alert* a, int on, const char* ctx) {
	if (!a->declId) return 0;
	if (!a->declared) return 1;              /* announced as soon as it is published (declared_cb) */
	AXEventKeyValueSet* set = ax_event_key_value_set_new();
	ax_event_key_value_set_add_key_value(set, "active", NULL, &on, AX_VALUE_TYPE_BOOL, NULL);
	ax_event_key_value_set_add_key_value(set, "context", NULL, ctx ? ctx : "", AX_VALUE_TYPE_STRING, NULL);
	AXEvent* ev = ax_event_new2(set, NULL);
	ax_event_key_value_set_free(set);
	GError* err = NULL;
	int ok = ax_event_handler_send_event(handler, a->declId, ev, &err) ? 1 : 0;
	if (!ok) {
		LOG("Alert %s: could not send the event: %s\n", a->id, err ? err->message : "unknown");
		g_strlcpy(a->problem, "Could not send the event", sizeof(a->problem));
		if (err) g_error_free(err);
	} else {
		a->sent = on;
	}
	ax_event_free(ev);
	return ok;
}

/* 1.5.0: the alert's state to every way out it chose; returns where it went (TO_*). The camera event only when
 * published; MQTT and InfluxDB are queued for the send-out thread (sent only while that way is on in Settings). */
static unsigned send_out(Alert* a, int on, const char* ctx) {
	unsigned m = 0;
	if ((a->to & TO_EVENT) && send_event(a, on, ctx)) m |= TO_EVENT;
	if (a->to & TO_MQTT) { sendout_alert_mqtt(a->id, a->name, on, ctx); m |= TO_MQTT; }
	if (a->to & TO_INFLUX) { sendout_alert_influx(a->id, a->name, on, ctx); m |= TO_INFLUX; }
	if (a->to & TO_PROM) m |= TO_PROM;
	a->outActive = on;
	return m;
}
/* The camera event could not be sent although it was chosen */
static gboolean event_failed(const Alert* a, unsigned m) { return (a->to & TO_EVENT) && !(m & TO_EVENT); }

static void declared_cb(guint decl, gpointer user_data) {
	(void)user_data;
	g_rec_mutex_lock(&mu);
	for (int i = 0; i < count; i++) {
		Alert* a = list[i];
		if (a->declId != decl) continue;
		a->declared = 1;
		/* Tell subscribers the current state (Off, or On if it already triggered while declaring) */
		char ctx[160];
		context_text(a, a->state == ST_TRIG, ctx, sizeof(ctx));
		send_event(a, a->state == ST_TRIG, ctx);
		log_line(a, "Event published; current state announced", a->state == ST_TRIG, TO_EVENT, NULL);
	}
	g_rec_mutex_unlock(&mu);
}

static void declare(Alert* a) {
	AXEventKeyValueSet* set = ax_event_key_value_set_new();
	int initial = 0;
	ax_event_key_value_set_add_key_value(set, "topic0", "tnsaxis", "CameraApplicationPlatform", AX_VALUE_TYPE_STRING, NULL);
	ax_event_key_value_set_add_key_value(set, "topic1", "tnsaxis", APP_PACKAGE, AX_VALUE_TYPE_STRING, NULL);
	ax_event_key_value_set_add_nice_names(set, "topic1", "tnsaxis", APP_PACKAGE, APP_NAME, NULL);
	ax_event_key_value_set_add_key_value(set, "topic2", "tnsaxis", a->id, AX_VALUE_TYPE_STRING, NULL);
	ax_event_key_value_set_add_nice_names(set, "topic2", "tnsaxis", a->id, a->name, NULL);
	ax_event_key_value_set_add_key_value(set, "active", NULL, &initial, AX_VALUE_TYPE_BOOL, NULL);
	ax_event_key_value_set_mark_as_data(set, "active", NULL, NULL);
	ax_event_key_value_set_add_key_value(set, "context", NULL, "", AX_VALUE_TYPE_STRING, NULL);
	ax_event_key_value_set_add_nice_names(set, "context", NULL, "Context", NULL, NULL);
	ax_event_key_value_set_mark_as_data(set, "context", NULL, NULL);
	GError* err = NULL;
	guint id = 0;
	a->declared = 0;
	a->sent = -1;
	/* Stateful (property) event, "active" its only state */
	if (ax_event_handler_declare2(handler, set, FALSE, (gchar*)"active", &id, declared_cb, NULL, &err)) {
		a->declId = id;
	} else {
		a->declId = 0;
		LOG("Alert %s: could not declare the event: %s\n", a->id, err ? err->message : "unknown");
		g_strlcpy(a->problem, "Could not publish the event", sizeof(a->problem));
		a->state = ST_PROBLEM;
		if (err) g_error_free(err);
	}
	ax_event_key_value_set_free(set);
}

static void undeclare(Alert* a) {
	if (a->testTimer) { g_source_remove(a->testTimer); a->testTimer = 0; }
	if (a->declId) {
		if (a->declared && a->sent == 1) send_event(a, 0, "Alert changed or removed");
		ax_event_handler_undeclare(handler, a->declId, NULL);
	}
	a->declId = 0;
	a->declared = 0;
	a->sent = -1;
}

static gboolean test_off(gpointer user_data) {
	char* id = user_data;
	g_rec_mutex_lock(&mu);
	for (int i = 0; i < count; i++) {
		Alert* a = list[i];
		if (strcmp(a->id, id)) continue;
		a->testTimer = 0;
		a->testing = 0;
		int back = a->state == ST_TRIG;          /* back to the real state */
		char ctx[160];
		if (back) context_text(a, 1, ctx, sizeof(ctx)); else g_strlcpy(ctx, "Test", sizeof(ctx));
		unsigned m = send_out(a, back, ctx);
		if (m) log_line(a, back ? "Test over: back to its real state (Triggered)" : "Test: sent Off", back, m, NULL);
	}
	g_rec_mutex_unlock(&mu);
	return G_SOURCE_REMOVE;
}

/*-----------------------------------------------------
 * Load and save
 *-----------------------------------------------------*/
static int metric_of(const char* key) {
	for (int i = 0; key && i < METRICS; i++) if (!strcmp(MET[i].key, key)) return i;
	return -1;
}

static gboolean good_id(const char* s) {
	if (!s || !*s || strlen(s) >= 16) return FALSE;
	for (; *s; s++) if (!g_ascii_islower(*s) && !g_ascii_isdigit(*s)) return FALSE;
	return TRUE;
}

/* Checks one list; fills out[] (new Alerts, only saved fields). Returns NULL or why not. */
static gchar* parse(const cJSON* in, Alert** out, int* n) {
	*n = 0;
	const cJSON* arr = cJSON_IsObject(in) ? cJSON_GetObjectItem(in, "alerts") : NULL;
	if (!cJSON_IsArray(arr)) return g_strdup("Send {\"alerts\":[...]}.");
	if (cJSON_GetArraySize(arr) > MAX_ALERTS) return g_strdup_printf("At most %d alerts.", MAX_ALERTS);
	const cJSON* it;
	int next = 1;
	cJSON_ArrayForEach(it, arr) {                         /* the next free number for new ones */
		const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
		if (id && g_str_has_prefix(id, "alert")) { int k = atoi(id + 5); if (k >= next) next = k + 1; }
	}
	gchar* why = NULL;
	cJSON_ArrayForEach(it, arr) {
		Alert* a = g_new0(Alert, 1);
		out[(*n)++] = a;
		int pos = *n;
		if (!cJSON_IsObject(it)) { why = g_strdup_printf("Alert %d: not an object.", pos); break; }
		const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
		if (id && *id) {
			if (!good_id(id)) { why = g_strdup_printf("Alert %d: the id may use only a-z and 0-9.", pos); break; }
			g_strlcpy(a->id, id, sizeof(a->id));
		} else {
			snprintf(a->id, sizeof(a->id), "alert%d", next++);
		}
		for (int j = 0; j < *n - 1; j++) if (!strcmp(out[j]->id, a->id)) { why = g_strdup_printf("Alert %d: the id %s is used twice.", pos, a->id); break; }
		if (why) break;
		const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
		gchar* nm = g_strstrip(g_strdup(name ? name : ""));
		if (strlen(nm) >= sizeof(a->name)) { g_free(nm); why = g_strdup_printf("Alert %d: the name is too long (at most %zu characters).", pos, sizeof(a->name) - 1); break; }
		g_strlcpy(a->name, nm, sizeof(a->name));
		g_free(nm);
		const cJSON* on = cJSON_GetObjectItem(it, "on");
		a->on = cJSON_IsTrue(on);
		a->metric = metric_of(cJSON_GetStringValue(cJSON_GetObjectItem(it, "metric")));
		const char* op = cJSON_GetStringValue(cJSON_GetObjectItem(it, "op"));
		a->above = !op || strcmp(op, "below") != 0;
		const cJSON* v = cJSON_GetObjectItem(it, "value");
		const cJSON* f = cJSON_GetObjectItem(it, "for");
		a->approval = 0;   /* 1.6.1: no approval step any more; an old "approval":true is ignored */
		const cJSON* to = cJSON_GetObjectItem(it, "to");
		if (cJSON_IsObject(to)) {
			a->to = (cJSON_IsTrue(cJSON_GetObjectItem(to, "event")) ? TO_EVENT : 0) | (cJSON_IsTrue(cJSON_GetObjectItem(to, "mqtt")) ? TO_MQTT : 0) |
				(cJSON_IsTrue(cJSON_GetObjectItem(to, "influx")) ? TO_INFLUX : 0) | (cJSON_IsTrue(cJSON_GetObjectItem(to, "prom")) ? TO_PROM : 0);
		} else a->to = TO_EVENT;                          /* before 1.5.0: the camera event only */
		a->value = cJSON_IsNumber(v) ? v->valuedouble : NAN;
		a->forSec = cJSON_IsNumber(f) ? (int)f->valuedouble : 30;
		/* An alert that is switched off may be saved unfinished */
		if (!a->on) { if (a->metric < 0) a->metric = 0; if (isnan(a->value)) a->value = 0; if (a->forSec < 0 || a->forSec > 3600) a->forSec = 30; continue; }
		const char* label = *a->name ? a->name : a->id;
		if (!*a->name) { why = g_strdup_printf("Alert %d: give it a name.", pos); break; }
		if (a->metric < 0) { why = g_strdup_printf("%s: pick what to watch.", label); break; }
		if (isnan(a->value) || a->value < -1e6 || a->value > 1e9) { why = g_strdup_printf("%s: give a limit (a number).", label); break; }
		if (!cJSON_IsNumber(f) || f->valuedouble != floor(f->valuedouble) || a->forSec < 0 || a->forSec > 3600) { why = g_strdup_printf("%s: For must be a whole number of seconds from 0 to 3600.", label); break; }
		if (!a->to) { why = g_strdup_printf("%s: pick at least one Send to.", label); break; }
	}
	if (why) { for (int j = 0; j < *n; j++) g_free(out[j]); *n = 0; }
	return why;
}

static cJSON* saved_json(const Alert* a) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "id", a->id);
	cJSON_AddStringToObject(o, "name", a->name);
	cJSON_AddBoolToObject(o, "on", a->on);
	cJSON_AddStringToObject(o, "metric", MET[a->metric].key);
	cJSON_AddStringToObject(o, "op", opw(a));
	cJSON_AddNumberToObject(o, "value", a->value);
	cJSON_AddNumberToObject(o, "for", a->forSec);
	cJSON_AddBoolToObject(o, "approval", a->approval);
	cJSON* to = cJSON_AddObjectToObject(o, "to");
	cJSON_AddBoolToObject(to, "event", (a->to & TO_EVENT) != 0);
	cJSON_AddBoolToObject(to, "mqtt", (a->to & TO_MQTT) != 0);
	cJSON_AddBoolToObject(to, "influx", (a->to & TO_INFLUX) != 0);
	cJSON_AddBoolToObject(to, "prom", (a->to & TO_PROM) != 0);
	return o;
}

static gboolean write_list(Alert** l, int n) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "schema", 1);
	cJSON* arr = cJSON_AddArrayToObject(o, "alerts");
	for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, saved_json(l[i]));
	char* txt = cJSON_Print(o);
	cJSON_Delete(o);
	gboolean ok = txt && g_file_set_contents(ALERTS_FILE, txt, -1, NULL);   /* written to a new file, then put in place */
	free(txt);
	return ok;
}

static unsigned groups_needed(void) {                     /* behind mu */
	unsigned g = 0;
	for (int i = 0; i < count; i++) if (list[i]->on) g |= MET[list[i]->metric].group;
	return g;
}

static void reset_run(Alert* a) {
	a->state = a->on ? ST_LISTEN : ST_OFF;
	a->raw = a->settled = 0;
	a->rawSince = g_get_monotonic_time();
	a->since = (long long)time(NULL);
	a->now = NAN;
	a->who[0] = a->problem[0] = 0;
	a->outActive = -1;
}

void alerts_init(AlertsNeed need) {
	g_rec_mutex_init(&mu);
	needFn = need;
	handler = ax_event_handler_new();
	logLines = ACAP_FILE_Read(LOG_FILE);
	if (!cJSON_IsArray(logLines)) { cJSON_Delete(logLines); logLines = cJSON_CreateArray(); }
	gchar* txt = NULL;
	if (g_file_get_contents(ALERTS_FILE, &txt, NULL, NULL)) {
		cJSON* in = cJSON_Parse(txt);
		Alert* l[MAX_ALERTS];
		int n = 0;
		gchar* why = parse(in, l, &n);
		if (why) { LOG("Alerts: %s No alerts loaded.\n", why); g_free(why); }
		for (int i = 0; i < n; i++) { reset_run(l[i]); list[count++] = l[i]; }
		cJSON_Delete(in);
		g_free(txt);
	}
	needSync = TRUE;
	if (needFn) needFn(groups_needed());
}

/* Nothing changed, so it carries on as it is */
static gboolean same_rule(const Alert* a, const Alert* b) {
	return !strcmp(a->id, b->id) && !strcmp(a->name, b->name) && a->on == b->on && a->metric == b->metric && a->above == b->above && a->value == b->value && a->forSec == b->forSec && a->approval == b->approval && a->to == b->to;
}

static GPtrArray* oldList = NULL;                         /* replaced or removed alerts, for the main loop to undeclare and free */

gchar* alerts_save(const cJSON* in, const char* who) {
	Alert* l[MAX_ALERTS];
	int n = 0;
	gchar* why = parse(in, l, &n);
	if (why) return why;
	if (!write_list(l, n)) { for (int i = 0; i < n; i++) g_free(l[i]); return g_strdup("The alerts could not be saved on the camera."); }
	g_rec_mutex_lock(&mu);
	/* Keep what is running for alerts that did not change; the rest start again (their old event is taken back,
	 * Off first if it was On, and published again) */
	if (!oldList) oldList = g_ptr_array_new();
	for (int i = 0; i < n; i++) {
		Alert* a = l[i];
		Alert* was = NULL;
		int wj = -1;
		for (int j = 0; j < count; j++) if (list[j] && !strcmp(list[j]->id, a->id)) { was = list[j]; wj = j; break; }
		if (was && same_rule(was, a)) {
			*a = *was;                                       /* the same: carries on, declaration and all */
			was->declId = 0; was->testTimer = 0;
			was->outActive = -1;                             /* the old copy must not send Off for the one carrying on */
		} else {
			reset_run(a);
			log_line(a, was ? "Changed and saved" : "Added", -1, 0, who);
		}
		if (was) { g_ptr_array_add(oldList, was); list[wj] = NULL; }
	}
	for (int j = 0; j < count; j++) {
		if (!list[j]) continue;
		log_line(list[j], "Removed", -1, 0, who);
		g_ptr_array_add(oldList, list[j]);
	}
	count = n;
	for (int i = 0; i < n; i++) list[i] = l[i];
	needSync = TRUE;
	unsigned g = groups_needed();
	g_rec_mutex_unlock(&mu);
	if (needFn) needFn(g);
	return NULL;
}

gchar* alerts_act(const char* id, const char* action, const char* who) {
	int act = !action ? 0 : !strcmp(action, "approve") ? 1 : !strcmp(action, "reject") ? 2 : !strcmp(action, "test") ? 3 : 0;
	if (!act || !id) return g_strdup("Expected {\"id\":\"...\", \"action\":\"approve\"|\"reject\"|\"test\"}.");
	gchar* why = NULL;
	g_rec_mutex_lock(&mu);
	Alert* a = NULL;
	for (int i = 0; i < count; i++) if (!strcmp(list[i]->id, id)) a = list[i];
	if (!a) why = g_strdup("There is no such alert (save first).");
	else if (act == 3 && (!a->on || ((a->to & TO_EVENT) && !a->declared))) why = g_strdup("Turn the alert on and save it first; then Test sends it.");
	else if (act != 3 && a->state != ST_WAIT) why = g_strdup("This alert is not waiting for approval.");
	if (!why) {
		Act* x = g_new0(Act, 1);
		g_strlcpy(x->id, id, sizeof(x->id));
		x->action = act;
		g_strlcpy(x->who, who && *who ? who : "Unknown", sizeof(x->who));
		g_queue_push_tail(&acts, x);
	}
	g_rec_mutex_unlock(&mu);
	return why;
}

/*-----------------------------------------------------
 * Every second (main loop)
 *-----------------------------------------------------*/
static double reading(const PromSnap* p, int m) {
	double v = NAN;
	switch (m) {
	case 0: v = p->cpu; break;
	case 1: v = p->apps; break;
	case 2: v = p->load[0]; break;
	case 3: v = p->memTotal > 0 ? 100.0 * p->memUsed / p->memTotal : NAN; break;
	case 4: for (int i = 0; i < p->driveN; i++) if (p->driveTotal[i] > 0) { double x = 100.0 * p->driveUsed[i] / p->driveTotal[i]; if (isnan(v) || x > v) v = x; } break;
	case 5: for (int i = 0; i < p->sensN; i++) if (!isnan(p->sensC[i]) && (isnan(v) || p->sensC[i] > v)) v = p->sensC[i]; break;
	case 6: v = p->powerW; break;
	case 7: case 8: for (int i = 0; i < p->ifN; i++) { double x = m == 7 ? p->ifRx[i] : p->ifTx[i]; if (!isnan(x)) v = (isnan(v) ? 0 : v) + x / 1e6; } break;
	}
	return v;
}

static void become(Alert* a, int st) { a->state = st; a->since = (long long)time(NULL); }

static void run_acts(void) {                              /* behind mu */
	Act* x;
	while ((x = g_queue_pop_head(&acts))) {
		Alert* a = NULL;
		for (int i = 0; i < count; i++) if (!strcmp(list[i]->id, x->id)) a = list[i];
		if (a && x->action == 1 && a->state == ST_WAIT) {
			char ctx[160];
			context_text(a, 1, ctx, sizeof(ctx));
			g_strlcpy(a->who, x->who, sizeof(a->who));
			become(a, ST_TRIG);
			unsigned m = send_out(a, 1, ctx);
			log_line(a, event_failed(a, m) ? "Approved, but the camera event could not be sent" : "Approved: sent", m ? 1 : -1, m, x->who);
		} else if (a && x->action == 2 && a->state == ST_WAIT) {
			g_strlcpy(a->who, x->who, sizeof(a->who));
			become(a, ST_REJECT);
			log_line(a, "Rejected: nothing sent until it is back to normal and over the limit again", -1, 0, x->who);
		} else if (a && x->action == 3 && a->on && (a->declared || !(a->to & TO_EVENT))) {
			if (a->testTimer) g_source_remove(a->testTimer);
			a->testing = 1;
			unsigned m = send_out(a, 1, "Test");
			if (m) log_line(a, "Test: sent On (Off follows in 3 s)", 1, m, x->who);
			a->testTimer = g_timeout_add_full(G_PRIORITY_DEFAULT, TEST_MS, test_off, g_strdup(a->id), g_free);
		}
		g_free(x);
	}
}

void alerts_tick(void) {
	g_rec_mutex_lock(&mu);
	if (needSync) {
		needSync = FALSE;
		for (guint i = 0; oldList && i < oldList->len; i++) {
			Alert* o = g_ptr_array_index(oldList, i);
			if (o->outActive == 1) {                     /* MQTT and InfluxDB hear it is Off too */
				if (o->to & TO_MQTT) sendout_alert_mqtt(o->id, o->name, 0, "Alert changed or removed");
				if (o->to & TO_INFLUX) sendout_alert_influx(o->id, o->name, 0, "Alert changed or removed");
			}
			undeclare(o);
			g_free(o);
		}
		if (oldList) g_ptr_array_set_size(oldList, 0);
		const char* keep[MAX_ALERTS];
		int keepN = 0;
		for (int i = 0; i < count; i++) {
			Alert* a = list[i];
			gboolean ev = a->on && (a->to & TO_EVENT);
			if (ev && !a->declId) declare(a);
			if (!ev && a->declId) undeclare(a);
			if (a->on && (a->to & TO_MQTT)) keep[keepN++] = a->id;
			/* Its current state from the start (at start-up and after a save): retained on MQTT, and one InfluxDB point,
			 * so a stop, crash or self-restart while On is not left On there */
			if (a->on && (a->to & (TO_MQTT | TO_INFLUX)) && a->outActive < 0) {
				int on = a->state == ST_TRIG;
				char ctx[160];
				context_text(a, on, ctx, sizeof(ctx));
				if (a->to & TO_MQTT) sendout_alert_mqtt(a->id, a->name, on, on ? ctx : "");
				if (a->to & TO_INFLUX) sendout_alert_influx(a->id, a->name, on, on ? ctx : "Current state (Resource Monitor started or the alert was saved)");
				a->outActive = on;
			}
		}
		sendout_alert_keep(keep, keepN);                  /* the others are taken back from the broker */
	}
	run_acts();
	int any = 0;
	for (int i = 0; i < count; i++) any |= list[i]->on;
	if (any) {
		PromSnap* p = g_new0(PromSnap, 1);
		g_rec_mutex_unlock(&mu);
		sampler_snapshot(p, 0);                               /* outside mu: it takes the sampler's lock */
		g_rec_mutex_lock(&mu);
		gint64 mono = g_get_monotonic_time();
		for (int i = 0; i < count; i++) {
			Alert* a = list[i];
			if (!a->on) continue;
			a->now = reading(p, a->metric);
			if (isnan(a->now)) {
				if (a->state != ST_PROBLEM) { g_strlcpy(a->problem, "No reading right now (is it collected? see Settings, What to collect)", sizeof(a->problem)); if (a->state == ST_TRIG) { char ctx[160]; context_text(a, 0, ctx, sizeof(ctx)); unsigned m = send_out(a, 0, ctx); log_line(a, "No reading: sent Off", 0, m, NULL); } become(a, ST_PROBLEM); a->settled = a->raw = 0; }
				continue;
			}
			if (a->state == ST_PROBLEM && (a->declared || !(a->to & TO_EVENT))) { a->problem[0] = 0; become(a, ST_LISTEN); a->rawSince = mono; }
			int raw = a->above ? a->now > a->value : a->now < a->value;
			if (raw != a->raw) { a->raw = raw; a->rawSince = mono; }
			int wait = raw ? a->forSec : MAX(a->forSec, CLEAR_MIN);
			if (a->raw == a->settled || mono - a->rawSince < (gint64)wait * G_USEC_PER_SEC) continue;
			a->settled = a->raw;
			char ctx[160], v[32];
			fmt_val(v, sizeof(v), a->metric, a->now);
			if (a->settled) {
				if (a->approval) {
					become(a, ST_WAIT);
					gchar* w = g_strdup_printf("Over the limit (%s): waiting for approval, nothing sent yet", v);
					log_line(a, w, -1, 0, NULL);
					g_free(w);
				} else {
					become(a, ST_TRIG);
					context_text(a, 1, ctx, sizeof(ctx));
					unsigned m = send_out(a, 1, ctx);
					gchar* w = g_strdup_printf("Over the limit (%s)%s", v, event_failed(a, m) ? ": the camera event could not be sent" : "");
					log_line(a, w, m ? 1 : -1, m, NULL);
					g_free(w);
				}
			} else {
				int was = a->state;
				become(a, ST_LISTEN);
				a->who[0] = 0;
				gchar* w = g_strdup_printf("Back to normal (%s)%s", v, was == ST_WAIT ? ": no one decided, nothing was sent" : was == ST_REJECT ? "" : "");
				if (was == ST_TRIG) {
					context_text(a, 0, ctx, sizeof(ctx));
					unsigned m = send_out(a, 0, ctx);
					log_line(a, w, m ? 0 : -1, m, NULL);
				} else {
					log_line(a, w, -1, 0, NULL);
				}
				g_free(w);
			}
		}
		g_free(p);
	}
	/* The log goes to storage at most every 10 s */
	static gint64 wrote = 0;
	if (logDirty && g_get_monotonic_time() - wrote > 10 * G_USEC_PER_SEC) {
		ACAP_FILE_Write(LOG_FILE, logLines);
		logDirty = FALSE;
		wrote = g_get_monotonic_time();
	}
	g_rec_mutex_unlock(&mu);
}

void alerts_stop(void) {
	g_rec_mutex_lock(&mu);
	for (int i = 0; i < count; i++) {
		Alert* a = list[i];
		if (a->outActive == 1) {                         /* MQTT and InfluxDB hear it is Off (sent if they still can) */
			if (a->to & TO_MQTT) sendout_alert_mqtt(a->id, a->name, 0, "Resource Monitor stopped");
			if (a->to & TO_INFLUX) sendout_alert_influx(a->id, a->name, 0, "Resource Monitor stopped");
		}
		undeclare(a);
	}
	if (logDirty) ACAP_FILE_Write(LOG_FILE, logLines);
	logDirty = FALSE;
	g_rec_mutex_unlock(&mu);
}

cJSON* alerts_json(void) {
	cJSON* o = cJSON_CreateObject();
	cJSON* m = cJSON_AddArrayToObject(o, "metrics");
	for (int i = 0; i < METRICS; i++) {
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "key", MET[i].key);
		cJSON_AddStringToObject(e, "name", MET[i].name);
		cJSON_AddStringToObject(e, "unit", MET[i].unit);
		cJSON_AddNumberToObject(e, "group", MET[i].group);
		cJSON_AddItemToArray(m, e);
	}
	{                                                   /* 1.5.0: which ways out are on in Settings */
		MqttCfg* m = g_new0(MqttCfg, 1);
		InfluxCfg* x = g_new0(InfluxCfg, 1);
		unsigned send = 0;
		config_out(m, x, &send);
		cJSON* w = cJSON_AddObjectToObject(o, "ways");
		cJSON_AddBoolToObject(w, "mqtt", m->on);
		cJSON_AddBoolToObject(w, "haDiscovery", m->on && m->ha);
		cJSON_AddBoolToObject(w, "influx", x->on);
		cJSON_AddBoolToObject(w, "prom", config_metrics(&send));
		g_free(m);
		g_free(x);
	}
	g_rec_mutex_lock(&mu);
	cJSON* arr = cJSON_AddArrayToObject(o, "alerts");
	for (int i = 0; i < count; i++) {
		const Alert* a = list[i];
		cJSON* e = saved_json(a);
		cJSON* s = cJSON_AddObjectToObject(e, "status");
		cJSON_AddStringToObject(s, "word", ST_WORD[a->state]);
		cJSON_AddNumberToObject(s, "since", (double)a->since);
		if (!isnan(a->now)) cJSON_AddNumberToObject(s, "now", a->now);
		if (a->who[0]) cJSON_AddStringToObject(s, "who", a->who);
		if (a->problem[0]) cJSON_AddStringToObject(s, "problem", a->problem);
		cJSON_AddBoolToObject(s, "published", a->declared);
		cJSON_AddBoolToObject(s, "testing", a->testing);
		cJSON_AddBoolToObject(s, "active", a->outActive == 1 || (a->sent == 1 && (a->to & TO_EVENT)));
		gchar* topic = g_strdup_printf("tnsaxis:CameraApplicationPlatform/%s/%s", APP_PACKAGE, a->id);
		cJSON_AddStringToObject(s, "topic", topic);
		g_free(topic);
		cJSON_AddItemToArray(arr, e);
	}
	cJSON_AddItemToObject(o, "log", cJSON_Duplicate(logLines, 1));
	g_rec_mutex_unlock(&mu);
	return o;
}

/* 1.5.0: the Prometheus page's alert gauges, for alerts that chose Prometheus (to free; NULL when none) */
char* alerts_prom(void) {
	GString* g = NULL;
	g_rec_mutex_lock(&mu);
	for (int i = 0; i < count; i++) {
		const Alert* a = list[i];
		if (!a->on || !(a->to & TO_PROM)) continue;
		if (!g) g = g_string_new("# HELP resource_monitor_alert_active 1 while the alert is active (sent On), else 0.\n# TYPE resource_monitor_alert_active gauge\n");
		g_string_append(g, "resource_monitor_alert_active{alert=\"");
		for (const char* p = a->name; *p; p++) {
			if (*p == '\\') g_string_append(g, "\\\\");
			else if (*p == '"') g_string_append(g, "\\\"");
			else if (*p == '\n') g_string_append(g, "\\n");
			else g_string_append_c(g, *p);
		}
		g_string_append_printf(g, "\",id=\"%s\"} %d\n", a->id, a->outActive == 1 ? 1 : 0);
	}
	g_rec_mutex_unlock(&mu);
	return g ? g_string_free(g, FALSE) : NULL;
}
