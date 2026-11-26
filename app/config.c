/* Settings (1.3.0): localdata/config.json, schema 1.
 * {"schema":1, "historyDays":1|7|30, "powerSource":"auto|camera|app|off", "poePorts":[n,...] (empty: all),
 *  "collect":{"cpu":{"show":true,"send":false}, ...}, "prometheus":false, "tempUnit":"C"|"F" (1.6.1, how temperatures are shown)}
 * Everything is checked before it is saved or used; a file that can't be read gives the defaults.
 * 1.3.2: Send switches feed the Prometheus page (off by default). Process names and connection addresses are
 * never sent out: their Send switches stay off.
 * 1.3.3: "mqtt":{on, host, port, tls, ca, checkName, user, password, clientId, prefix, haDiscovery, interval} and
 * "influx":{on, url, version, database, user, password, org, bucket, token, ca, checkName, interval}, both off by
 * default. password and token are taken in but never given back (only "passwordSet" / "tokenSet") or logged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include "config.h"
#include "sampler.h"

#define CONFIG_FILE "localdata/config.json"
#define SCHEMA      1
#define MAX_PORTS   16
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

/* 1.9.0 "links": links to other devices, from the Port Testing app (a file from before it has it shown, not sent) */
static const char* GROUP[] = { "cpu", "mem", "storage", "net", "temp", "power", "dlpu", "procs", "conns", "events", "links" };
static const char* GROUP_NAME[] = { "CPU", "Memory", "Storage", "Network", "Temperature", "Power", "AI chip", "Processes", "Connections", "Camera events", "Links to other devices" };
#define GROUPS 11
static const char* PS_KEY[] = { "auto", "camera", "app", "off" };
static const char* PS_NAME[] = { "automatic", "the camera's power log", "recorded by Resource Monitor", "off" };

typedef struct {
	int historyDays, powerSource, portsN, ports[MAX_PORTS];
	unsigned show, send;
	int prometheus;                                  /* the Prometheus page is on */
	int tempF;                                       /* 1.6.1: show temperatures in °F (stored and sent out in °C) */
	MqttCfg mqtt;
	InfluxCfg influx;
	int outVersion;                                  /* changes with every save, so the senders pick it up */
} Config;

static GMutex cfgMu;
static Config cur;

static void defaults(Config* c) {
	memset(c, 0, sizeof(*c));
	c->historyDays = 1;
	c->powerSource = PS_AUTO;
	c->show = G_ALL;
	c->send = 0;
	c->mqtt.port = 8883; c->mqtt.tls = 1; c->mqtt.checkName = 1; c->mqtt.interval = 30;
	c->influx.version = 2; c->influx.checkName = 1; c->influx.interval = 30;
	g_strlcpy(c->influx.url, "https://", sizeof(c->influx.url));
}

/* Small checks for the ways out */
static gboolean plain(const char* s, const char* extra) {        /* letters, digits and a few more */
	for (; *s; s++) if (!g_ascii_isalnum(*s) && !strchr(extra, *s)) return FALSE;
	return TRUE;
}
static gchar* take_str(const cJSON* o, const char* key, char* out, size_t n, const char* what) {
	const cJSON* v = cJSON_GetObjectItem(o, key);
	if (!v) return NULL;
	if (!cJSON_IsString(v)) return g_strdup_printf("%s must be text.", what);
	if (strlen(v->valuestring) >= n) return g_strdup_printf("%s is too long (at most %zu characters).", what, n - 1);
	g_strlcpy(out, v->valuestring, n);
	g_strstrip(out);
	return NULL;
}
static gchar* take_bool(const cJSON* o, const char* key, int* out, const char* what) {
	const cJSON* v = cJSON_GetObjectItem(o, key);
	if (!v) return NULL;
	if (!cJSON_IsBool(v)) return g_strdup_printf("%s must be on (true) or off (false).", what);
	*out = cJSON_IsTrue(v);
	return NULL;
}
static gchar* take_int(const cJSON* o, const char* key, int* out, int lo, int hi, const char* what) {
	const cJSON* v = cJSON_GetObjectItem(o, key);
	if (!v) return NULL;
	if (!cJSON_IsNumber(v) || v->valuedouble != (double)v->valueint || v->valueint < lo || v->valueint > hi) return g_strdup_printf("%s must be a whole number from %d to %d.", what, lo, hi);
	*out = v->valueint;
	return NULL;
}
static gchar* check_ca(const char* ca) {
	if (!*ca) return NULL;
	if (!strstr(ca, "-----BEGIN CERTIFICATE-----") || !strstr(ca, "-----END CERTIFICATE-----")) return g_strdup("Trust this CA: paste the certificate in PEM form (-----BEGIN CERTIFICATE----- ...).");
	return NULL;
}
#define TRY(x) do { gchar* e_ = (x); if (e_) return e_; } while (0)
static gchar* parse_mqtt(const cJSON* in, MqttCfg* m) {
	if (!in) return NULL;
	if (!cJSON_IsObject(in)) return g_strdup("MQTT settings must be an object.");
	int tlsWas = m->tls;
	TRY(take_bool(in, "on", &m->on, "MQTT"));
	TRY(take_str(in, "host", m->host, sizeof(m->host), "MQTT broker"));
	TRY(take_bool(in, "tls", &m->tls, "MQTT TLS"));
	if (m->tls != tlsWas && !cJSON_GetObjectItem(in, "port")) m->port = m->tls ? 8883 : 1883;
	TRY(take_int(in, "port", &m->port, 1, 65535, "MQTT port"));
	TRY(take_str(in, "ca", m->ca, sizeof(m->ca), "Trust this CA"));
	TRY(take_bool(in, "checkName", &m->checkName, "Check the server name"));
	TRY(take_str(in, "user", m->user, sizeof(m->user), "MQTT user"));
	TRY(take_str(in, "password", m->pass, sizeof(m->pass), "MQTT password"));
	TRY(take_str(in, "clientId", m->clientId, sizeof(m->clientId), "MQTT client id"));
	TRY(take_str(in, "prefix", m->prefix, sizeof(m->prefix), "MQTT topic prefix"));
	TRY(take_bool(in, "haDiscovery", &m->ha, "Home Assistant discovery"));
	TRY(take_int(in, "interval", &m->interval, 10, 300, "MQTT, every (seconds)"));
	if (*m->host && !plain(m->host, ".-_:[]")) return g_strdup("MQTT broker: a host name or address only (no scheme, path or spaces).");
	if (*m->clientId && !plain(m->clientId, "-_.")) return g_strdup("MQTT client id: letters, digits, - _ and . only.");
	if (*m->prefix && (strpbrk(m->prefix, "#+ ") || m->prefix[0] == '/' || m->prefix[strlen(m->prefix) - 1] == '/')) return g_strdup("MQTT topic prefix: no # + or spaces, and no / at either end.");
	TRY(check_ca(m->ca));
	if (m->on && !*m->host) return g_strdup("MQTT is on: give the broker's host name or address.");
	return NULL;
}
static gchar* parse_influx(const cJSON* in, InfluxCfg* x) {
	if (!in) return NULL;
	if (!cJSON_IsObject(in)) return g_strdup("InfluxDB settings must be an object.");
	TRY(take_bool(in, "on", &x->on, "InfluxDB"));
	TRY(take_str(in, "url", x->url, sizeof(x->url), "InfluxDB address"));
	TRY(take_int(in, "version", &x->version, 1, 2, "InfluxDB version"));
	TRY(take_str(in, "database", x->db, sizeof(x->db), "InfluxDB database"));
	TRY(take_str(in, "user", x->user, sizeof(x->user), "InfluxDB user"));
	TRY(take_str(in, "password", x->pass, sizeof(x->pass), "InfluxDB password"));
	TRY(take_str(in, "org", x->org, sizeof(x->org), "InfluxDB organization"));
	TRY(take_str(in, "bucket", x->bucket, sizeof(x->bucket), "InfluxDB bucket"));
	TRY(take_str(in, "token", x->token, sizeof(x->token), "InfluxDB token"));
	TRY(take_str(in, "ca", x->ca, sizeof(x->ca), "Trust this CA"));
	TRY(take_bool(in, "checkName", &x->checkName, "Check the server name"));
	TRY(take_int(in, "interval", &x->interval, 10, 300, "InfluxDB, every (seconds)"));
	gboolean urlOk = (g_str_has_prefix(x->url, "https://") || g_str_has_prefix(x->url, "http://")) && !strpbrk(x->url, " \"\\<>");
	if (!urlOk && (x->on || strcmp(x->url, "https://"))) return g_strdup("InfluxDB address: starts with https:// (or http://), no spaces.");
	if (*x->db && !plain(x->db, "-_.")) return g_strdup("InfluxDB database: letters, digits, - _ and . only.");
	TRY(check_ca(x->ca));
	if (x->on) {
		const char* host = strstr(x->url, "://") + 3;
		if (!*host) return g_strdup("InfluxDB is on: give its address, for example https://influx.local:8086.");
		if (x->version == 1 && !*x->db) return g_strdup("InfluxDB 1.x: give the database.");
		if (x->version == 2 && (!*x->org || !*x->bucket || !*x->token)) return g_strdup("InfluxDB 2.x: give the organization, the bucket and a token.");
	}
	return NULL;
}

/* Read and check; on any error says why (to free) and leaves *c as it was */
static gchar* parse(const cJSON* in, Config* out, gboolean strict) {
	if (!cJSON_IsObject(in)) return g_strdup("Settings must be a JSON object.");
	Config c = *out;
	const cJSON* v = cJSON_GetObjectItem(in, "schema");
	if (v && (!cJSON_IsNumber(v) || v->valueint != SCHEMA)) return g_strdup_printf("Unknown settings version (expected %d).", SCHEMA);
	v = cJSON_GetObjectItem(in, "historyDays");
	if (v) {
		if (!cJSON_IsNumber(v) || (v->valueint != 1 && v->valueint != 7 && v->valueint != 30)) return g_strdup("Keep history must be 24 h, 7 days or 30 days.");
		c.historyDays = v->valueint;
	}
	v = cJSON_GetObjectItem(in, "powerSource");
	if (v) {
		int k = -1;
		for (int i = 0; i < 4 && cJSON_IsString(v); i++) if (!strcmp(v->valuestring, PS_KEY[i])) k = i;
		if (k < 0) return g_strdup("Power source must be auto, camera, app or off.");
		c.powerSource = k;
	}
	v = cJSON_GetObjectItem(in, "tempUnit");
	if (v) {
		if (!cJSON_IsString(v) || (strcmp(v->valuestring, "C") && strcmp(v->valuestring, "F"))) return g_strdup("Temperature must be shown in C or F.");
		c.tempF = v->valuestring[0] == 'F';
	}
	v = cJSON_GetObjectItem(in, "poePorts");
	if (v) {
		if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > MAX_PORTS) return g_strdup_printf("PoE ports: a list of up to %d port numbers.", MAX_PORTS);
		c.portsN = 0;
		const cJSON* p;
		cJSON_ArrayForEach(p, v) {
			if (!cJSON_IsNumber(p) || p->valueint < 1 || p->valueint > 64) return g_strdup("PoE ports: each a port number from 1 to 64.");
			c.ports[c.portsN++] = p->valueint;
		}
	}
	v = cJSON_GetObjectItem(in, "collect");
	if (v) {
		if (!cJSON_IsObject(v)) return g_strdup("What to collect must be an object.");
		for (int i = 0; i < GROUPS; i++) {
			const cJSON* g = cJSON_GetObjectItem(v, GROUP[i]);
			if (!g) continue;
			const cJSON* sh = cJSON_GetObjectItem(g, "show");
			const cJSON* se = cJSON_GetObjectItem(g, "send");
			if (!cJSON_IsObject(g) || (sh && !cJSON_IsBool(sh)) || (se && !cJSON_IsBool(se))) return g_strdup_printf("What to collect, %s: show and send must be true or false.", GROUP_NAME[i]);
			if (sh) c.show = cJSON_IsTrue(sh) ? c.show | (1u << i) : c.show & ~(1u << i);
			if (se) c.send = cJSON_IsTrue(se) ? c.send | (1u << i) : c.send & ~(1u << i);
		}
	}
	c.show |= G_CPU | G_MEM;                         /* always read and shown: every other figure needs them */
	c.send &= (unsigned)G_ALL & ~(unsigned)(G_PROCS | G_CONNS);   /* process names and connection addresses: never sent */
	TRY(parse_mqtt(cJSON_GetObjectItem(in, "mqtt"), &c.mqtt));
	TRY(parse_influx(cJSON_GetObjectItem(in, "influx"), &c.influx));
	v = cJSON_GetObjectItem(in, "prometheus");
	if (v) {
		if (!cJSON_IsBool(v)) return g_strdup("Prometheus page must be on (true) or off (false).");
		c.prometheus = cJSON_IsTrue(v);
	}
	/* What this camera can do: checked only for a key that is given and changes (1.7.0), so a setting that no
	 * longer fits (an SD card taken out) never blocks saving or importing something else */
	if (strict) {
		if (cJSON_GetObjectItem(in, "historyDays") && c.historyDays != out->historyDays && c.historyDays > 1 && !sampler_card_ok()) return g_strdup("Keeping 7 or 30 days needs an SD card this app can write to.");
		if (cJSON_GetObjectItem(in, "powerSource") && c.powerSource != out->powerSource && c.powerSource == PS_CAMERA && sampler_power_log_known() == 0) return g_strdup("This camera keeps no power log of its own.");
	}
	*out = c;
	return NULL;
}

/* secrets: only for the file on the camera, never for the page */
static cJSON* to_json(const Config* c, gboolean secrets) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "schema", SCHEMA);
	cJSON_AddNumberToObject(o, "historyDays", c->historyDays);
	cJSON_AddStringToObject(o, "powerSource", PS_KEY[c->powerSource]);
	cJSON_AddStringToObject(o, "tempUnit", c->tempF ? "F" : "C");
	cJSON* p = cJSON_AddArrayToObject(o, "poePorts");
	for (int i = 0; i < c->portsN; i++) cJSON_AddItemToArray(p, cJSON_CreateNumber(c->ports[i]));
	cJSON_AddBoolToObject(o, "prometheus", c->prometheus);
	{
		const MqttCfg* m = &c->mqtt;
		cJSON* q = cJSON_AddObjectToObject(o, "mqtt");
		cJSON_AddBoolToObject(q, "on", m->on); cJSON_AddStringToObject(q, "host", m->host); cJSON_AddNumberToObject(q, "port", m->port);
		cJSON_AddBoolToObject(q, "tls", m->tls); cJSON_AddStringToObject(q, "ca", m->ca); cJSON_AddBoolToObject(q, "checkName", m->checkName);
		cJSON_AddStringToObject(q, "user", m->user);
		if (secrets) cJSON_AddStringToObject(q, "password", m->pass); else cJSON_AddBoolToObject(q, "passwordSet", m->pass[0] != 0);
		cJSON_AddStringToObject(q, "clientId", m->clientId); cJSON_AddStringToObject(q, "prefix", m->prefix);
		cJSON_AddBoolToObject(q, "haDiscovery", m->ha); cJSON_AddNumberToObject(q, "interval", m->interval);
		const InfluxCfg* x = &c->influx;
		cJSON* i = cJSON_AddObjectToObject(o, "influx");
		cJSON_AddBoolToObject(i, "on", x->on); cJSON_AddStringToObject(i, "url", x->url); cJSON_AddNumberToObject(i, "version", x->version);
		cJSON_AddStringToObject(i, "database", x->db); cJSON_AddStringToObject(i, "user", x->user);
		if (secrets) { cJSON_AddStringToObject(i, "password", x->pass); cJSON_AddStringToObject(i, "token", x->token); }
		else { cJSON_AddBoolToObject(i, "passwordSet", x->pass[0] != 0); cJSON_AddBoolToObject(i, "tokenSet", x->token[0] != 0); }
		cJSON_AddStringToObject(i, "org", x->org); cJSON_AddStringToObject(i, "bucket", x->bucket);
		cJSON_AddStringToObject(i, "ca", x->ca); cJSON_AddBoolToObject(i, "checkName", x->checkName); cJSON_AddNumberToObject(i, "interval", x->interval);
	}
	cJSON* g = cJSON_AddObjectToObject(o, "collect");
	for (int i = 0; i < GROUPS; i++) {
		cJSON* e = cJSON_AddObjectToObject(g, GROUP[i]);
		cJSON_AddBoolToObject(e, "show", (c->show >> i) & 1);
		cJSON_AddBoolToObject(e, "send", (c->send >> i) & 1);
	}
	return o;
}

static void apply(const Config* c) {
	gboolean out = c->prometheus || c->mqtt.on || c->influx.on;
	SamplerSettings s = { c->historyDays, c->powerSource, c->show | (out ? c->send : 0), c->tempF };   /* sent only while a way out is on */
	sampler_apply(&s);
}

/* Written to a new file, then put in place, so a cut in power never leaves half a file */
static gboolean write_file(const Config* c) {
	cJSON* o = to_json(c, TRUE);
	char* txt = cJSON_Print(o);
	cJSON_Delete(o);
	if (!txt) return FALSE;
	int fd = open(CONFIG_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* it holds passwords: this app only */
	FILE* f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (fd >= 0 && !f) close(fd);
	gboolean ok = f && fputs(txt, f) >= 0;
	if (f && fclose(f) != 0) ok = FALSE;
	free(txt);
	if (ok) ok = rename(CONFIG_FILE ".new", CONFIG_FILE) == 0;
	if (!ok) unlink(CONFIG_FILE ".new");
	return ok;
}

void config_load(void) {
	g_mutex_init(&cfgMu);
	defaults(&cur);
	gchar* txt = NULL;
	if (g_file_get_contents(CONFIG_FILE, &txt, NULL, NULL)) {
		cJSON* in = cJSON_Parse(txt);
		Config c = cur;
		gchar* why = parse(in, &c, FALSE);
		if (why) { LOG("Settings: %s Using the defaults.\n", why); g_free(why); }
		else cur = c;
		cJSON_Delete(in);
		g_free(txt);
	}
	apply(&cur);
}

int config_out(MqttCfg* m, InfluxCfg* i, unsigned* send) {
	g_mutex_lock(&cfgMu);
	*m = cur.mqtt; *i = cur.influx; *send = cur.send;
	int v = cur.outVersion;
	g_mutex_unlock(&cfgMu);
	return v;
}

/* The Prometheus page: on, and which groups it sends */
int config_metrics(unsigned* send) {
	g_mutex_lock(&cfgMu);
	int on = cur.prometheus;
	*send = cur.send;
	g_mutex_unlock(&cfgMu);
	return on;
}

cJSON* config_json(void) {
	g_mutex_lock(&cfgMu);
	Config c = cur;
	g_mutex_unlock(&cfgMu);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddItemToObject(out, "settings", to_json(&c, FALSE));
	cJSON* st = cJSON_AddObjectToObject(out, "status");
	cJSON_AddItemToObject(st, "history", sampler_store_json());
	int pl = sampler_power_log_known();
	cJSON_AddStringToObject(st, "powerLog", pl < 0 ? "unknown" : pl ? "yes" : "no");
	return out;
}

gchar* config_save(const cJSON* in, gchar** changes) {
	*changes = NULL;
	g_mutex_lock(&cfgMu);
	Config c = cur, was = cur;
	gchar* why = parse(in, &c, TRUE);
	if (!why && !write_file(&c)) why = g_strdup("The settings could not be saved on the camera.");
	if (!why) { c.outVersion = was.outVersion + 1; cur = c; }
	g_mutex_unlock(&cfgMu);
	if (why) return why;
	/* What changed, in plain words, for the app log */
	GString* s = g_string_new("");
	if (c.historyDays != was.historyDays) g_string_append_printf(s, "%sKeep history: %s", s->len ? "; " : "", c.historyDays == 1 ? "24 h" : c.historyDays == 7 ? "7 days" : "30 days");
	if (c.tempF != was.tempF) g_string_append_printf(s, "%sTemperatures in %s", s->len ? "; " : "", c.tempF ? "°F" : "°C");
	if (c.powerSource != was.powerSource) g_string_append_printf(s, "%sPower: %s", s->len ? "; " : "", PS_NAME[c.powerSource]);
	if (c.portsN != was.portsN || memcmp(c.ports, was.ports, sizeof(int) * (size_t)c.portsN)) g_string_append_printf(s, "%sPoE ports shown changed", s->len ? "; " : "");
	if (c.prometheus != was.prometheus) g_string_append_printf(s, "%sPrometheus page %s", s->len ? "; " : "", c.prometheus ? "on" : "off");
	/* MQTT and InfluxDB: what changed, never the password or token themselves */
	const MqttCfg *m = &c.mqtt, *mw = &was.mqtt;
	if (m->on != mw->on) g_string_append_printf(s, "%sMQTT %s", s->len ? "; " : "", m->on ? "on" : "off");
	if (strcmp(m->host, mw->host) || m->port != mw->port || m->tls != mw->tls) g_string_append_printf(s, "%sMQTT broker %s:%d%s", s->len ? "; " : "", m->host, m->port, m->tls ? " (TLS)" : " (no TLS)");
	if (strcmp(m->ca, mw->ca) || m->checkName != mw->checkName) g_string_append_printf(s, "%sMQTT certificate checks changed", s->len ? "; " : "");
	if (strcmp(m->user, mw->user)) g_string_append_printf(s, "%sMQTT user changed", s->len ? "; " : "");
	if (strcmp(m->pass, mw->pass)) g_string_append_printf(s, "%sMQTT password %s", s->len ? "; " : "", m->pass[0] ? "changed" : "cleared");
	if (strcmp(m->clientId, mw->clientId) || strcmp(m->prefix, mw->prefix)) g_string_append_printf(s, "%sMQTT client id or topic prefix changed", s->len ? "; " : "");
	if (m->ha != mw->ha) g_string_append_printf(s, "%sHome Assistant discovery %s", s->len ? "; " : "", m->ha ? "on" : "off");
	if (m->interval != mw->interval) g_string_append_printf(s, "%sMQTT every %d s", s->len ? "; " : "", m->interval);
	const InfluxCfg *x = &c.influx, *xw = &was.influx;
	if (x->on != xw->on) g_string_append_printf(s, "%sInfluxDB %s", s->len ? "; " : "", x->on ? "on" : "off");
	if (strcmp(x->url, xw->url) || x->version != xw->version || strcmp(x->db, xw->db) || strcmp(x->org, xw->org) || strcmp(x->bucket, xw->bucket)) g_string_append_printf(s, "%sInfluxDB target changed", s->len ? "; " : "");
	if (strcmp(x->ca, xw->ca) || x->checkName != xw->checkName) g_string_append_printf(s, "%sInfluxDB certificate checks changed", s->len ? "; " : "");
	if (strcmp(x->user, xw->user)) g_string_append_printf(s, "%sInfluxDB user changed", s->len ? "; " : "");
	if (strcmp(x->pass, xw->pass)) g_string_append_printf(s, "%sInfluxDB password %s", s->len ? "; " : "", x->pass[0] ? "changed" : "cleared");
	if (strcmp(x->token, xw->token)) g_string_append_printf(s, "%sInfluxDB token %s", s->len ? "; " : "", x->token[0] ? "changed" : "cleared");
	if (x->interval != xw->interval) g_string_append_printf(s, "%sInfluxDB every %d s", s->len ? "; " : "", x->interval);
	for (int i = 0; i < GROUPS; i++) if (((c.send ^ was.send) >> i) & 1) g_string_append_printf(s, "%s%s %s", s->len ? "; " : "", GROUP_NAME[i], (c.send >> i) & 1 ? "sent" : "not sent");
	for (int i = 0; i < GROUPS; i++) if (((c.show ^ was.show) >> i) & 1) g_string_append_printf(s, "%s%s %s", s->len ? "; " : "", GROUP_NAME[i], (c.show >> i) & 1 ? "shown" : "hidden and not read");
	if (s->len) *changes = g_string_free(s, FALSE);
	else g_string_free(s, TRUE);
	apply(&c);
	return NULL;
}

/* 1.7.0 Copy settings to other cameras (cfgsections.c): the settings in two parts, passwords and token included.
 * CFG_PART_MAIN: historyDays, powerSource, poePorts, collect, prometheus, tempUnit.
 * CFG_PART_OUT:  mqtt and influx, without their CA certificates (each camera keeps its own). */
static const char* const PART_MAIN[] = { "historyDays", "powerSource", "poePorts", "collect", "prometheus", "tempUnit", NULL };
static const char* const PART_OUT[] = { "mqtt", "influx", NULL };

/* Moves the part's keys from the full settings `from` into `to` (replacing what is there) */
static void take_part(cJSON* to, cJSON* from, int part) {
	const char* const* keys = part == CFG_PART_OUT ? PART_OUT : PART_MAIN;
	for (int k = 0; keys[k]; k++) {
		cJSON* v = cJSON_DetachItemFromObjectCaseSensitive(from, keys[k]);
		if (!v) continue;
		if (part == CFG_PART_OUT) cJSON_DeleteItemFromObjectCaseSensitive(v, "ca");
		cJSON_DeleteItemFromObjectCaseSensitive(to, keys[k]);
		cJSON_AddItemToObject(to, keys[k], v);
	}
}

cJSON* config_part(int part) {
	g_mutex_lock(&cfgMu);
	Config c = cur;
	g_mutex_unlock(&cfgMu);
	cJSON* all = to_json(&c, TRUE);
	cJSON* out = cJSON_CreateObject();
	take_part(out, all, part);
	cJSON_Delete(all);
	return out;
}

gchar* config_part_check(cJSON* value, int part) {
	g_mutex_lock(&cfgMu);
	Config c = cur;
	g_mutex_unlock(&cfgMu);
	gchar* why = parse(value, &c, TRUE);                 /* the same check as config_save */
	if (why) return why;
	cJSON* all = to_json(&c, TRUE);
	take_part(value, all, part);
	cJSON_Delete(all);
	return NULL;
}
