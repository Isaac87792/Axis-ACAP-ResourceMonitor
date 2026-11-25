/* The metrics sent out; see prom.h. One walk over the snapshot gives every metric with its name, labels and group;
 * the Prometheus text, the MQTT JSON and the InfluxDB lines are each written from that same walk. */
#include "prom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/*----------------------------------------------------- the walk */
typedef struct Walk Walk;
struct Walk {
	void (*fam)(Walk* w, const char* name, const char* type, const char* help);
	void (*smp)(Walk* w, const char* name, const char* l1, const char* v1, const char* l2, const char* v2, double v);
	const char* group;                              /* "cpu", "memory", ... while walking that group */
	void* u;
};
static void S(Walk* w, const char* name, const char* l1, const char* v1, const char* l2, const char* v2, double v) {
	if (isnan(v) || isinf(v)) return;               /* not known: left out */
	w->smp(w, name, l1, v1, l2, v2, v);
}
static void walk(const PromSnap* p, Walk* w) {
	char num[16];
	if (p->send & PG_CPU) {
		w->group = "cpu";
		w->fam(w, "axis_rm_cpu_percent", "gauge", "CPU in use, all cores, percent.");
		S(w, "axis_rm_cpu_percent", NULL, NULL, NULL, NULL, p->cpu);
		w->fam(w, "axis_rm_cpu_core_percent", "gauge", "CPU in use on each core, percent.");
		for (int i = 0; i < p->coreN && i < PROM_CORES; i++) { snprintf(num, sizeof(num), "%d", i + 1); S(w, "axis_rm_cpu_core_percent", "core", num, NULL, NULL, p->cores[i]); }
		w->fam(w, "axis_rm_cpu_work_percent", "gauge", "CPU by kind of work, percent of all cores.");
		static const char* K[4] = { "user", "system", "iowait", "irq" };
		for (int i = 0; i < 4; i++) S(w, "axis_rm_cpu_work_percent", "kind", K[i], NULL, NULL, p->work[i]);
		w->fam(w, "axis_rm_load", "gauge", "Load average: processes running or waiting.");
		static const char* L[3] = { "1m", "5m", "15m" };
		for (int i = 0; i < 3; i++) S(w, "axis_rm_load", "period", L[i], NULL, NULL, p->load[i]);
		w->fam(w, "axis_rm_nonaxis_apps_cpu_percent", "gauge", "CPU used by installed apps, percent of all cores.");
		S(w, "axis_rm_nonaxis_apps_cpu_percent", NULL, NULL, NULL, NULL, p->apps);
	}
	if (p->send & PG_MEM) {
		w->group = "memory";
		w->fam(w, "axis_rm_memory_bytes", "gauge", "Memory by kind, bytes.");
		S(w, "axis_rm_memory_bytes", "kind", "used", NULL, NULL, p->memUsed);
		S(w, "axis_rm_memory_bytes", "kind", "available", NULL, NULL, p->memAvail);
		S(w, "axis_rm_memory_bytes", "kind", "cache", NULL, NULL, p->memCache);
		S(w, "axis_rm_memory_bytes", "kind", "video", NULL, NULL, p->memVideo);
		w->fam(w, "axis_rm_memory_total_bytes", "gauge", "All memory, bytes.");
		S(w, "axis_rm_memory_total_bytes", NULL, NULL, NULL, NULL, p->memTotal);
		w->fam(w, "axis_rm_swap_bytes", "gauge", "Swap in use, bytes.");
		S(w, "axis_rm_swap_bytes", NULL, NULL, NULL, NULL, p->swapUsed);
		w->fam(w, "axis_rm_nonaxis_apps_memory_bytes", "gauge", "Memory used by installed apps, bytes.");
		S(w, "axis_rm_nonaxis_apps_memory_bytes", NULL, NULL, NULL, NULL, p->appsMem);
	}
	if (p->send & PG_STORAGE) {
		w->group = "storage";
		w->fam(w, "axis_rm_drive_used_bytes", "gauge", "Space used on each drive, bytes.");
		for (int i = 0; i < p->driveN && i < PROM_DRIVES; i++) S(w, "axis_rm_drive_used_bytes", "drive", p->driveName[i], NULL, NULL, p->driveUsed[i]);
		w->fam(w, "axis_rm_drive_total_bytes", "gauge", "Size of each drive, bytes.");
		for (int i = 0; i < p->driveN && i < PROM_DRIVES; i++) S(w, "axis_rm_drive_total_bytes", "drive", p->driveName[i], NULL, NULL, p->driveTotal[i]);
		w->fam(w, "axis_rm_disk_bytes_per_second", "gauge", "Reads and writes on each storage device, bytes a second.");
		for (int i = 0; i < p->devN && i < PROM_DEVS; i++) {
			S(w, "axis_rm_disk_bytes_per_second", "device", p->devName[i], "direction", "read", p->devRead[i]);
			S(w, "axis_rm_disk_bytes_per_second", "device", p->devName[i], "direction", "write", p->devWrite[i]);
		}
	}
	if (p->send & PG_NET) {
		w->group = "network";
		w->fam(w, "axis_rm_net_bits_per_second", "gauge", "Traffic on each network interface, bits a second.");
		for (int i = 0; i < p->ifN && i < PROM_IFS; i++) {
			S(w, "axis_rm_net_bits_per_second", "interface", p->ifName[i], "direction", "receive", p->ifRx[i]);
			S(w, "axis_rm_net_bits_per_second", "interface", p->ifName[i], "direction", "transmit", p->ifTx[i]);
		}
		w->fam(w, "axis_rm_net_errors_total", "counter", "Network errors on each interface since the camera started.");
		for (int i = 0; i < p->ifN && i < PROM_IFS; i++) {
			S(w, "axis_rm_net_errors_total", "interface", p->ifName[i], "direction", "receive", (double)p->ifTot[i][0]);
			S(w, "axis_rm_net_errors_total", "interface", p->ifName[i], "direction", "transmit", (double)p->ifTot[i][2]);
		}
		w->fam(w, "axis_rm_net_drops_total", "counter", "Network packets dropped on each interface since the camera started.");
		for (int i = 0; i < p->ifN && i < PROM_IFS; i++) {
			S(w, "axis_rm_net_drops_total", "interface", p->ifName[i], "direction", "receive", (double)p->ifTot[i][1]);
			S(w, "axis_rm_net_drops_total", "interface", p->ifName[i], "direction", "transmit", (double)p->ifTot[i][3]);
		}
	}
	if (p->send & PG_TEMP) {
		w->group = "temperature";
		w->fam(w, "axis_rm_temperature_celsius", "gauge", "Each temperature sensor, degrees Celsius.");
		for (int i = 0; i < p->sensN && i < PROM_SENS; i++) S(w, "axis_rm_temperature_celsius", "sensor", p->sensName[i], NULL, NULL, p->sensC[i]);
		w->fam(w, "axis_rm_fan_rpm", "gauge", "Fan speed, turns a minute.");
		S(w, "axis_rm_fan_rpm", NULL, NULL, NULL, NULL, p->fanRpm);
	}
	if (p->send & PG_POWER) {
		w->group = "power";
		w->fam(w, "axis_rm_power_watts", "gauge", "Power the camera draws, or its PoE ports give out, watts.");
		S(w, "axis_rm_power_watts", NULL, NULL, NULL, NULL, p->powerW);
	}
	if ((p->send & PG_DLPU) && p->hasAi) {
		w->group = "ai_chip";
		w->fam(w, "axis_rm_ai_jobs_per_second", "gauge", "AI chip jobs a second.");
		S(w, "axis_rm_ai_jobs_per_second", NULL, NULL, NULL, NULL, p->aiJobs);
	}
	w->group = NULL;
}

/*----------------------------------------------------- a growing text (with a fixed most) */
typedef struct { char* b; size_t n, cap; int count, failed; } Buf;
#define BUF_MAX (1 << 20)                               /* never more than 1 MB */
static void put(Buf* o, const char* fmt, ...) {
	if (o->failed) return;
	for (;;) {
		va_list a;
		va_start(a, fmt);
		int w = vsnprintf(o->b + o->n, o->cap - o->n, fmt, a);
		va_end(a);
		if (w < 0) { o->failed = 1; return; }
		if ((size_t)w < o->cap - o->n) { o->n += (size_t)w; return; }
		size_t cap = o->cap * 2 + (size_t)w;
		char* nb = cap <= BUF_MAX ? realloc(o->b, cap) : NULL;
		if (!nb) { o->failed = 1; return; }
		o->b = nb; o->cap = cap;
	}
}
static int buf_init(Buf* o, size_t cap) { memset(o, 0, sizeof(*o)); o->b = malloc(cap); o->cap = cap; if (o->b) o->b[0] = 0; else o->failed = 1; return o->b != NULL; }
static void num_put(Buf* o, double v) { if (v == floor(v) && fabs(v) < 1e15) put(o, "%.0f", v); else put(o, "%.6g", v); }

/*----------------------------------------------------- the Prometheus text */
static void put_label(Buf* o, const char* name, const char* v) {
	put(o, "%s=\"", name);
	for (; v && *v; v++) {
		if (*v == '\\') put(o, "\\\\");
		else if (*v == '"') put(o, "\\\"");
		else if (*v == '\n') put(o, "\\n");
		else put(o, "%c", *v);
	}
	put(o, "\"");
}
static void t_fam(Walk* w, const char* name, const char* type, const char* help) {
	put(w->u, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}
static void t_smp(Walk* w, const char* name, const char* l1, const char* v1, const char* l2, const char* v2, double v) {
	Buf* o = w->u;
	put(o, "%s", name);
	if (l1) {
		put(o, "{");
		put_label(o, l1, v1);
		if (l2) { put(o, ","); put_label(o, l2, v2); }
		put(o, "}");
	}
	put(o, " ");
	num_put(o, v);
	put(o, "\n");
	o->count++;
}
char* prom_render(const PromSnap* p, int* count) {
	Buf o;
	if (!buf_init(&o, 8192)) return NULL;
	put(&o, "# HELP axis_rm_up 1 while Resource Monitor is running and reading.\n# TYPE axis_rm_up gauge\naxis_rm_up 1\n");
	put(&o, "# HELP axis_rm_info The camera and app this page comes from (always 1).\n# TYPE axis_rm_info gauge\naxis_rm_info{");
	put_label(&o, "model", p->model); put(&o, ",");
	put_label(&o, "axis_os", p->os); put(&o, ",");
	put_label(&o, "app_version", p->appVer);
	put(&o, "} 1\n");
	o.count = 2;
	Walk w = { t_fam, t_smp, NULL, &o };
	walk(p, &w);
	if (o.failed) { free(o.b); return NULL; }
	if (count) *count = o.count;
	return o.b;
}

/*----------------------------------------------------- JSON for MQTT, one object per group */
static void no_fam(Walk* w, const char* name, const char* type, const char* help) { (void)w; (void)name; (void)type; (void)help; }
static cJSON* obj(cJSON* parent, const char* key) {
	cJSON* o = cJSON_GetObjectItemCaseSensitive(parent, key);
	if (!o) o = cJSON_AddObjectToObject(parent, key);
	return o;
}
static void j_smp(Walk* w, const char* name, const char* l1, const char* v1, const char* l2, const char* v2, double v) {
	(void)l1; (void)l2;
	cJSON* g = obj(w->u, w->group);
	const char* k = strncmp(name, "axis_rm_", 8) ? name : name + 8;
	if (!v1) { cJSON_AddNumberToObject(g, k, v); return; }
	cJSON* m = obj(g, k);
	if (v2) m = obj(m, v1);
	cJSON_AddNumberToObject(m, v2 ? v2 : v1, v);
}
cJSON* prom_json(const PromSnap* p) {
	cJSON* out = cJSON_CreateObject();
	Walk w = { no_fam, j_smp, NULL, out };
	walk(p, &w);
	cJSON* g;
	cJSON_ArrayForEach(g, out) cJSON_AddNumberToObject(g, "t", (double)p->t);
	return out;
}

/*----------------------------------------------------- InfluxDB line protocol */
#define IL_MAX 96
typedef struct { char tags[160]; Buf fields; } ILine;
typedef struct { ILine l[IL_MAX]; int n; } ICtx;
static void esc_tag(Buf* o, const char* s) {               /* tag keys and values, field keys: , = space and \ escaped */
	for (; s && *s; s++) { if (*s == '\n') continue; if (*s == ',' || *s == '=' || *s == ' ' || *s == '\\') put(o, "\\"); put(o, "%c", *s); }
}
static void i_smp(Walk* w, const char* name, const char* l1, const char* v1, const char* l2, const char* v2, double v) {
	ICtx* c = w->u;
	Buf t;
	if (!buf_init(&t, 160)) return;
	if (l1) { put(&t, ",%s=", l1); esc_tag(&t, v1); }
	if (l2) { put(&t, ",%s=", l2); esc_tag(&t, v2); }
	int k = -1;
	if (!t.failed) for (int i = 0; i < c->n; i++) if (!strcmp(c->l[i].tags, t.b)) k = i;
	if (k < 0 && c->n < IL_MAX && !t.failed && t.n < sizeof(c->l[0].tags) && buf_init(&c->l[c->n].fields, 256)) {
		k = c->n++;
		memcpy(c->l[k].tags, t.b, t.n + 1);
	}
	free(t.b);
	if (k < 0) return;
	Buf* f = &c->l[k].fields;
	if (f->n) put(f, ",");
	esc_tag(f, strncmp(name, "axis_rm_", 8) ? name : name + 8);
	put(f, "=");
	num_put(f, v);
	if (v == floor(v) && fabs(v) < 1e15) put(f, ".0");    /* always a float, so a field never changes type */
}
char* prom_influx(const PromSnap* p, int* lines) {
	ICtx* c = calloc(1, sizeof(ICtx));
	if (!c) return NULL;
	Walk w = { no_fam, i_smp, NULL, c };
	walk(p, &w);
	Buf o;
	char* out = NULL;
	if (buf_init(&o, 8192)) {
		for (int i = 0; i < c->n; i++) {
			if (!c->l[i].fields.n || c->l[i].fields.failed) continue;
			put(&o, "axis_resource_monitor,model=");
			esc_tag(&o, p->model[0] ? p->model : "unknown");
			put(&o, ",serial=");
			esc_tag(&o, p->serial[0] ? p->serial : "unknown");
			put(&o, "%s %s %lld\n", c->l[i].tags, c->l[i].fields.b, p->t);
			o.count++;
		}
		if (o.failed) free(o.b); else out = o.b;
		if (lines) *lines = o.count;
	}
	for (int i = 0; i < c->n; i++) free(c->l[i].fields.b);
	free(c);
	return out;
}
