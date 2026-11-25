/*
 * sendout.c (1.3.3): the latest reading sent out over MQTT and to InfluxDB, every 10 to 300 s, on its own thread.
 * Only the groups whose Send switch is on, with the same metric names and labels as the Prometheus page (prom.c).
 * Passwords and tokens never leave this file except to the broker or the database, and are never logged.
 *
 * MQTT uses the camera's own Paho library (libpaho-mqtt3as.so.1, part of AXIS OS), loaded when needed; nothing is
 * linked or downloaded at build time. The loading and connection code is adapted from Fred Juhlin's ACAP MQTT
 * template (MQTT.c), used under the MIT License:
 *
 *   Copyright (c) 2025 Fred Juhlin <fred.juhlin@gmail.com>
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
 *   copyright notice and this permission notice shall be included in all copies or substantial portions of the
 *   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 *   LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 *   SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *   OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 *
 * The Paho headers (MQTTAsync.h and the ones it includes) are Eclipse Paho, EPL-2.0 / EDL-1.0, as their own heads say.
 *
 * InfluxDB: line protocol POSTed with libcurl (timeout 10 s). A batch that could not be sent is kept and tried again;
 * at most the last 10 are kept, older ones are dropped (nothing only grows).
 *
 * 1.5.0 Alerts out: each alert that chose MQTT has its state retained at <prefix>/alert/<id>, published on every change
 * and on each connect, with a Home Assistant binary_sensor when discovery is on; each change of an alert that chose
 * InfluxDB is one point, resource_monitor_alert, sent through the same queue within a second.
 */
typedef struct MQTTPersistence_beforeWrite MQTTPersistence_beforeWrite;
typedef struct MQTTPersistence_afterRead MQTTPersistence_afterRead;
#define NO_PERSISTENCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <curl/curl.h>
#include "MQTTAsync.h"
#include "cJSON.h"
#include "config.h"
#include "prom.h"
#include "sampler.h"
#include "sendout.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define BACKLOG     10                          /* InfluxDB batches kept while it can't be reached */
#define MQTT_CA     "localdata/mqtt-ca.pem"
#define INFLUX_CA   "localdata/influx-ca.pem"
#define SYSTEM_CA   "/etc/ssl/certs/ca-certificates.crt"

/*----------------------------------------------------- device facts and status */
static void (*logLine)(const char*) = NULL;
static char devSerial[32] = "", devModel[64] = "", devOs[32] = "", devVer[24] = "";
static GMutex stLock;
typedef struct { char state[16]; char text[200]; int kind; int loggedKind; } OutStatus;   /* kind: 0 off, 1 fine, 2 failing */
static OutStatus stMqtt = { "off", "Off", 0, 0 }, stInflux = { "off", "Off", 0, 0 };
static int influxWaiting = 0;
static volatile int alertAnnounce = 0;          /* 1.5.0: every alert's state again on the next round (after a connect) */

/* state: off, connecting, connected, sending, failing. Logged once each time it goes between off, fine and failing. */
static void set_status(OutStatus* s, const char* name, const char* state, const char* fmt, ...) {
	char text[200];
	va_list a;
	va_start(a, fmt);
	vsnprintf(text, sizeof(text), fmt, a);
	va_end(a);
	int kind = !strcmp(state, "off") ? 0 : !strcmp(state, "failing") ? 2 : 1;
	g_mutex_lock(&stLock);
	g_strlcpy(s->state, state, sizeof(s->state));
	g_strlcpy(s->text, text, sizeof(s->text));
	s->kind = kind;
	gboolean log = kind != s->loggedKind && strcmp(state, "connecting");
	if (log) s->loggedKind = kind;
	g_mutex_unlock(&stLock);
	if (log) {
		gchar* line = g_strdup_printf("%s: %s", name, text);
		LOG("%s\n", line);
		if (logLine) logLine(line);
		g_free(line);
	}
}

static void write_private(const char* path, const char* text) {          /* 0600: this app only */
	if (!text || !*text) { unlink(path); return; }
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return;
	size_t n = strlen(text);
	if (write(fd, text, n) != (ssize_t)n) LOG("Could not write %s\n", path);
	close(fd);
}
static void snapshot(PromSnap* p, unsigned send) {
	memset(p, 0, sizeof(*p));
	g_strlcpy(p->serial, devSerial, sizeof(p->serial)); g_strlcpy(p->model, devModel, sizeof(p->model));
	g_strlcpy(p->os, devOs, sizeof(p->os)); g_strlcpy(p->appVer, devVer, sizeof(p->appVer));
	sampler_snapshot(p, send);
}

/*----------------------------------------------------- MQTT (the camera's Paho library) */
static struct {
	int (*create)(MQTTAsync*, const char*, const char*, int, void*);
	int (*connect)(MQTTAsync, const MQTTAsync_connectOptions*);
	int (*disconnect)(MQTTAsync, const MQTTAsync_disconnectOptions*);
	int (*isConnected)(MQTTAsync);
	int (*sendMessage)(MQTTAsync, const char*, const MQTTAsync_message*, MQTTAsync_responseOptions*);
	int (*setCallbacks)(MQTTAsync, void*, MQTTAsync_connectionLost*, MQTTAsync_messageArrived*, MQTTAsync_deliveryComplete*);
	int (*setConnected)(MQTTAsync, void*, MQTTAsync_connected*);
	void (*freeMessage)(MQTTAsync_message**);
	void (*free)(void*);
	void (*destroy)(MQTTAsync*);
} paho;
static void* pahoLib = NULL;
static gboolean paho_load(void) {
	if (pahoLib) return TRUE;
	void* h = dlopen("libpaho-mqtt3as.so.1", RTLD_LAZY);
	if (!h) return FALSE;
#define SYM(f, name) if (!(*(void**)&paho.f = dlsym(h, name))) { dlclose(h); return FALSE; }
	SYM(create, "MQTTAsync_create") SYM(connect, "MQTTAsync_connect") SYM(disconnect, "MQTTAsync_disconnect")
	SYM(isConnected, "MQTTAsync_isConnected") SYM(sendMessage, "MQTTAsync_sendMessage") SYM(setCallbacks, "MQTTAsync_setCallbacks")
	SYM(setConnected, "MQTTAsync_setConnected") SYM(freeMessage, "MQTTAsync_freeMessage") SYM(free, "MQTTAsync_free") SYM(destroy, "MQTTAsync_destroy")
#undef SYM
	pahoLib = h;
	return TRUE;
}

typedef struct {
	MQTTAsync c;
	MqttCfg cfg;
	char uri[200], clientId[96], prefix[160], avail[200];
	MQTTAsync_SSLOptions ssl;
	MQTTAsync_willOptions will;
	volatile int connected, failed, announce;   /* announce: say online (and discovery) on the next round */
	int test;                                   /* the short Test connection: never announces the alerts */
	char why[160];
	char haTopics[24][160];                     /* discovery topics published, so they can be taken back */
	int haN;
} Mq;
static Mq* mq = NULL;                           /* the running connection (send-out thread only) */

static void mq_names(Mq* m) {
	g_snprintf(m->prefix, sizeof(m->prefix), "%s", m->cfg.prefix[0] ? m->cfg.prefix : "");
	if (!m->prefix[0]) g_snprintf(m->prefix, sizeof(m->prefix), "axis/%s/resource_monitor", devSerial[0] ? devSerial : "camera");
	if (m->cfg.clientId[0]) g_strlcpy(m->clientId, m->cfg.clientId, sizeof(m->clientId));
	else g_snprintf(m->clientId, sizeof(m->clientId), "resource_monitor-%s", devSerial[0] ? devSerial : "camera");
	g_snprintf(m->uri, sizeof(m->uri), "%s://%s:%d", m->cfg.tls ? "ssl" : "tcp", m->cfg.host, m->cfg.port);
	g_snprintf(m->avail, sizeof(m->avail), "%s/availability", m->prefix);
}
static const char* paho_why(int code) {
	switch (code) {
		case MQTTASYNC_BAD_UTF8_STRING: return "a setting is not valid text";
		case MQTTASYNC_NULL_PARAMETER: return "a setting is missing";
		case MQTTASYNC_BAD_STRUCTURE: return "the camera's MQTT library is too old";
		case MQTTASYNC_BAD_PROTOCOL: return "the address must be a host name or IP address";
		case 1: return "the broker does not speak this MQTT version";
		case 2: return "the broker refused the client id";
		case 3: return "the broker is not available";
		case 4: return "the broker refused the user name or password";
		case 5: return "the broker refused this client: check the user name and password";
		default: return NULL;
	}
}
static void on_lost(void* ctx, char* cause) {
	Mq* m = ctx;
	m->connected = 0;
	set_status(&stMqtt, "MQTT", "failing", "Not reachable: the connection to %s was lost%s%s%s; trying again", m->cfg.host, cause ? " (" : "", cause ? cause : "", cause ? ")" : "");
}
static int on_message(void* ctx, char* topic, int len, MQTTAsync_message* msg) {
	(void)ctx; (void)len;
	paho.freeMessage(&msg);
	paho.free(topic);
	return 1;
}
static void on_connected(void* ctx, char* cause) {             /* first connect and every reconnect */
	Mq* m = ctx;
	(void)cause;
	m->connected = 1; m->failed = 0; m->announce = 1;
	if (!m->test) alertAnnounce = 1;
	set_status(&stMqtt, "MQTT", "connected", "Connected to %s:%d", m->cfg.host, m->cfg.port);
}
static void on_connect_ok(void* ctx, MQTTAsync_successData* r) { (void)r; on_connected(ctx, NULL); }
static void on_connect_fail(void* ctx, MQTTAsync_failureData* r) {
	Mq* m = ctx;
	m->connected = 0; m->failed = 1;
	const char* w = r ? paho_why(r->code) : NULL;
	g_snprintf(m->why, sizeof(m->why), "%s", w ? w : r && r->message ? r->message : "no answer");
	set_status(&stMqtt, "MQTT", "failing", "Not reachable: %s (%s:%d); trying again", m->why, m->cfg.host, m->cfg.port);
}
static int mq_publish(Mq* m, const char* topic, const char* payload, int retained) {
	MQTTAsync_message msg = MQTTAsync_message_initializer;
	msg.payload = (void*)payload;
	msg.payloadlen = (int)strlen(payload);
	msg.qos = 0;
	msg.retained = retained;
	MQTTAsync_responseOptions o = MQTTAsync_responseOptions_initializer;
	return paho.sendMessage(m->c, topic, &msg, &o) == MQTTASYNC_SUCCESS;
}
/*----------------------------------------------------- 1.5.0: alerts out */
#define OUT_ALERTS 40                           /* alerts (20 at most) and ones being taken back */
#define IX_ALERT_MAX 200                        /* InfluxDB alert points waiting, at most */
typedef struct { char id[16], name[64], ctx[256]; int active, dirty, gone, ha; long long t; unsigned ver; } OutAlert;   /* ver: changes each update */
static GMutex alLock;                           /* statically allocated: no init needed */
static OutAlert outA[OUT_ALERTS];
static int outN = 0;
static GPtrArray* ixAlert = NULL;               /* InfluxDB lines waiting (alLock) */

static OutAlert* out_find(const char* id) {    /* alLock held */
	for (int i = 0; i < outN; i++) if (!strcmp(outA[i].id, id)) return &outA[i];
	return NULL;
}
void sendout_alert_mqtt(const char* id, const char* name, int active, const char* context) {
	if (!id || !*id) return;
	g_mutex_lock(&alLock);
	OutAlert* e = out_find(id);
	if (!e && outN == OUT_ALERTS) {                 /* full: drop one being taken back, else the oldest */
		int k = 0;
		for (int i = 0; i < outN; i++) if (outA[i].gone) { k = i; break; }
		memmove(&outA[k], &outA[k + 1], sizeof(OutAlert) * (size_t)(outN - k - 1));
		outN--;
	}
	if (!e) { e = &outA[outN++]; memset(e, 0, sizeof(*e)); g_strlcpy(e->id, id, sizeof(e->id)); }
	if (strcmp(e->name, name ? name : id)) e->ha = 0;  /* renamed: Home Assistant gets the new name */
	g_strlcpy(e->name, name ? name : id, sizeof(e->name));
	e->ver++;
	g_strlcpy(e->ctx, context ? context : "", sizeof(e->ctx));
	e->active = active ? 1 : 0;
	e->t = (long long)time(NULL);
	e->dirty = 1;
	e->gone = 0;
	g_mutex_unlock(&alLock);
}
void sendout_alert_keep(const char* const* ids, int n) {
	g_mutex_lock(&alLock);
	for (int i = 0; i < outN; i++) {
		gboolean keep = FALSE;
		for (int j = 0; j < n && !keep; j++) keep = ids[j] && !strcmp(ids[j], outA[i].id);
		if (!keep && !outA[i].gone) { outA[i].gone = 1; outA[i].dirty = 1; }
	}
	g_mutex_unlock(&alLock);
}
/* Tag values: , = space and \ escaped; a field string: " and \ escaped */
static void ix_tag(GString* g, const char* v) {
	for (; v && *v; v++) { if (*v == '\n' || *v == '\r') continue; if (*v == ',' || *v == '=' || *v == ' ' || *v == '\\') g_string_append_c(g, '\\'); g_string_append_c(g, *v); }
}
static void ix_str(GString* g, const char* v) {
	for (; v && *v; v++) { if (*v == '\n' || *v == '\r') continue; if (*v == '"' || *v == '\\') g_string_append_c(g, '\\'); g_string_append_c(g, *v); }
}
void sendout_alert_influx(const char* id, const char* name, int active, const char* context) {
	GString* g = g_string_new("resource_monitor_alert,alert=");
	ix_tag(g, name && *name ? name : id);
	g_string_append(g, ",id=");
	ix_tag(g, id);
	g_string_append(g, ",model=");
	ix_tag(g, devModel[0] ? devModel : "unknown");
	g_string_append(g, ",serial=");
	ix_tag(g, devSerial[0] ? devSerial : "unknown");
	g_string_append_printf(g, " active=%di,context=\"", active ? 1 : 0);
	ix_str(g, context);
	g_string_append_printf(g, "\" %lld\n", (long long)time(NULL));
	g_mutex_lock(&alLock);
	if (!ixAlert) ixAlert = g_ptr_array_new_with_free_func(g_free);
	if (ixAlert->len >= IX_ALERT_MAX) g_ptr_array_remove_index(ixAlert, 0);
	g_ptr_array_add(ixAlert, g_string_free(g, FALSE));
	g_mutex_unlock(&alLock);
}
static gchar* ha_node(void) {
	gchar* node = g_strdup_printf("rm_%s", devSerial[0] ? devSerial : "camera");
	for (char* p = node; *p; p++) if (!g_ascii_isalnum(*p) && *p != '_') *p = '_';
	return node;
}
static void out_topics(const char* prefix, const OutAlert* e, char* st, size_t stN, char* ha, size_t haN) {
	g_snprintf(st, stN, "%s/alert/%s", prefix, e->id);
	gchar* node = ha_node();
	g_snprintf(ha, haN, "homeassistant/binary_sensor/%s/alert_%s/config", node, e->id);
	g_free(node);
}
static void mq_alerts_takeback(Mq* m);

static Mq* mq_start(const MqttCfg* cfg, const char* idSuffix, gboolean withWill, char* why, size_t whyN) {
	if (!paho_load()) { g_snprintf(why, whyN, "this camera has no MQTT library (libpaho-mqtt3as)"); return NULL; }
	Mq* m = g_new0(Mq, 1);
	m->cfg = *cfg;
	mq_names(m);
	if (idSuffix) g_strlcat(m->clientId, idSuffix, sizeof(m->clientId));
	m->test = idSuffix != NULL;
	int rc = paho.create(&m->c, m->uri, m->clientId, MQTTCLIENT_PERSISTENCE_NONE, NULL);
	if (rc != MQTTASYNC_SUCCESS) {
		const char* w = paho_why(rc);
		g_snprintf(why, whyN, "%s", w ? w : "the client could not be made");
		g_free(m);
		return NULL;
	}
	paho.setCallbacks(m->c, m, on_lost, on_message, NULL);
	paho.setConnected(m->c, m, on_connected);
	MQTTAsync_connectOptions o = MQTTAsync_connectOptions_initializer;
	o.keepAliveInterval = 60;
	o.cleansession = 1;
	o.automaticReconnect = idSuffix ? 0 : 1;
	o.minRetryInterval = 5;
	o.maxRetryInterval = 60;
	o.connectTimeout = 10;
	o.onSuccess = on_connect_ok;
	o.onFailure = on_connect_fail;
	o.context = m;
	if (m->cfg.user[0]) o.username = m->cfg.user;
	if (m->cfg.pass[0]) o.password = m->cfg.pass;
	if (m->cfg.tls) {
		m->ssl = (MQTTAsync_SSLOptions)MQTTAsync_SSLOptions_initializer;
		m->ssl.trustStore = m->cfg.ca[0] ? MQTT_CA : SYSTEM_CA;
		m->ssl.enableServerCertAuth = 1;
		m->ssl.verify = m->cfg.checkName;
		o.ssl = &m->ssl;
	}
	if (withWill) {
		m->will = (MQTTAsync_willOptions)MQTTAsync_willOptions_initializer;
		m->will.topicName = m->avail;
		m->will.message = "offline";
		m->will.retained = 1;
		m->will.qos = 0;
		o.will = &m->will;
	}
	rc = paho.connect(m->c, &o);
	if (rc != MQTTASYNC_SUCCESS) {
		const char* w = paho_why(rc);
		g_snprintf(why, whyN, "%s", w ? w : "the connection could not start");
		paho.destroy(&m->c);
		g_free(m);
		return NULL;
	}
	return m;
}
static void mq_end(Mq* m, gboolean sayOffline) {
	if (!m) return;
	if (sayOffline && m->connected) {
		mq_alerts_takeback(m);
		for (int i = 0; i < m->haN; i++) mq_publish(m, m->haTopics[i], "", 1);      /* take Home Assistant's sensors back */
		mq_publish(m, m->avail, "offline", 1);
	}
	MQTTAsync_disconnectOptions d = MQTTAsync_disconnectOptions_initializer;
	d.timeout = 2000;
	if (paho.isConnected(m->c)) paho.disconnect(m->c, &d);
	for (int i = 0; i < 25 && paho.isConnected(m->c); i++) g_usleep(100000);
	paho.destroy(&m->c);
	g_free(m);
}

/* 1.5.0: the alerts' states (send-out thread, while connected): what changed, or all of them after a connect.
 * The list is copied under alLock and published without it, so the main loop is never held up by the broker. */
static void mq_alert_json(Mq* m, const OutAlert* e, const char* st, const char* ha, int* okState, int* okHa) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "alert", e->name);
	cJSON_AddStringToObject(o, "id", e->id);
	cJSON_AddBoolToObject(o, "active", e->active);
	cJSON_AddStringToObject(o, "context", e->ctx);
	cJSON_AddNumberToObject(o, "t", (double)e->t);
	char* txt = cJSON_PrintUnformatted(o);
	*okState = txt && mq_publish(m, st, txt, 1);
	free(txt);
	cJSON_Delete(o);
	if (!okHa) return;
	cJSON* c = cJSON_CreateObject();
	cJSON_AddStringToObject(c, "name", e->name);
	gchar* node = ha_node();
	gchar* uid = g_strdup_printf("%s_alert_%s", node, e->id);
	cJSON_AddStringToObject(c, "unique_id", uid);
	cJSON_AddStringToObject(c, "state_topic", st);
	cJSON_AddStringToObject(c, "value_template", "{{ 'ON' if value_json.active else 'OFF' }}");
	cJSON_AddStringToObject(c, "json_attributes_topic", st);
	cJSON_AddStringToObject(c, "availability_topic", m->avail);
	cJSON_AddStringToObject(c, "device_class", "problem");
	cJSON* d = cJSON_AddObjectToObject(c, "device");
	cJSON_AddItemToArray(cJSON_AddArrayToObject(d, "identifiers"), cJSON_CreateString(node));
	gchar* dn = g_strdup_printf("%s %s", devModel[0] ? devModel : "Camera", devSerial);
	cJSON_AddStringToObject(d, "name", dn);
	cJSON_AddStringToObject(d, "model", devModel);
	cJSON_AddStringToObject(d, "sw_version", devVer);
	txt = cJSON_PrintUnformatted(c);
	*okHa = txt && mq_publish(m, ha, txt, 1);
	free(txt);
	cJSON_Delete(c);
	g_free(node); g_free(uid); g_free(dn);
}
static void mq_alerts(Mq* m) {
	static OutAlert snap[OUT_ALERTS];             /* send-out thread only */
	char st[200], ha[200];
	g_mutex_lock(&alLock);
	int all = alertAnnounce, n = outN;
	alertAnnounce = 0;
	memcpy(snap, outA, sizeof(OutAlert) * (size_t)n);
	for (int i = 0; i < outN; i++) if (outA[i].gone) {      /* taken back below, from the copy */
		memmove(&outA[i], &outA[i + 1], sizeof(OutAlert) * (size_t)(outN - i - 1));
		outN--; i--;
	}
	g_mutex_unlock(&alLock);
	int done[OUT_ALERTS], haSet[OUT_ALERTS];            /* done: state published; haSet: 1 published, 0 taken back, -1 as it was */
	for (int i = 0; i < n; i++) {
		const OutAlert* e = &snap[i];
		done[i] = 0; haSet[i] = -1;
		out_topics(m->prefix, e, st, sizeof(st), ha, sizeof(ha));
		if (e->gone) {                                   /* taken back: no retained state, no Home Assistant sensor */
			mq_publish(m, st, "", 1);
			if (e->ha) mq_publish(m, ha, "", 1);
			continue;
		}
		gboolean wantHa = m->cfg.ha && (!e->ha || all);
		if (e->dirty || all || wantHa) {
			int okS = 0, okH = 0;
			mq_alert_json(m, e, st, ha, &okS, wantHa ? &okH : NULL);
			done[i] = okS;
			if (wantHa && okH) haSet[i] = 1;
		}
		if (!m->cfg.ha && e->ha) { mq_publish(m, ha, "", 1); haSet[i] = 0; }
	}
	g_mutex_lock(&alLock);                              /* what went out, unless it changed meanwhile */
	for (int i = 0; i < n; i++) {
		if (snap[i].gone) continue;
		OutAlert* e = out_find(snap[i].id);
		if (!e) continue;
		if (done[i] && e->ver == snap[i].ver) e->dirty = 0;
		if (haSet[i] >= 0 && e->ver == snap[i].ver) e->ha = haSet[i];
	}
	g_mutex_unlock(&alLock);
}
/* On a settings change or stop: the alerts' retained states and sensors are taken back from this broker and prefix;
 * they are published again after the next connect (copied under alLock, published without it) */
static void mq_alerts_takeback(Mq* m) {
	static OutAlert snap[OUT_ALERTS];
	char st[200], ha[200];
	g_mutex_lock(&alLock);
	int n = outN;
	memcpy(snap, outA, sizeof(OutAlert) * (size_t)n);
	for (int i = 0; i < outN; i++) {
		outA[i].ha = 0;
		outA[i].dirty = 1;
		if (outA[i].gone) { memmove(&outA[i], &outA[i + 1], sizeof(OutAlert) * (size_t)(outN - i - 1)); outN--; i--; }
	}
	g_mutex_unlock(&alLock);
	for (int i = 0; i < n; i++) {
		out_topics(m->prefix, &snap[i], st, sizeof(st), ha, sizeof(ha));
		mq_publish(m, st, "", 1);
		if (snap[i].ha) mq_publish(m, ha, "", 1);
	}
}

/* Home Assistant discovery: about 15 sensors from what is sent, retained, taken back when switched off */
typedef struct { const char* group; const char* obj; const char* name; const char* tmpl; const char* unit; const char* cls; } HaSensor;
static void ha_add(Mq* m, cJSON* groups, const char* group, const char* obj, const char* name, const char* tmpl, const char* unit, const char* cls) {
	if (m->haN >= 24 || !cJSON_GetObjectItemCaseSensitive(groups, group)) return;
	gchar* oid = g_ascii_strdown(obj, -1);
	for (char* p = oid; *p; p++) if (!g_ascii_isalnum(*p)) *p = '_';
	gchar* node = g_strdup_printf("rm_%s", devSerial[0] ? devSerial : "camera");
	for (char* p = node; *p; p++) if (!g_ascii_isalnum(*p) && *p != '_') *p = '_';
	g_snprintf(m->haTopics[m->haN], sizeof(m->haTopics[0]), "homeassistant/sensor/%s/%s/config", node, oid);
	cJSON* c = cJSON_CreateObject();
	cJSON_AddStringToObject(c, "name", name);
	gchar* uid = g_strdup_printf("%s_%s", node, oid);
	cJSON_AddStringToObject(c, "unique_id", uid);
	gchar* st = g_strdup_printf("%s/%s", m->prefix, group);
	cJSON_AddStringToObject(c, "state_topic", st);
	cJSON_AddStringToObject(c, "value_template", tmpl);
	cJSON_AddStringToObject(c, "availability_topic", m->avail);
	if (unit) cJSON_AddStringToObject(c, "unit_of_measurement", unit);
	if (cls) cJSON_AddStringToObject(c, "device_class", cls);
	cJSON_AddStringToObject(c, "state_class", "measurement");
	cJSON_AddNumberToObject(c, "expire_after", m->cfg.interval * 3);
	cJSON* d = cJSON_AddObjectToObject(c, "device");
	cJSON* ids = cJSON_AddArrayToObject(d, "identifiers");
	cJSON_AddItemToArray(ids, cJSON_CreateString(node));
	gchar* dn = g_strdup_printf("%s %s", devModel[0] ? devModel : "Camera", devSerial);
	cJSON_AddStringToObject(d, "name", dn);
	cJSON_AddStringToObject(d, "model", devModel);
	cJSON_AddStringToObject(d, "sw_version", devVer);
	char* txt = cJSON_PrintUnformatted(c);
	if (txt && mq_publish(m, m->haTopics[m->haN], txt, 1)) m->haN++;
	free(txt);
	cJSON_Delete(c);
	g_free(oid); g_free(node); g_free(uid); g_free(st); g_free(dn);
}
static void ha_publish(Mq* m, cJSON* groups) {
	m->haN = 0;
	char t[200], o[64], n[96];
	ha_add(m, groups, "cpu", "cpu", "CPU", "{{ value_json['cpu_percent'] }}", "%", NULL);
	ha_add(m, groups, "cpu", "load_1m", "Load (1 min)", "{{ value_json['load']['1m'] }}", NULL, NULL);
	ha_add(m, groups, "cpu", "iowait", "CPU waiting on disk", "{{ value_json['cpu_work_percent']['iowait'] }}", "%", NULL);
	ha_add(m, groups, "cpu", "apps_cpu", "Apps CPU", "{{ value_json['nonaxis_apps_cpu_percent'] }}", "%", NULL);
	ha_add(m, groups, "memory", "mem_used", "Memory used", "{{ value_json['memory_bytes']['used'] }}", "B", "data_size");
	ha_add(m, groups, "memory", "mem_available", "Memory available", "{{ value_json['memory_bytes']['available'] }}", "B", "data_size");
	ha_add(m, groups, "memory", "apps_mem", "Apps memory", "{{ value_json['nonaxis_apps_memory_bytes'] }}", "B", "data_size");
	cJSON* g = cJSON_GetObjectItemCaseSensitive(groups, "storage");
	cJSON* du = g ? cJSON_GetObjectItemCaseSensitive(g, "drive_used_bytes") : NULL;
	cJSON* e;
	int k = 0;
	cJSON_ArrayForEach(e, du) {
		if (k++ >= 2) break;
		g_snprintf(t, sizeof(t), "{{ value_json['drive_used_bytes']['%s'] }}", e->string);
		g_snprintf(o, sizeof(o), "drive_%s", e->string); g_snprintf(n, sizeof(n), "%s used", e->string);
		ha_add(m, groups, "storage", o, n, t, "B", "data_size");
	}
	g = cJSON_GetObjectItemCaseSensitive(groups, "network");
	cJSON* nb = g ? cJSON_GetObjectItemCaseSensitive(g, "net_bits_per_second") : NULL;
	if (nb && nb->child) {
		const char* ifn = nb->child->string;
		g_snprintf(t, sizeof(t), "{{ value_json['net_bits_per_second']['%s']['receive'] }}", ifn); g_snprintf(o, sizeof(o), "%s_rx", ifn); g_snprintf(n, sizeof(n), "%s received", ifn);
		ha_add(m, groups, "network", o, n, t, "bit/s", "data_rate");
		g_snprintf(t, sizeof(t), "{{ value_json['net_bits_per_second']['%s']['transmit'] }}", ifn); g_snprintf(o, sizeof(o), "%s_tx", ifn); g_snprintf(n, sizeof(n), "%s sent", ifn);
		ha_add(m, groups, "network", o, n, t, "bit/s", "data_rate");
	}
	g = cJSON_GetObjectItemCaseSensitive(groups, "temperature");
	cJSON* tc = g ? cJSON_GetObjectItemCaseSensitive(g, "temperature_celsius") : NULL;
	k = 0;
	cJSON_ArrayForEach(e, tc) {
		if (k++ >= 3) break;
		g_snprintf(t, sizeof(t), "{{ value_json['temperature_celsius']['%s'] }}", e->string);
		g_snprintf(o, sizeof(o), "temp_%s", e->string); g_snprintf(n, sizeof(n), "Temperature %s", e->string);
		ha_add(m, groups, "temperature", o, n, t, "\xC2\xB0" "C", "temperature");
	}
	if (g && cJSON_GetObjectItemCaseSensitive(g, "fan_rpm")) ha_add(m, groups, "temperature", "fan", "Fan", "{{ value_json['fan_rpm'] }}", "rpm", NULL);
	ha_add(m, groups, "power", "power", "Power", "{{ value_json['power_watts'] }}", "W", "power");
	ha_add(m, groups, "ai_chip", "ai_jobs", "AI chip jobs", "{{ value_json['ai_jobs_per_second'] }}", "/s", NULL);
}

/* One round: say online (and discovery) after a connect, then one JSON per group */
static void mq_round(Mq* m, unsigned send) {
	PromSnap* p = g_new(PromSnap, 1);
	snapshot(p, send);
	cJSON* groups = prom_json(p);
	g_free(p);
	if (m->announce) {
		m->announce = 0;
		mq_publish(m, m->avail, "online", 1);
		if (m->cfg.ha) ha_publish(m, groups);
	}
	int n = 0, ok = 0;
	cJSON* g;
	cJSON_ArrayForEach(g, groups) {
		char* txt = cJSON_PrintUnformatted(g);
		gchar* topic = g_strdup_printf("%s/%s", m->prefix, g->string);
		n++;
		if (txt && mq_publish(m, topic, txt, 0)) ok++;
		free(txt);
		g_free(topic);
	}
	cJSON_Delete(groups);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (!n) set_status(&stMqtt, "MQTT", "connected", "Connected to %s:%d; nothing to send (no Send switch is on)", m->cfg.host, m->cfg.port);
	else if (ok) set_status(&stMqtt, "MQTT", "sending", "Sending to %s:%d: %d topics under %s, last at %02d:%02d:%02d", m->cfg.host, m->cfg.port, ok, m->prefix, tm.tm_hour, tm.tm_min, tm.tm_sec);
	else set_status(&stMqtt, "MQTT", "failing", "Not reachable: %s:%d took no messages", m->cfg.host, m->cfg.port);
}

/*----------------------------------------------------- InfluxDB (libcurl) */
static size_t keep_body(char* d, size_t sz, size_t n, void* u) {
	GString* s = u;
	if (s->len < 2048) g_string_append_len(s, d, (gssize)(sz * n < 2048 ? sz * n : 2048));
	return sz * n;
}
/* One request; returns the HTTP code (0: not reached, *why says why). body may be NULL for GET. */
static long ix_request(CURL* h, const InfluxCfg* x, const char* path, const char* body, GString* reply, char* why, size_t whyN) {
	gchar* base = g_strdup(x->url);
	while (*base && base[strlen(base) - 1] == '/') base[strlen(base) - 1] = 0;
	gchar* url = g_strdup_printf("%s%s", base, path);
	curl_easy_reset(h);
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, x->checkName ? 2L : 0L);
	if (x->ca[0]) curl_easy_setopt(h, CURLOPT_CAINFO, INFLUX_CA);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, keep_body);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, reply);
	struct curl_slist* hd = NULL;
	if (x->version == 2) {
		gchar* a = g_strdup_printf("Authorization: Token %s", x->token);
		hd = curl_slist_append(hd, a);
		g_free(a);
	} else if (x->user[0]) {
		curl_easy_setopt(h, CURLOPT_USERNAME, x->user);
		curl_easy_setopt(h, CURLOPT_PASSWORD, x->pass);
	}
	if (body) {
		hd = curl_slist_append(hd, "Content-Type: text/plain; charset=utf-8");
		curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
	}
	if (hd) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hd);
	CURLcode rc = curl_easy_perform(h);
	long code = 0;
	if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	else g_snprintf(why, whyN, "%s", curl_easy_strerror(rc));
	curl_slist_free_all(hd);
	g_free(url);
	g_free(base);
	return code;
}
/* An answer in plain words, or NULL when it is fine */
static gchar* ix_answer(long code, const char* why, GString* reply) {
	if (!code) return g_strdup_printf("Not reachable: %s", why);
	if (code == 401 || code == 403) return g_strdup("Not allowed: check the user and password, or the token");
	if (code == 404) return g_strdup_printf("Not found: check the address, the database or the bucket (%.120s)", reply->str);
	if (code >= 300) return g_strdup_printf("The database said %ld: %.120s", code, reply->str);
	return NULL;
}
static gchar* ix_write_path(CURL* h, const InfluxCfg* x) {
	if (x->version == 1) {
		char* db = curl_easy_escape(h, x->db, 0);
		gchar* p = g_strdup_printf("/write?db=%s&precision=s", db);
		curl_free(db);
		return p;
	}
	char* org = curl_easy_escape(h, x->org, 0);
	char* bk = curl_easy_escape(h, x->bucket, 0);
	gchar* p = g_strdup_printf("/api/v2/write?org=%s&bucket=%s&precision=s", org, bk);
	curl_free(org); curl_free(bk);
	return p;
}
static char* backlog[BACKLOG];                 /* oldest first; send-out thread only */
static int backN = 0, dropped = 0;
static void ix_push(char* batch) {
	if (backN == BACKLOG) { free(backlog[0]); memmove(backlog, backlog + 1, sizeof(char*) * (BACKLOG - 1)); backN--; dropped++; }
	backlog[backN++] = batch;
}
static void ix_send(CURL* h, const InfluxCfg* x, int lines, gboolean alertsOnly);
static void ix_round(CURL* h, const InfluxCfg* x, unsigned send) {
	PromSnap* p = g_new(PromSnap, 1);
	snapshot(p, send);
	int lines = 0;
	char* batch = prom_influx(p, &lines);
	g_free(p);
	if (batch && lines) ix_push(batch);
	else free(batch);
	if (!backN) { set_status(&stInflux, "InfluxDB", "connected", "No metrics to send (no Send switch is on)"); return; }
	ix_send(h, x, lines, FALSE);
}
/* 1.5.0: alert points waiting go out at once, as one batch through the same queue */
static void ix_alerts(CURL* h, const InfluxCfg* x) {
	g_mutex_lock(&alLock);
	GString* g = NULL;
	int n = 0;
	for (guint i = 0; ixAlert && i < ixAlert->len; i++) { if (!g) g = g_string_new(""); g_string_append(g, g_ptr_array_index(ixAlert, i)); n++; }
	if (ixAlert) g_ptr_array_set_size(ixAlert, 0);
	g_mutex_unlock(&alLock);
	if (!g) return;
	char* batch = malloc(g->len + 1);
	if (batch) { memcpy(batch, g->str, g->len + 1); ix_push(batch); }
	g_string_free(g, TRUE);
	if (batch) ix_send(h, x, n, TRUE);
}
static void ix_alerts_drop(void) {
	g_mutex_lock(&alLock);
	if (ixAlert) g_ptr_array_set_size(ixAlert, 0);
	g_mutex_unlock(&alLock);
}
static void ix_send(CURL* h, const InfluxCfg* x, int lines, gboolean alertsOnly) {
	gchar* path = ix_write_path(h, x);
	int sent = 0;
	gchar* bad = NULL;
	while (backN) {
		GString* reply = g_string_new("");
		char why[200] = "";
		long code = ix_request(h, x, path, backlog[0], reply, why, sizeof(why));
		bad = ix_answer(code, why, reply);
		g_string_free(reply, TRUE);
		if (bad) break;
		free(backlog[0]);
		memmove(backlog, backlog + 1, sizeof(char*) * (BACKLOG - 1));
		backN--;
		sent++;
	}
	g_free(path);
	g_mutex_lock(&stLock);
	influxWaiting = backN;
	g_mutex_unlock(&stLock);
	if (bad) {
		set_status(&stInflux, "InfluxDB", "failing", "%s; %d of the last %d rounds waiting%s", bad, backN, BACKLOG, dropped ? ", older ones dropped" : "");
		g_free(bad);
	} else {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		if (alertsOnly) set_status(&stInflux, "InfluxDB", "sending", "Sending: an alert change went out at %02d:%02d:%02d%s", tm.tm_hour, tm.tm_min, tm.tm_sec, sent > 1 ? " (caught up on waiting rounds)" : "");
		else set_status(&stInflux, "InfluxDB", "sending", "Sending: %d lines a round, last at %02d:%02d:%02d%s", lines, tm.tm_hour, tm.tm_min, tm.tm_sec, sent > 1 ? " (caught up on waiting rounds)" : "");
		dropped = 0;
	}
}
static void ix_clear(void) { for (int i = 0; i < backN; i++) free(backlog[i]); backN = 0; dropped = 0; influxWaiting = 0; }

/*----------------------------------------------------- the thread */
static volatile int stopping = 0;
static GMutex doneLock;                         /* statically allocated: no init needed */
static GCond doneCond;
static gboolean threadDone = FALSE, threadRuns = FALSE;
static gpointer sendout_thread(gpointer u) {
	(void)u;
	int version = -1;
	MqttCfg m; InfluxCfg x; unsigned send = 0;
	gint64 nextMq = 0, nextIx = 0, retryMq = 0;
	CURL* h = curl_easy_init();
	static MqttCfg* mcopy = NULL;
	if (!mcopy) mcopy = g_new0(MqttCfg, 1);
	while (!stopping) {
		int v = config_out(&m, &x, &send);
		if (v != version) {                          /* settings saved: start again with them */
			gboolean mqChanged = version < 0 || memcmp(mcopy, &m, sizeof(m)) != 0;
			version = v;
			if (mqChanged) {
				if (mq) { mq_end(mq, TRUE); mq = NULL; }
				*mcopy = m;
				write_private(MQTT_CA, m.ca);
				if (m.on) {
					char why[160] = "";
					set_status(&stMqtt, "MQTT", "connecting", "Connecting to %s:%d", m.host, m.port);
					mq = mq_start(&m, NULL, TRUE, why, sizeof(why));
					if (!mq) set_status(&stMqtt, "MQTT", "failing", "Not reachable: %s", why);
				} else set_status(&stMqtt, "MQTT", "off", "Off");
				nextMq = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
				retryMq = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
			}
			write_private(INFLUX_CA, x.ca);
			if (!x.on) { ix_clear(); set_status(&stInflux, "InfluxDB", "off", "Off"); }
			else if (stInflux.kind == 0) set_status(&stInflux, "InfluxDB", "connecting", "Starting: the first round goes out within %d s", x.interval);
			nextIx = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
		}
		gint64 t = g_get_monotonic_time();
		/* A first connection that failed (Paho only reconnects after a success), or one that could not start: again each minute */
		if (m.on && (!mq || (mq->failed && !mq->connected)) && t >= retryMq) {
			retryMq = t + 60 * G_USEC_PER_SEC;
			if (mq) { mq_end(mq, FALSE); mq = NULL; }
			char why[160] = "";
			mq = mq_start(&m, NULL, TRUE, why, sizeof(why));
			if (!mq) set_status(&stMqtt, "MQTT", "failing", "Not reachable: %s", why);
		}
		if (mq && t >= nextMq) {
			nextMq = t + (gint64)m.interval * G_USEC_PER_SEC;
			if (mq->connected) mq_round(mq, send);
		}
		if (mq && mq->connected) mq_alerts(mq);           /* alert changes, within a second */
		if (x.on && h && t >= nextIx) {
			nextIx = t + (gint64)x.interval * G_USEC_PER_SEC;
			ix_round(h, &x, send);
		}
		if (x.on && h) ix_alerts(h, &x);
		else ix_alerts_drop();                            /* InfluxDB is off: alert points are not kept */
		for (int i = 0; i < 10 && !stopping; i++) g_usleep(G_USEC_PER_SEC / 10);
	}
	/* Stopping (only this thread touches the connection): the alerts' last states (Off), then say offline and take the
	 * alerts and sensors back, then the InfluxDB alert points still waiting */
	if (mq) {
		if (mq->connected) mq_alerts(mq);
		mq_end(mq, TRUE);
		mq = NULL;
	}
	if (x.on && h) ix_alerts(h, &x);
	if (h) curl_easy_cleanup(h);
	g_mutex_lock(&doneLock);
	threadDone = TRUE;
	g_cond_signal(&doneCond);
	g_mutex_unlock(&doneLock);
	return NULL;
}

void sendout_init(void (*log)(const char*), const char* serial, const char* model, const char* os, const char* appVer) {
	g_mutex_init(&stLock);
	logLine = log;
	g_strlcpy(devSerial, serial ? serial : "", sizeof(devSerial));
	g_strlcpy(devModel, model ? model : "", sizeof(devModel));
	g_strlcpy(devOs, os ? os : "", sizeof(devOs));
	g_strlcpy(devVer, appVer ? appVer : "", sizeof(devVer));
	curl_global_init(CURL_GLOBAL_DEFAULT);
	threadRuns = TRUE;
	g_thread_unref(g_thread_new("send-out", sendout_thread, NULL));
}

/* The send-out thread finishes on its own (last alert states, offline, InfluxDB points waiting); this waits for it,
 * at most SENDOUT_STOP_S, so the rest of the stop always runs */
#define SENDOUT_STOP_S 6
void sendout_stop(void) {
	stopping = 1;
	if (!threadRuns) return;
	gint64 until = g_get_monotonic_time() + SENDOUT_STOP_S * G_USEC_PER_SEC;
	g_mutex_lock(&doneLock);
	while (!threadDone) if (!g_cond_wait_until(&doneCond, &doneLock, until)) break;
	gboolean done = threadDone;
	g_mutex_unlock(&doneLock);
	if (!done) LOG("Send out: still finishing after %d s; stopping without waiting longer\n", SENDOUT_STOP_S);
}

cJSON* sendout_status_json(void) {
	cJSON* o = cJSON_CreateObject();
	g_mutex_lock(&stLock);
	cJSON* a = cJSON_AddObjectToObject(o, "mqtt");
	cJSON_AddStringToObject(a, "state", stMqtt.state); cJSON_AddStringToObject(a, "text", stMqtt.text);
	cJSON* b = cJSON_AddObjectToObject(o, "influx");
	cJSON_AddStringToObject(b, "state", stInflux.state); cJSON_AddStringToObject(b, "text", stInflux.text);
	cJSON_AddNumberToObject(b, "waiting", influxWaiting);
	g_mutex_unlock(&stLock);
	return o;
}

/* Test the saved settings now (web thread): a second, short connection, so the running one is not disturbed */
gchar* sendout_test(const char* what) {
	MqttCfg* m = g_new0(MqttCfg, 1);
	InfluxCfg* x = g_new0(InfluxCfg, 1);
	unsigned send = 0;
	config_out(m, x, &send);
	gchar* out = NULL;
	if (!strcmp(what, "mqtt")) {
		if (!m->host[0]) out = g_strdup("Give the broker's host name or address, and save, first.");
		else {
			write_private(MQTT_CA, m->ca);
			char why[160] = "";
			Mq* t = mq_start(m, "-test", FALSE, why, sizeof(why));
			if (!t) out = g_strdup_printf("Not reachable: %s", why);
			else {
				for (int i = 0; i < 120 && !t->connected && !t->failed; i++) g_usleep(100000);
				if (t->connected) {
					gchar* topic = g_strdup_printf("%s/test", t->prefix);
					gchar* msg = g_strdup_printf("{\"test\":true,\"t\":%lld}", (long long)time(NULL));
					gboolean ok = mq_publish(t, topic, msg, 0);
					g_usleep(300000);
					out = ok ? g_strdup_printf("OK: connected to %s:%d%s and sent a test message to %s.", m->host, m->port, m->tls ? " with TLS" : "", topic)
						: g_strdup_printf("Connected to %s:%d, but the broker took no message.", m->host, m->port);
					g_free(topic); g_free(msg);
				} else out = g_strdup_printf("Not reachable: %s (%s:%d).", t->failed ? t->why : "no answer in 12 s", m->host, m->port);
				mq_end(t, FALSE);
			}
		}
	} else {
		CURL* h = curl_easy_init();
		if (!h) out = g_strdup("The test could not start.");
		else if (strlen(x->url) <= 8) out = g_strdup("Give the database's address, and save, first.");
		else {
			write_private(INFLUX_CA, x->ca);
			GString* reply = g_string_new("");
			char why[200] = "";
			gchar* path;
			if (x->version == 1) {
				char* db = curl_easy_escape(h, x->db, 0);
				path = g_strdup_printf("/query?db=%s&q=SHOW%%20MEASUREMENTS", db);
				curl_free(db);
			} else {
				char* org = curl_easy_escape(h, x->org, 0);
				char* bk = curl_easy_escape(h, x->bucket, 0);
				path = g_strdup_printf("/api/v2/buckets?org=%s&name=%s", org, bk);
				curl_free(org); curl_free(bk);
			}
			long code = ix_request(h, x, path, NULL, reply, why, sizeof(why));
			out = ix_answer(code, why, reply);
			if (!out) {
				if (x->version == 1 && strstr(reply->str, "\"error\"")) out = g_strdup_printf("The database answered, but: %.160s", reply->str);
				else if (x->version == 2 && !strstr(reply->str, "\"name\"")) out = g_strdup_printf("No bucket %s in %s, or the token may not read it.", x->bucket, x->org);
				else out = g_strdup_printf("OK: InfluxDB %d at %s answered, and %s %s is there.", x->version, x->url, x->version == 1 ? "database" : "bucket", x->version == 1 ? x->db : x->bucket);
			}
			g_string_free(reply, TRUE);
			g_free(path);
		}
		if (h) curl_easy_cleanup(h);
	}
	g_free(m);
	g_free(x);
	return out;
}
