/* 1.9.0 Links to other devices, from the Port Testing app. See links.h.
 *
 * Port Testing's metrics endpoint (/local/port_testing/metrics on this camera, read with the camera's own
 * service account, as every other camera service here):
 *   {"links":[{"camera","address","state","rttMs","rttP95Ms","lostPct","refused","playing","game"}],
 *    "matches":[{"start","end" (null while playing),"game","camera"}]}
 * A field that is missing has no value yet. Times are unix seconds; the matches cover the last 24 h.
 *
 * Threads: the reading thread fills the state below; the main loop (live readings, alerts) and the web thread
 * (the page, the Prometheus page) read it. All of it is behind `mu`. This file never takes the sampler's lock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <syslog.h>
#include <glib.h>
/* Not ACAP.h: it pulls in fcgi_stdio.h. The reading thread uses the background connection, as sampler.c's slow thread does. */
char* ACAP_VAPIX_Get_Background(const char* request);
#include "cJSON.h"
#include "links.h"
#include "sampler.h"
#include "prom.h"

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

#define READ_EVERY   5          /* seconds between reads */
#define FRESH_S      15         /* a reading older than this is not shown as now */
#define FORGET_S     86400      /* a device not seen for this long gives its place to a new one */
#define LH_STEP      30         /* seconds per history point */
#define LH_N         2880       /* 24 h of history points */
#define NONE         0xFFFF     /* no value in a history point */
#define ENDPOINT     "../local/port_testing/metrics"   /* from /axis-cgi/, so /local/port_testing/metrics */

typedef struct {
	char name[64], address[64], state[32], game[16];
	double rtt, p95, loss, refused;              /* NAN: not given */
	int playing;                                 /* -1 not given */
	gint64 seen;                                 /* unix s it was last in a reading; 0 = the place is free */
	int current;                                 /* in the latest reading */
} Dev;

typedef struct { gint64 start, end; char game[16], dev[64]; } Match;   /* end 0: still playing */
typedef struct { guint32 b; guint16 r[LINK_MAX], l[LINK_MAX]; } LPoint;  /* b: unix s / LH_STEP; r: 0.1 ms; l: 0.01 % */

static GMutex mu;
static GCond wake;
static GThread* thread = NULL;
static volatile gint stopping = 0;
static int app = LA_MISSING;
static Dev dev[LINK_MAX];
static Match matches[LINK_MATCHES];
static int matchN = 0;
static gint64 readAt = 0;                       /* monotonic µs of the last good reading; 0 none */
static LPoint* hist = NULL;                     /* LH_N points, set aside the first time Port Testing answers */
static int failN = 0;

/*----------------------------------------------------- helpers (mu held) */
static guint16 enc(double v, double scale, double most) {
	if (isnan(v) || v < 0) return NONE;
	double x = round(v * scale);
	return (guint16)(x > most ? most : x);
}
static float dec(guint16 v, double scale) { return v == NONE ? NAN : (float)(v / scale); }

static gboolean fresh(void) { return app == LA_RUNNING && readAt && g_get_monotonic_time() - readAt <= (gint64)FRESH_S * G_USEC_PER_SEC; }

static void forget_column(int k) {
	if (!hist) return;
	for (int i = 0; i < LH_N; i++) { hist[i].r[k] = NONE; hist[i].l[k] = NONE; }
}

/* The place a device keeps: its own, a free one, or one not seen for 24 h (its history is cleared). -1 when full. */
static int place_of(const char* name, gint64 now) {
	for (int k = 0; k < LINK_MAX; k++) if (dev[k].seen && !strcmp(dev[k].name, name)) return k;
	int k = -1;
	for (int i = 0; i < LINK_MAX && k < 0; i++) if (!dev[i].seen) k = i;
	for (int i = 0; i < LINK_MAX && k < 0; i++) if (now - dev[i].seen > FORGET_S) k = i;
	if (k < 0) return -1;
	memset(&dev[k], 0, sizeof(dev[k]));
	forget_column(k);
	g_strlcpy(dev[k].name, name, sizeof(dev[k].name));
	return k;
}

static void hist_add(gint64 now) {
	if (!hist) {
		hist = g_new(LPoint, LH_N);
		for (int i = 0; i < LH_N; i++) { hist[i].b = 0; memset(hist[i].r, 0xFF, sizeof(hist[i].r)); memset(hist[i].l, 0xFF, sizeof(hist[i].l)); }
	}
	guint32 b = (guint32)(now / LH_STEP);
	LPoint* p = &hist[b % LH_N];
	if (p->b != b) { p->b = b; memset(p->r, 0xFF, sizeof(p->r)); memset(p->l, 0xFF, sizeof(p->l)); }
	for (int k = 0; k < LINK_MAX; k++) {
		if (!dev[k].current) continue;
		guint16 r = enc(dev[k].rtt, 10, 65534), l = enc(dev[k].loss, 100, 10000);
		if (r != NONE && (p->r[k] == NONE || r > p->r[k])) p->r[k] = r;      /* each point keeps its peak */
		if (l != NONE && (p->l[k] == NONE || l > p->l[k])) p->l[k] = l;
	}
}

static double jnum(const cJSON* o, const char* key) {
	const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}
static void jstr(const cJSON* o, const char* key, char* out, size_t n) {
	const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
	g_strlcpy(out, v ? v : "", n);
	g_strstrip(out);
	for (char* q = out; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
}

/*----------------------------------------------------- reading */
static void take(const cJSON* in) {                 /* mu held */
	gint64 now = (gint64)time(NULL);
	for (int k = 0; k < LINK_MAX; k++) dev[k].current = 0;
	const cJSON* it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(in, "links")) {
		char name[64];
		jstr(it, "camera", name, sizeof(name));
		if (!*name) jstr(it, "address", name, sizeof(name));
		if (!*name) continue;
		int k = place_of(name, now);
		if (k < 0 || dev[k].current) continue;       /* full, or the same name twice in one reading */
		Dev* d = &dev[k];
		jstr(it, "address", d->address, sizeof(d->address));
		jstr(it, "state", d->state, sizeof(d->state));
		jstr(it, "game", d->game, sizeof(d->game));
		d->rtt = jnum(it, "rttMs"); d->p95 = jnum(it, "rttP95Ms"); d->loss = jnum(it, "lostPct"); d->refused = jnum(it, "refused");
		const cJSON* pl = cJSON_GetObjectItemCaseSensitive(it, "playing");
		d->playing = cJSON_IsBool(pl) ? cJSON_IsTrue(pl) : -1;
		d->seen = now;
		d->current = 1;
	}
	/* The matches of the last 24 h, newest first, as many as fit */
	matchN = 0;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(in, "matches")) {
		double s = jnum(it, "start"), e = jnum(it, "end");
		if (isnan(s) || s <= 0) continue;
		if (!isnan(e) && e < (double)(now - 86400)) continue;
		Match m;
		memset(&m, 0, sizeof(m));
		m.start = (gint64)s;
		m.end = isnan(e) ? 0 : (gint64)e;
		jstr(it, "game", m.game, sizeof(m.game));
		jstr(it, "camera", m.dev, sizeof(m.dev));
		int at = matchN;
		while (at > 0 && matches[at - 1].start < m.start) at--;
		if (at >= LINK_MATCHES) continue;
		int n = matchN < LINK_MATCHES ? matchN : LINK_MATCHES - 1;
		memmove(&matches[at + 1], &matches[at], sizeof(Match) * (size_t)(n - at));
		matches[at] = m;
		if (matchN < LINK_MATCHES) matchN++;
	}
	readAt = g_get_monotonic_time();
	hist_add(now);
}

static void read_once(void) {
	char* x = ACAP_VAPIX_Get_Background(ENDPOINT);
	cJSON* in = x ? cJSON_Parse(x) : NULL;
	free(x);
	g_mutex_lock(&mu);
	if (cJSON_IsObject(in) && app == LA_RUNNING) {
		if (failN >= 3) LOG("Port Testing answers again\n");
		failN = 0;
		take(in);
	} else if (++failN == 3) {
		LOG("Port Testing is running but its metrics could not be read\n");
	}
	g_mutex_unlock(&mu);
	cJSON_Delete(in);
}

static gpointer read_thread(gpointer user_data) {
	(void)user_data;
	while (!g_atomic_int_get(&stopping)) {
		g_mutex_lock(&mu);
		gint64 until = g_get_monotonic_time() + (gint64)READ_EVERY * G_USEC_PER_SEC;
		while (!g_atomic_int_get(&stopping) && g_cond_wait_until(&wake, &mu, until)) { /* woken early: wait out the rest */ }
		int running = app == LA_RUNNING;
		if (!running) for (int k = 0; k < LINK_MAX; k++) dev[k].current = 0;
		g_mutex_unlock(&mu);
		if (g_atomic_int_get(&stopping)) break;
		if (running && (sampler_collect() & G_LINKS)) read_once();    /* not wanted: nothing is read */
	}
	return NULL;
}

/*----------------------------------------------------- API */
void links_init(void) {
	g_mutex_init(&mu);
	g_cond_init(&wake);
	for (int k = 0; k < LINK_MAX; k++) { dev[k].rtt = dev[k].p95 = dev[k].loss = dev[k].refused = NAN; dev[k].playing = -1; }
	thread = g_thread_new("links", read_thread, NULL);
}

void links_stop(void) {
	if (!thread) return;
	g_atomic_int_set(&stopping, 1);
	g_mutex_lock(&mu);
	g_cond_signal(&wake);
	g_mutex_unlock(&mu);
	g_thread_join(thread);                          /* at most one read's time limit */
	thread = NULL;
}

void links_set_app(int state) {
	g_mutex_lock(&mu);
	if (state != app) {
		LOG("Port Testing: %s\n", state == LA_RUNNING ? "running, its links are read" : state == LA_STOPPED ? "not running" : "not installed");
		if (state != LA_RUNNING) { readAt = 0; failN = 0; for (int k = 0; k < LINK_MAX; k++) dev[k].current = 0; }
	}
	app = state;
	g_mutex_unlock(&mu);
}

int links_app(void) {
	g_mutex_lock(&mu);
	int a = app;
	g_mutex_unlock(&mu);
	return a;
}

int links_now(float* rtt, float* loss) {
	int n = 0;
	g_mutex_lock(&mu);
	gboolean ok = fresh();
	for (int k = 0; k < LINK_MAX; k++) {
		rtt[k] = loss[k] = NAN;
		if (!ok || !dev[k].current) continue;
		rtt[k] = (float)dev[k].rtt;
		loss[k] = (float)dev[k].loss;
		n = k + 1;
	}
	g_mutex_unlock(&mu);
	return n;
}

int links_hist(gint64 t0, gint64 t1, float* rtt, float* loss) {
	int any = 0;
	for (int k = 0; k < LINK_MAX; k++) rtt[k] = loss[k] = NAN;
	if (t1 <= t0) return 0;
	g_mutex_lock(&mu);
	if (hist) {
		guint32 b0 = (guint32)((t0 + 1) / LH_STEP), b1 = (guint32)(t1 / LH_STEP);
		if (b1 - b0 >= LH_N) b0 = b1 - LH_N + 1;
		for (guint32 b = b0; b <= b1; b++) {
			const LPoint* p = &hist[b % LH_N];
			if (p->b != b) continue;
			for (int k = 0; k < LINK_MAX; k++) {
				float r = dec(p->r[k], 10), l = dec(p->l[k], 100);
				if (!isnan(r) && (isnan(rtt[k]) || r > rtt[k])) { rtt[k] = r; any = 1; }
				if (!isnan(l) && (isnan(loss[k]) || l > loss[k])) { loss[k] = l; any = 1; }
			}
		}
	}
	g_mutex_unlock(&mu);
	return any;
}

static void add_num(cJSON* o, const char* k, double v) {
	if (isnan(v) || isinf(v)) cJSON_AddNullToObject(o, k);
	else cJSON_AddNumberToObject(o, k, round(v * 100.0) / 100.0);
}

cJSON* links_json(void) {
	cJSON* o = cJSON_CreateObject();
	g_mutex_lock(&mu);
	cJSON_AddStringToObject(o, "app", app == LA_RUNNING ? "running" : app == LA_STOPPED ? "stopped" : "missing");
	gboolean ok = fresh();
	cJSON_AddBoolToObject(o, "ok", ok);
	if (app == LA_RUNNING && (ok || hist)) {
		int last = 0;
		for (int k = 0; k < LINK_MAX; k++) if (dev[k].seen) last = k + 1;
		cJSON* a = cJSON_AddArrayToObject(o, "devs");
		for (int k = 0; k < last; k++) {
			const Dev* d = &dev[k];
			if (!d->seen) { cJSON_AddItemToArray(a, cJSON_CreateNull()); continue; }
			cJSON* e = cJSON_CreateObject();
			gboolean now = ok && d->current;
			cJSON_AddStringToObject(e, "name", d->name);
			cJSON_AddStringToObject(e, "address", d->address);
			cJSON_AddBoolToObject(e, "current", now);
			if (now) {
				cJSON_AddStringToObject(e, "state", d->state);
				add_num(e, "rtt", d->rtt); add_num(e, "p95", d->p95); add_num(e, "loss", d->loss); add_num(e, "refused", d->refused);
				if (d->playing >= 0) cJSON_AddBoolToObject(e, "playing", d->playing);
				if (d->game[0]) cJSON_AddStringToObject(e, "game", d->game);
			}
			cJSON_AddItemToArray(a, e);
		}
		cJSON* m = cJSON_AddArrayToObject(o, "matches");
		for (int i = 0; i < matchN; i++) {
			cJSON* e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "start", (double)matches[i].start);
			if (matches[i].end) cJSON_AddNumberToObject(e, "end", (double)matches[i].end); else cJSON_AddNullToObject(e, "end");
			cJSON_AddStringToObject(e, "game", matches[i].game);
			cJSON_AddStringToObject(e, "camera", matches[i].dev);
			cJSON_AddItemToArray(m, e);
		}
	}
	g_mutex_unlock(&mu);
	return o;
}

int links_alert(int which, const char* device, int above, double* v, char* devOut, size_t n) {
	*v = NAN;
	if (devOut && n) devOut[0] = 0;
	g_mutex_lock(&mu);
	gboolean ok = fresh();
	for (int k = 0; ok && k < LINK_MAX; k++) {
		const Dev* d = &dev[k];
		if (!d->current || (device && *device && strcmp(d->name, device))) continue;
		double x = which ? d->loss : d->rtt;
		if (device && *device) { *v = x; if (devOut) g_strlcpy(devOut, d->name, n); break; }
		if (isnan(x)) continue;
		if (isnan(*v) || (above ? x > *v : x < *v)) { *v = x; if (devOut) g_strlcpy(devOut, d->name, n); }
	}
	g_mutex_unlock(&mu);
	if (ok && device && *device && devOut && !*devOut) g_strlcpy(devOut, device, n);
	return ok ? 1 : 0;
}

cJSON* links_device_names(void) {
	cJSON* a = cJSON_CreateArray();
	g_mutex_lock(&mu);
	if (fresh()) for (int k = 0; k < LINK_MAX; k++) if (dev[k].current) cJSON_AddItemToArray(a, cJSON_CreateString(dev[k].name));
	g_mutex_unlock(&mu);
	return a;
}

void links_snap(void* ps) {
	PromSnap* p = ps;
	p->linkN = 0;
	g_mutex_lock(&mu);
	if (fresh()) for (int k = 0; k < LINK_MAX && p->linkN < PROM_LINKS; k++) {
		if (!dev[k].current) continue;
		g_strlcpy(p->linkName[p->linkN], dev[k].name, sizeof(p->linkName[0]));
		p->linkRtt[p->linkN] = dev[k].rtt;
		p->linkP95[p->linkN] = dev[k].p95;
		p->linkLoss[p->linkN] = dev[k].loss;
		p->linkN++;
	}
	g_mutex_unlock(&mu);
}
