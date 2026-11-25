/*
 * Resource Monitor
 *
 * A task manager for the camera: live and 24 h graphs of CPU (with its kind of work, clock and load),
 * memory (with video memory and swap), every drive, every network interface, every temperature
 * sensor, fan and heater, power and the AI chip,
 * every process added together by name, and the installed apps
 * with Start, Stop and Restart (each asked for on the page and written to the log).
 *
 * Main loop: the 1 s sampler (sampler.c) and a 5 s poll of the camera's app list.
 * Web thread: the endpoints below. Shared state is behind locks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <syslog.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib-unix.h>
#include <sys/resource.h>
#include "ACAP.h"
#include "cJSON.h"
#include "sampler.h"
#include "config.h"
#include "sendout.h"
#include "alerts.h"

#define APP_PACKAGE "resource_monitor"
#define LOG_MAX     100
#define LOG_FILE    "localdata/log.json"
#ifndef MEM_LIMIT_KB
#define MEM_LIMIT_KB 32768  /* about ten times normal use: above it, the app restarts itself */
#endif
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

static GMainLoop* main_loop = NULL;

/*-----------------------------------------------------
 * Log: app starts, stops and restarts, and who asked. The last 100 lines, kept on storage.
 *-----------------------------------------------------*/
static GMutex logLock;
static cJSON* logLines = NULL;              /* newest first */

static void log_add(const char* app, const char* what, const char* who) {
	g_mutex_lock(&logLock);
	cJSON* e = cJSON_CreateObject();
	cJSON_AddNumberToObject(e, "t", (double)time(NULL));
	cJSON_AddStringToObject(e, "app", app ? app : "");
	cJSON_AddStringToObject(e, "what", what ? what : "");
	cJSON_AddStringToObject(e, "who", who && *who ? who : "Unknown");
	cJSON_InsertItemInArray(logLines, 0, e);
	while (cJSON_GetArraySize(logLines) > LOG_MAX) cJSON_DeleteItemFromArray(logLines, LOG_MAX);
	ACAP_FILE_Write(LOG_FILE, logLines);
	g_mutex_unlock(&logLock);
}

/*-----------------------------------------------------
 * Installed apps, from the camera's list.cgi every 5 s (main loop)
 *-----------------------------------------------------*/
static GMutex appsLock;
static cJSON* apps = NULL;                  /* [{id, name, vendor, version, status}] */
static GHashTable* lastStatus = NULL;       /* id -> status, to log changes made elsewhere (main loop only) */
static gint64 ownActionUntil = 0;           /* a change we asked for is logged by the control endpoint */
static gchar ownActionApp[64] = "";       /* both under appsLock */
static gboolean poll_apps(gpointer user_data);
static gboolean poll_apps_once(gpointer user_data) { poll_apps(user_data); return G_SOURCE_REMOVE; }

static gchar* xml_attr(const char* tag, const char* end, const char* name) {
	gchar* key = g_strdup_printf(" %s=\"", name);
	const char* p = g_strstr_len(tag, end - tag, key);
	gchar* v = NULL;
	if (p) {
		p += strlen(key);
		const char* q = strchr(p, '"');
		if (q && q <= end) v = g_strndup(p, q - p);
	}
	g_free(key);
	return v ? v : g_strdup("");
}

/* Apps made by Axis (Object Analytics, Video Motion Detection, ...) come with the camera: Vendor "Axis" or "Axis Communications ..." */
static gboolean is_axis_vendor(const char* v) {
	gchar* s = g_ascii_strdown(v ? v : "", -1);
	g_strstrip(s);
	gboolean r = !strcmp(s, "axis") || g_str_has_prefix(s, "axis communications");
	g_free(s);
	return r;
}

static gboolean poll_apps(gpointer user_data) {
	(void)user_data;
	char* xml = ACAP_VAPIX_Get("applications/list.cgi");
	if (!xml) return G_SOURCE_CONTINUE;
	cJSON* list = cJSON_CreateArray();
	GPtrArray* axisIds = g_ptr_array_new_with_free_func(g_free);
	for (const char* p = strstr(xml, "<application "); p; p = strstr(p + 1, "<application ")) {
		const char* end = strchr(p, '>');
		if (!end) break;
		gchar* id = xml_attr(p, end, "Name");
		gchar* nice = xml_attr(p, end, "NiceName");
		gchar* vendor = xml_attr(p, end, "Vendor");
		gchar* version = xml_attr(p, end, "Version");
		gchar* st = xml_attr(p, end, "Status");
		cJSON* a = cJSON_CreateObject();
		cJSON_AddStringToObject(a, "id", id);
		cJSON_AddStringToObject(a, "name", *nice ? nice : id);
		cJSON_AddStringToObject(a, "vendor", vendor);
		gboolean axis = is_axis_vendor(vendor);
		cJSON_AddBoolToObject(a, "axis", axis);              /* made by Axis: part of the camera, not an app you installed */
		/* 1.6.1: Axis-made apps count as Apps too, so none is added to axisIds */
		cJSON_AddStringToObject(a, "version", version);
		cJSON_AddStringToObject(a, "status", st);
		cJSON_AddItemToArray(list, a);
		/* A change nobody asked for here (another user, the camera, a crash) goes in the log too */
		const char* was = g_hash_table_lookup(lastStatus, id);
		g_mutex_lock(&appsLock);
		gboolean ours = g_get_monotonic_time() < ownActionUntil && strcmp(ownActionApp, id) == 0;
		g_mutex_unlock(&appsLock);
		if (was && strcmp(was, st) != 0 && !ours) {
			gchar* what = g_strdup_printf("%s (not from Resource Monitor)", st);
			log_add(*nice ? nice : id, what, "Camera or another user");
			g_free(what);
		}
		g_hash_table_replace(lastStatus, g_strdup(id), g_strdup(st));
		g_free(id); g_free(nice); g_free(vendor); g_free(version); g_free(st);
	}
	free(xml);
	sampler_set_axis_apps(axisIds);                      /* 1.6.1: empty, so every installed app counts as Apps */
	g_mutex_lock(&appsLock);
	if (apps) cJSON_Delete(apps);
	apps = list;
	g_mutex_unlock(&appsLock);
	return G_SOURCE_CONTINUE;
}

static long long dir_bytes(const char* path, int depth) {
	if (depth > 12) return 0;
	GDir* d = g_dir_open(path, 0, NULL);
	if (!d) return 0;
	long long sum = 0;
	const gchar* name;
	while ((name = g_dir_read_name(d))) {
		gchar* p = g_build_filename(path, name, NULL);
		struct stat st;
		if (lstat(p, &st) == 0) sum += S_ISDIR(st.st_mode) ? dir_bytes(p, depth + 1) : (long long)st.st_blocks * 512;
		g_free(p);
	}
	g_dir_close(d);
	return sum;
}

/* Each app's storage, measured at most every 10 minutes (web thread only) */
static GHashTable* diskCache = NULL;        /* id -> kB */
static gint64 diskAt = 0;

/*-----------------------------------------------------
 * Endpoints
 *-----------------------------------------------------*/
static long param_long(const ACAP_HTTP_Request request, const char* name, long def) {
	char* v = ACAP_HTTP_Request_Param(request, name);
	long r = v && *v ? atol(v) : def;
	free(v);
	return r;
}

static void respond(ACAP_HTTP_Response response, cJSON* o) {
	ACAP_HTTP_Respond_JSON(response, o);
	cJSON_Delete(o);
}

static void HTTP_live(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	respond(response, sampler_live_json(param_long(request, "since", 0), param_long(request, "max", 180)));
}

static void HTTP_history(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	respond(response, sampler_history_json((int)param_long(request, "seconds", 3600), (int)param_long(request, "points", 120)));
}

static void HTTP_processes(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	respond(response, sampler_processes_json());
}

static void HTTP_apps(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	g_mutex_lock(&appsLock);
	cJSON* list = apps ? cJSON_Duplicate(apps, 1) : cJSON_CreateArray();
	g_mutex_unlock(&appsLock);
	cJSON* use = sampler_app_usage_json();
	gboolean measure = g_get_monotonic_time() - diskAt > 600 * G_USEC_PER_SEC || !diskAt;
	if (measure) diskAt = g_get_monotonic_time();
	cJSON* a;
	cJSON_ArrayForEach(a, list) {
		const char* id = cJSON_GetObjectItem(a, "id")->valuestring;
		cJSON* u = cJSON_GetObjectItem(use, id);
		if (u) cJSON_AddItemToObject(a, "use", cJSON_Duplicate(u, 1));
		if (measure) {
			gchar* path = g_strdup_printf("/usr/local/packages/%s", id);
			g_hash_table_replace(diskCache, g_strdup(id), GINT_TO_POINTER((gint)(dir_bytes(path, 0) / 1024)));
			g_free(path);
		}
		gpointer kb = g_hash_table_lookup(diskCache, id);
		if (kb) cJSON_AddNumberToObject(a, "diskKb", GPOINTER_TO_INT(kb));
		cJSON_AddBoolToObject(a, "self", strcmp(id, APP_PACKAGE) == 0);
	}
	cJSON_Delete(use);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddItemToObject(out, "apps", list);
	respond(response, out);
}

/* POST {"action":"stop|start|restart","package":"<id>"}. JSON only, so another site can't send it from a form. */
static void HTTP_control(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	const char* type = ACAP_HTTP_Get_Content_Type(request);
	if (!method || strcmp(method, "POST") != 0 || !type || !strstr(type, "application/json")) {
		ACAP_HTTP_Respond_Error(response, 405, "Use POST with JSON");
		return;
	}
	const char* body = ACAP_HTTP_Get_Body(request);
	cJSON* in = body ? cJSON_ParseWithLength(body, ACAP_HTTP_Get_Body_Length(request)) : NULL;
	const char* action = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "action")) : NULL;
	const char* pkg = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "package")) : NULL;
	if (!action || !pkg || (strcmp(action, "stop") && strcmp(action, "start") && strcmp(action, "restart"))) {
		ACAP_HTTP_Respond_Error(response, 400, "Expected action stop, start or restart, and a package");
		cJSON_Delete(in);
		return;
	}
	/* Only an installed app, never this one */
	gchar* nice = NULL;
	g_mutex_lock(&appsLock);
	cJSON* a;
	cJSON_ArrayForEach(a, apps) {
		if (strcmp(cJSON_GetObjectItem(a, "id")->valuestring, pkg) == 0) nice = g_strdup(cJSON_GetObjectItem(a, "name")->valuestring);
	}
	g_mutex_unlock(&appsLock);
	if (!nice || strcmp(pkg, APP_PACKAGE) == 0) {
		ACAP_HTTP_Respond_Error(response, 400, nice ? "Resource Monitor can't stop itself" : "No such app");
		g_free(nice);
		cJSON_Delete(in);
		return;
	}
	const char* who = ACAP_HTTP_Get_User(request);
	g_mutex_lock(&appsLock);
	g_strlcpy(ownActionApp, pkg, sizeof(ownActionApp));
	ownActionUntil = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	g_mutex_unlock(&appsLock);
	gchar* req = g_strdup_printf("applications/control.cgi?action=%s&package=%s", action, pkg);
	char* r = ACAP_VAPIX_Get(req);
	g_free(req);
	gboolean ok = r && g_str_has_prefix(g_strstrip(r), "OK");
	const char* past = !strcmp(action, "stop") ? "Stopped" : !strcmp(action, "start") ? "Started" : "Restarted";
	gchar* what = ok ? g_strdup_printf("%s from Resource Monitor", past)
		: g_strdup_printf("%s failed: %.200s", action, r && *r ? r : "the camera did not answer");
	log_add(nice, what, who);
	LOG("%s %s by %s: %.200s\n", action, pkg, who ? who : "?", ok ? "OK" : (r ? r : "no answer"));
	cJSON* out = cJSON_CreateObject();
	cJSON_AddBoolToObject(out, "ok", ok);
	cJSON_AddStringToObject(out, "message", what);
	g_free(what);
	free(r);
	g_free(nice);
	cJSON_Delete(in);
	g_timeout_add(1500, poll_apps_once, NULL);   /* show the new state soon */
	respond(response, out);
}

/* The camera's own power log (getPowerHistory), so the page needs no VAPIX rights of its own */
static void HTTP_power(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	respond(response, sampler_power_history_json((int)param_long(request, "seconds", 3600)));
}

/* What the camera itself reports (streams, disk health, recording space, ) */
static void HTTP_camera(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	respond(response, sampler_camera_json());
}

/* 1.3.1: endpoints are split by who may use them (manifest httpConfig): viewers read every page; Start, Stop, Restart,
 * Clear and Settings changes are admin only, each on its own endpoint, so the camera itself refuses anyone else. */
static cJSON* post_json(const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	const char* type = ACAP_HTTP_Get_Content_Type(request);
	const char* body = ACAP_HTTP_Get_Body(request);
	if (!method || strcmp(method, "POST") != 0 || !type || !strstr(type, "application/json") || !body) return NULL;
	return cJSON_ParseWithLength(body, ACAP_HTTP_Get_Body_Length(request));
}

/* The app log (viewer); clearing it is on the clear endpoint */
static void HTTP_log(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	g_mutex_lock(&logLock);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddItemToObject(out, "lines", cJSON_Duplicate(logLines, 1));
	g_mutex_unlock(&logLock);
	respond(response, out);
}

/* Settings, to read (viewer): {settings, status} */
static void HTTP_config(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	cJSON* out = config_json();
	cJSON_AddItemToObject(cJSON_GetObjectItem(out, "status"), "sendOut", sendout_status_json());
	cJSON_AddBoolToObject(out, "ok", TRUE);
	respond(response, out);
}

/* Settings, to change (admin): POST the whole settings object. Saved at once (nothing to confirm); what changed
 * goes in the app log with who changed it. */
static void HTTP_configset(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	gchar* changes = NULL;
	gchar* why = in ? config_save(in, &changes) : g_strdup("Send the settings as JSON with POST.");
	cJSON_Delete(in);
	if (why) {
		cJSON* out = cJSON_CreateObject();
		cJSON_AddBoolToObject(out, "ok", FALSE);
		cJSON_AddStringToObject(out, "error", why);
		g_free(why);
		respond(response, out);
		return;
	}
	if (changes) {
		gchar* what = g_strdup_printf("Settings changed: %s", changes);
		log_add("Resource Monitor", what, ACAP_HTTP_Get_User(request));
		g_free(what);
		g_free(changes);
	}
	cJSON* out = config_json();
	cJSON_AddItemToObject(cJSON_GetObjectItem(out, "status"), "sendOut", sendout_status_json());
	cJSON_AddBoolToObject(out, "ok", TRUE);
	respond(response, out);
}

/* Clear (admin): POST {"what":"history"|"log"}. The page asks first; each is logged with who and when. */
static void HTTP_clear(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	const char* what = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "what")) : NULL;
	const char* who = ACAP_HTTP_Get_User(request);
	const char* line = NULL;
	if (what && !strcmp(what, "history")) {
		sampler_clear_history();
		line = "History cleared (memory, built-in flash and SD card)";
	} else if (what && !strcmp(what, "log")) {
		g_mutex_lock(&logLock);
		cJSON_Delete(logLines);
		logLines = cJSON_CreateArray();
		g_mutex_unlock(&logLock);
		line = "Log cleared";
	}
	cJSON_Delete(in);
	if (!line) { ACAP_HTTP_Respond_Error(response, 400, "Expected {\"what\":\"history\"|\"log\"}"); return; }
	log_add("Resource Monitor", line, who);
	LOG("%s by %s\n", line, who && *who ? who : "?");
	cJSON* out = cJSON_CreateObject();
	cJSON_AddBoolToObject(out, "ok", TRUE);
	cJSON_AddStringToObject(out, "message", line);
	respond(response, out);
}

/* Who is looking (viewer): the user's access level from the camera's user groups, so the page can grey out what
 * only an admin may do. The camera still refuses those endpoints to anyone else. Kept 5 minutes per user. */
static GMutex whoLock;
static gchar whoUser[64] = "";
static const char* whoLevel = "unknown";
static gint64 whoAt = 0;
static gboolean in_group(const char* list, const char* group, const char* user) {
	gchar* key = g_strdup_printf("%s=\"", group);
	const char* p = strstr(list, key);
	gboolean r = FALSE;
	if (p) {
		p += strlen(key);
		const char* q = strchr(p, '"');
		gchar* names = q ? g_strndup(p, q - p) : NULL;
		gchar** v = names ? g_strsplit(names, ",", -1) : NULL;
		for (int i = 0; v && v[i]; i++) if (!strcmp(g_strstrip(v[i]), user)) r = TRUE;
		g_strfreev(v);
		g_free(names);
	}
	g_free(key);
	return r;
}
static void HTTP_whoami(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* user = ACAP_HTTP_Get_User(request);
	if (!user) user = "";
	g_mutex_lock(&whoLock);
	gboolean fresh = whoAt && !strcmp(whoUser, user) && g_get_monotonic_time() - whoAt < 300 * G_USEC_PER_SEC;
	const char* level = whoLevel;
	g_mutex_unlock(&whoLock);
	if (!fresh && *user) {
		char* x = ACAP_VAPIX_Get("pwdgrp.cgi?action=get");
		const char* lv = !x ? "unknown" : in_group(x, "admin", user) ? "admin" : in_group(x, "operator", user) ? "operator" : in_group(x, "viewer", user) ? "viewer" : "unknown";
		free(x);
		g_mutex_lock(&whoLock);
		g_strlcpy(whoUser, user, sizeof(whoUser));
		whoLevel = lv;
		whoAt = g_get_monotonic_time();
		g_mutex_unlock(&whoLock);
		level = lv;
	}
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "user", user);
	cJSON_AddStringToObject(out, "level", level);                /* admin, operator, viewer, or unknown (then the page shows every control) */
	respond(response, out);
}

/* 1.3.2: the Prometheus page (viewer): /local/resource_monitor/metrics, text format 0.0.4, only the groups whose
 * Send switch is on. Off in Settings: 404 with plain words. */
static void HTTP_metrics(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	unsigned send = 0;
	if (!config_metrics(&send)) {
		/* 200 with only a comment: the camera's web server replaces the text of error answers, and a scraper reads
		 * this as a page with no metrics, while a person sees why */
		ACAP_HTTP_Respond_String(response, "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\nCache-Control: no-cache\r\nX-Resource-Monitor: off\r\n\r\n"
			"# The Prometheus page is off. An administrator can turn it on in Resource Monitor, Settings, Send out.\n");
		return;
	}
	cJSON* dev = ACAP_Get_Config("device");
	cJSON* man = ACAP_Get_Config("manifest");
	const char* ver = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(man, "acapPackageConf"), "setup"), "version"));
	int n = 0;
	char* text = sampler_metrics(send, cJSON_GetStringValue(cJSON_GetObjectItem(dev, "model")), cJSON_GetStringValue(cJSON_GetObjectItem(dev, "firmware")), ver, &n);
	if (!text) { ACAP_HTTP_Respond_Error(response, 500, "Out of memory"); return; }
	ACAP_HTTP_Respond_String(response, "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\nCache-Control: no-cache\r\n\r\n");
	ACAP_HTTP_Respond_Data(response, strlen(text), text);
	free(text);
	char* al = alerts_prom();                            /* 1.5.0: alerts that chose Prometheus */
	if (al) { ACAP_HTTP_Respond_Data(response, strlen(al), al); g_free(al); }
}

/* 1.3.3: Test for MQTT or InfluxDB (admin): POST {"what":"mqtt"|"influx"}; tries the saved settings now */
static void HTTP_sendtest(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	const char* what = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "what")) : NULL;
	if (!what || (strcmp(what, "mqtt") && strcmp(what, "influx"))) {
		cJSON_Delete(in);
		ACAP_HTTP_Respond_Error(response, 400, "Expected {\"what\":\"mqtt\"|\"influx\"}");
		return;
	}
	gchar* msg = sendout_test(what);
	cJSON_Delete(in);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddBoolToObject(out, "ok", msg && g_str_has_prefix(msg, "OK"));
	cJSON_AddStringToObject(out, "message", msg ? msg : "The test could not run.");
	g_free(msg);
	respond(response, out);
}
/* 1.4.0 Alerts: the list with status and its log (viewer); saving (admin); approve, reject, test (operator) */
static void HTTP_alerts(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;
	cJSON* out = alerts_json();
	cJSON_AddBoolToObject(out, "ok", TRUE);
	respond(response, out);
}
static void answer(ACAP_HTTP_Response response, gchar* why) {
	cJSON* out = why ? cJSON_CreateObject() : alerts_json();
	cJSON_AddBoolToObject(out, "ok", why == NULL);
	if (why) cJSON_AddStringToObject(out, "error", why);
	g_free(why);
	respond(response, out);
}
static void HTTP_alertset(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	gchar* why = in ? alerts_save(in, ACAP_HTTP_Get_User(request)) : g_strdup("Send the alerts as JSON with POST.");
	cJSON_Delete(in);
	answer(response, why);
}
static void HTTP_alertact(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	gchar* why = in ? alerts_act(cJSON_GetStringValue(cJSON_GetObjectItem(in, "id")), cJSON_GetStringValue(cJSON_GetObjectItem(in, "action")), ACAP_HTTP_Get_User(request)) : g_strdup("Send {\"id\":...,\"action\":...} with POST.");
	cJSON_Delete(in);
	answer(response, why);
}
/* 1.6.3: the Context editor's preview (viewer: it only reads) */
static void HTTP_alertpreview(ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* in = post_json(request);
	if (!in) { cJSON* o = cJSON_CreateObject(); cJSON_AddBoolToObject(o, "ok", FALSE); cJSON_AddStringToObject(o, "error", "Send the alert as JSON with POST."); respond(response, o); return; }
	cJSON* out = alerts_preview(in);
	cJSON_Delete(in);
	respond(response, out);
}
static void log_sendout(const char* what) { log_add("Resource Monitor", what, "Resource Monitor"); }

/*-----------------------------------------------------
 * Main
 *-----------------------------------------------------*/
static gboolean tick(gpointer user_data) {
	(void)user_data;
	sampler_tick();
	alerts_tick();
	return G_SOURCE_CONTINUE;
}

/* Memory self-limit: if the app's own private memory stays above the limit for 3 checks in a row
 * (3 minutes), say so in the app log and the camera log, and exit; the camera starts it again. */
static gboolean check_memory(gpointer user_data) {
	(void)user_data;
	static int over = 0;
	long kb = -1;
	FILE* f = fopen("/proc/self/status", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) if (strncmp(line, "RssAnon:", 8) == 0) { kb = atol(line + 8); break; }
		fclose(f);
	}
	over = kb > MEM_LIMIT_KB ? over + 1 : 0;
	if (over >= 3) {
		gchar* what = g_strdup_printf("Restarted itself: its memory was %.1f MB, over its %.0f MB limit", kb / 1024.0, MEM_LIMIT_KB / 1024.0);
		log_add("Resource Monitor", what, "Resource Monitor");
		LOG("%s\n", what);
		g_free(what);
		sampler_stop();
		closelog();
		exit(EXIT_FAILURE);
	}
	return G_SOURCE_CONTINUE;
}

static gboolean signal_handler(gpointer user_data) {
	(void)user_data;
	if (main_loop && g_main_loop_is_running(main_loop)) g_main_loop_quit(main_loop);
	return G_SOURCE_REMOVE;
}

int main(void) {
	openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
	LOG("------ Starting Resource Monitor ------\n");
	g_mutex_init(&logLock);
	g_mutex_init(&appsLock);
	g_mutex_init(&whoLock);
	lastStatus = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	diskCache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* Lower priority, before any thread starts (threads take it from the one that creates them),
	 * so video and the camera's own work always come first */
	if (setpriority(PRIO_PROCESS, 0, 10) != 0) LOG("Could not lower the priority\n");
	ACAP_Init(APP_PACKAGE, NULL);
	logLines = ACAP_FILE_Read(LOG_FILE);
	if (!cJSON_IsArray(logLines)) { cJSON_Delete(logLines); logLines = cJSON_CreateArray(); }

	sampler_init();
	config_load();                                       /* the saved settings, before the first reading */
	poll_apps(NULL);                                     /* first, so the first reading already knows which apps are Axis's */
	sampler_tick();
	ACAP_HTTP_Node("live", HTTP_live);
	ACAP_HTTP_Node("history", HTTP_history);
	ACAP_HTTP_Node("processes", HTTP_processes);
	ACAP_HTTP_Node("apps", HTTP_apps);
	ACAP_HTTP_Node("control", HTTP_control);
	ACAP_HTTP_Node("log", HTTP_log);
	ACAP_HTTP_Node("power", HTTP_power);
	ACAP_HTTP_Node("camera", HTTP_camera);
	ACAP_HTTP_Node("config", HTTP_config);
	ACAP_HTTP_Node("configset", HTTP_configset);
	ACAP_HTTP_Node("clear", HTTP_clear);
	ACAP_HTTP_Node("whoami", HTTP_whoami);
	ACAP_HTTP_Node("metrics", HTTP_metrics);
	ACAP_HTTP_Node("sendtest", HTTP_sendtest);
	ACAP_HTTP_Node("alerts", HTTP_alerts);
	ACAP_HTTP_Node("alertset", HTTP_alertset);
	ACAP_HTTP_Node("alertact", HTTP_alertact);
	ACAP_HTTP_Node("alertpreview", HTTP_alertpreview);
	alerts_init(sampler_set_need);
	{
		cJSON* dev = ACAP_Get_Config("device");
		cJSON* man = ACAP_Get_Config("manifest");
		const char* ver = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(man, "acapPackageConf"), "setup"), "version"));
		sendout_init(log_sendout, ACAP_DEVICE_Prop("serial"), cJSON_GetStringValue(cJSON_GetObjectItem(dev, "model")), cJSON_GetStringValue(cJSON_GetObjectItem(dev, "firmware")), ver);
	}

	g_timeout_add(1000, tick, NULL);
	g_timeout_add_seconds(60, check_memory, NULL);
	g_timeout_add_seconds(5, poll_apps, NULL);
	main_loop = g_main_loop_new(NULL, FALSE);
	GSource* sig = g_unix_signal_source_new(SIGTERM);
	if (sig) { g_source_set_callback(sig, signal_handler, NULL, NULL); g_source_attach(sig, NULL); }
	g_main_loop_run(main_loop);
	alerts_stop();                                       /* Off for every triggered alert */
	sendout_stop();                                      /* MQTT says "offline" */
	sampler_stop();                                      /* history still waiting goes to the SD card */
	LOG("Stopping Resource Monitor\n");
	closelog();
	return 0;
}
