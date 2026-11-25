/*
 * sampler.c: what the camera is using, read from /proc and /sys once a second.
 *
 * - Live: the last 180 one-second readings (CPU, its kind of work and clock, each core, memory,
 *   video memory, swap, every drive, every network interface, every temperature sensor, power,
 *   the AI chip, and this app's own use).
 * - Processes: every 2 s all processes are read and added together by name.
 * - History: 24 h of 10 s points kept in memory (about 2.7 MB), each with the peak of its
 *   readings and the top 10 processes by CPU (from the busiest scan in it) and by memory, plus
 *   the 1.1 readings as 16-bit floats (about 1.3 MB, set aside once at start).
 *   With an SD card this app can write (1.3.0), history is also kept on the card (store.c): 10 s points
 *   for 24 h, 1 min for 7 days, 5 min for 30 days, as Settings chooses; it survives restarts.
 *   1.5.0: without a card, the 24 h of 10 s points are kept in a ring file in the app's own storage
 *   (localdata, the built-in flash, about 2.2 MB, written once a minute), so they survive restarts too.
 *   If that file can't be written, history is memory only and the reason is shown in Settings.
 * - Slow sources (the camera's temperature, power and PoE services, the SD card list and network
 *   drives) are asked every 30 s on their own thread, so a slow answer never holds up the 1 s
 *   reading. A source that has never answered is asked once an hour after two tries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
/* Not ACAP.h: it pulls in fcgi_stdio.h, whose FILE would break fscanf here */
char* ACAP_VAPIX_Get_Background(const char* request);     /* the slow thread has its own connection */
char* ACAP_VAPIX_Post_Background(const char* request, const char* body);
#include "cJSON.h"
#include "sampler.h"
#include "netconn.h"
#include "store.h"
#include "prom.h"
#include <errno.h>
#include <syslog.h>

#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }

#define LIVE_N      180     /* one-second readings kept for the live graphs */
#define SCAN_EVERY  2       /* seconds between process scans, when they are quick */
#define SCAN_MAX    10      /* the longest it stretches to when scans run long */
#define HIST_STEP   10      /* seconds per history point */
#define HIST_N      8640    /* 24 h of history points */
#define TOPN        10
#define IOTOP       5       /* names waiting on the disk kept per history point */
#define MAX_CORES   16
#define SD_PATH     "/var/spool/storage/SD_DISK"

/* Caps: every list below has a fixed size, set aside once */
#define MAX_SENS    8       /* temperature sensors */
#define MAX_IF      4       /* network interfaces */
#define NET_F       8       /* per interface: rx Mbit/s, tx Mbit/s, rx packets/s, tx packets/s, rx errors, rx drops, tx errors, tx drops */
#define MAX_MNT     8       /* mounted drives */
#define MAX_DSK     4       /* whole block devices, for read/write activity */
#define DSK_HF      3       /* per device in history: read kB/s, write kB/s, busy % (IOPS is live only) */
#define MAX_PORTS   16      /* PoE ports on a recorder */
#define SLOW_EVERY  30      /* seconds between asks of the slow sources */

/* The 1.1 readings: one float array per live reading, one 16-bit float array per history point */
enum { X_USER, X_SYS, X_WAIT, X_IRQ, X_STEAL, X_MHZ, X_L1, X_L5, X_L15, X_CACHE, X_VIDEO, X_SWAP, X_FAN, X_POWER, X_INFER,
	X_HEAT,                                                   /* 1.5.0: heater 1 on, 0 off (peak: on at any time in a point) */
	X_SELF_CPU, X_SELF_MEM,                                   /* 1.6.1: this app's own CPU % and memory in MB, for the This app line (peak per point) */
	X_TEMP0, X_NET0 = X_TEMP0 + MAX_SENS, X_MNT0 = X_NET0 + MAX_IF * NET_F, X_DSK0 = X_MNT0 + MAX_MNT, X_N = X_DSK0 + MAX_DSK * DSK_HF };
enum { AGG_MAX, AGG_SUM, AGG_LAST };

typedef struct { guint16 name, owner; float val; } TopEntry;     /* owner 0 = the camera itself */

typedef struct {
	gint64 t;                                   /* unix seconds at the end of the point */
	float cpu, apps, mem, flash, sd, tx, rx, temp;
	TopEntry cpuTop[TOPN], memTop[TOPN];
	float cpuOther, memOther;
	guint8 cpuN, memN;
	float bestCpu;                              /* while building: CPU of the scan whose top 10 is kept */
	TopEntry netTop[TOPN];                      /* busiest connections (service · other end), Mbit/s */
	float netOther, bestNet;
	guint8 netN;
	TopEntry ioTop[IOTOP];                      /* names waiting on the disk (state D): seconds blocked in the point */
	guint8 ioN;
} HistPoint;

typedef struct {
	long seq;
	gint64 tms;
	float cpu, apps, mem, flash, sd, tx, rx, temp;
	float cores[MAX_CORES];
	float coresApps[MAX_CORES];                 /* apps you installed, per core (from the last process scan) */
	float meCpu, meMemKb;
	float x[X_N];                               /* the 1.1 readings (X_...) */
	float iops[MAX_DSK];                        /* reads and writes a second, per device (live only) */
	float coreWork[MAX_CORES][4];               /* each core: user, system, waiting on disk, interrupts (%) */
} Sample;

typedef struct {
	char name[48];
	char app[64];
	int count, threads, dCount;                 /* dCount: processes of this name waiting on the disk (state D) */
	double cpu, memKb, ageS;
} ProcSum;

typedef struct {
	unsigned long long start;                   /* start time, to notice a reused pid */
	guint64 prevTicks;
	gboolean seen;
	char name[48];
	char app[64];
} PidInfo;

/* What the page needs to know about each slot of the lists. A slot keeps its place (and its
 * history column) for the app's life; a name that comes after the list is full is left out. */
typedef struct { char name[16]; char state[24]; char oper[24]; float linkMbps, dropsHour, errorsHour; gboolean present; unsigned long long tot[4]; } IfPub;   /* tot: rx errors, rx drops, tx errors, tx drops since start of the camera */
typedef struct { char name[40]; char path[160]; char kind[8]; int disk; gboolean present; double totalKb, freeKb; } MntPub;
typedef struct { char dev[16]; char name[40]; int life, eol; gboolean present; } DskPub;     /* life: 1..11 from the card, 0 unknown */
typedef struct { char name[32]; float high24; } SensPub;
typedef struct { int port; float w, limitW; char cls[8]; char state[24]; } PortPub;

typedef struct {
	double memTotalKb, memAvailKb, memFreeKb, memCacheKb;
	double flashTotalKb, flashFreeKb, sdTotalKb, sdFreeKb;
	double linkMbps, load1, load5, load15, uptime, appsMemKb, appsCpu;
	unsigned long long rxErr, txErr, rxDrop, txDrop;
	int cores, procs, threads, names;
	char netIf[16];
	float coresApps[MAX_CORES];
	/* 1.1 */
	double swapTotalKb, swapUsedKb, videoTotalKb, videoUsedKb;    /* video: -1 when the camera doesn't say */
	float mhz;
	int sensN, ifN, mntN, dskN;
	gboolean sensFromCamera;                    /* no readable thermal zone: the camera's temperature service */
	SensPub sens[MAX_SENS];
	IfPub ifs[MAX_IF];
	MntPub mnts[MAX_MNT];
	DskPub dsks[MAX_DSK];
	gboolean fanOk, fanRunning, heaterOk, heaterOn;
	gint64 heatSince, heatLastOff, heatLastFor;  /* 1.5.0: unix s it went on (0 off or not known), it last went off, how long that was on */
	gboolean powerOk, poeOk, powerHist;   /* powerHist: the camera keeps its own power log */                    /* the camera's own draw; a recorder's PoE ports */
	float powerW, budgetW, powerAvgW, powerMaxW, powerReqW;
	char powerCls[8];
	int portsN;
	PortPub ports[MAX_PORTS];
	char dlpu[16];                              /* "" when there is no AI chip line in /proc/interrupts */
} Now;

static GMutex lock;
static Sample live[LIVE_N];
static long liveSeq = 0;
static HistPoint* hist = NULL;
static int histHead = 0, histCount = 0;
static HistPoint cur;
static int curTicks = 0;
/* The top 10 with each live reading that had a process scan (every 2 s), for the Live graphs' hover */
typedef struct {
	long seq;
	TopEntry cpu[TOPN], mem[TOPN], net[TOPN];
	float cpuOther, memOther, netOther;
	guint8 cpuN, memN, netN, hasNet, ioN;
	TopEntry io[IOTOP];                         /* names waiting on the disk (state D) in this scan, seconds */
} LiveTop;
static LiveTop liveTop[LIVE_N];
static GArray* scan = NULL;                     /* ProcSum, latest scan, by CPU */
static GArray* net = NULL;                      /* NetUse, latest connection scan, busiest first */
static gboolean netOk = FALSE;                  /* the camera allows reading connections */
static int scanProcs = 0;
static Now now;
static GPtrArray* names = NULL;                 /* index -> name; index 0 is "" */
static GHashTable* nameIdx = NULL;

/* Only the main loop touches these */
static GHashTable* pids = NULL;
static GHashTable* tids = NULL;                 /* thread id -> CPU ticks at the previous scan (app threads only) */
static unsigned long long scanCoreTotal[MAX_CORES];
static unsigned long long prevTotal = 0, prevIdle = 0, prevScanTotal = 0, prevSelf = 0, prevSelfTotal = 0;
static unsigned long long prevCoreTotal[MAX_CORES], prevCoreIdle[MAX_CORES];
static unsigned long long prevAgg[8];          /* /proc/stat "cpu" line: user nice system idle iowait irq softirq steal */
static char thermal[MAX_SENS][64];
static int thermalN = 0;
static char fanPath[64] = "";                   /* hwmon fan speed, when the kernel has one */
static int mhzN = 0;                            /* cores with a readable clock */
static int tickN = 0;

/* 1.1 history: X_N 16-bit floats per point, beside `hist`; the point being built in curX (lock held) */
static guint16* histX = NULL;
static float curX[X_N];
/* Each core's kind of work and apps you installed in history: CW_F 16-bit floats per core, for the first
 * coreHN cores (8 at most), set aside once at start (about 345 kB for 4 cores) */
#define CW_F    5
#define CW_MAX  8
static guint16* histC = NULL;
static int coreHN = 0;
static float curC[CW_MAX * CW_F];

/* Slow sources, filled every 30 s by their own thread (slowLock) */
typedef struct {
	gboolean tempOk; int vsN; char vsName[MAX_SENS][32]; float vsC[MAX_SENS];
	gboolean fanOk, fanRunning, heaterOk, heaterOn; float fanRpm;
	gboolean powerOk; float powerW, powerAvgW, powerMaxW, powerReqW; char powerCls[8]; gboolean powerHist, powerCapKnown;
	gboolean poeOk; float budgetW; int portsN; PortPub ports[MAX_PORTS];
	double sdTotalKb, sdFreeKb;                 /* from the camera's disk list; -1 when it has no card */
} Slow;
static GMutex slowLock;
static GMutex phLock;                           /* the cache of the camera's power history */
static Slow slow;
static char slowNetPath[MAX_MNT][160];           /* network drives to measure (slot index; "" = none) */
static gboolean slowWantSd = FALSE;             /* the SD card's folder isn't visible to the app */
static int scanEvery = SCAN_EVERY, lastScanTick = -100;
static gint64 lastPruneUs = 0;
static gint64 startMono = 0, startUnix = 0;          /* 1.5.1: when this run started (monotonic µs, unix s) */
static gboolean histSettled = FALSE;                 /* 1.5.1: where history is kept is decided, and it was read back */
/* 1.3.0: the settings that change what is read (cfgLock; read once per reading and per slow round) */
static GMutex cfgLock;
static GMutex storeLock;                        /* the history rings on the SD card (store block below) */
static SamplerSettings cfg = { 1, PS_AUTO, G_ALL, 0 };
static gboolean cfgDirty = TRUE;
static SamplerSettings cfg_get(void) { g_mutex_lock(&cfgLock); SamplerSettings c = cfg; g_mutex_unlock(&cfgLock); return c; }
static long pageKb = 4;
static long hz = 100;

/*----------------------------------------------------- helpers */

static void add_num(cJSON* o, const char* k, double v) {
	if (isnan(v) || isinf(v)) cJSON_AddNullToObject(o, k);
	else cJSON_AddNumberToObject(o, k, round(v * 100.0) / 100.0);
}

static long read_kb(const char* file, const char* key) {
	FILE* f = fopen(file, "r");
	if (!f) return -1;
	char line[256];
	long v = -1;
	size_t n = strlen(key);
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, key, n) == 0) { v = atol(line + n); break; }
	fclose(f);
	return v;
}

static guint16 name_id(const char* s) {                  /* called with the lock held */
	if (!s || !*s) return 0;
	gpointer v;
	if (g_hash_table_lookup_extended(nameIdx, s, NULL, &v)) return (guint16)GPOINTER_TO_UINT(v);
	if (names->len >= 65000) return 0;
	gchar* copy = g_strdup(s);
	g_ptr_array_add(names, copy);
	g_hash_table_insert(nameIdx, copy, GUINT_TO_POINTER(names->len - 1));
	return (guint16)(names->len - 1);
}

/* The name table only grows as new process names and connection labels appear (a new client
 * address is a new label). Once a day, or past 4000 names, keep only the names the history and
 * the live readings still use, and renumber them. Lock held. */
static void prune_names(void);
static const char* name_of(guint16 id) { return id < names->len ? g_ptr_array_index(names, id) : ""; }

static float fmaxn(float a, float b) { return isnan(a) ? b : isnan(b) ? a : (a > b ? a : b); }

static long read_long(const char* path, long def) {
	FILE* f = fopen(path, "r");
	if (!f) return def;
	char line[64];
	long v = def;
	if (fgets(line, sizeof(line), f)) { char* e; long r = strtol(line, &e, 10); if (e != line) v = r; }
	fclose(f);
	return v;
}
static gboolean read_line(const char* path, char* out, size_t n) {
	FILE* f = fopen(path, "r");
	if (!f) return FALSE;
	gboolean ok = fgets(out, (int)n, f) != NULL;
	fclose(f);
	if (ok) g_strchomp(out);
	return ok;
}

/*----------------------------------------------------- 16-bit floats for the 1.1 history */

static guint16 f2h(float f) {
	if (isnan(f)) return 0x7E00;
	union { float f; guint32 u; } v = { f };
	guint32 x = v.u, sign = (x >> 16) & 0x8000, m = x & 0x7fffff;
	int e = (int)((x >> 23) & 0xff) - 127 + 15;
	if (e >= 31) return (guint16)(sign | 0x7BFF);                 /* too big: the largest there is */
	if (e <= 0) {                                                  /* very small: subnormal or zero */
		if (e < -10) return (guint16)sign;
		m |= 0x800000;
		int sh = 14 - e;
		guint32 hm = m >> sh;
		if ((m >> (sh - 1)) & 1) hm++;
		return (guint16)(sign | hm);
	}
	guint32 hm = m >> 13;
	if (m & 0x1000) { hm++; if (hm == 0x400) { hm = 0; if (++e >= 31) return (guint16)(sign | 0x7BFF); } }
	return (guint16)(sign | ((guint32)e << 10) | hm);
}
static float h2f(guint16 h) {
	guint32 sign = (guint32)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
	if (e == 31) return m ? NAN : (sign ? -INFINITY : INFINITY);
	if (e == 0) { float r = ldexpf((float)m, -24); return sign ? -r : r; }
	union { guint32 u; float f; } v = { sign | ((e - 15 + 127) << 23) | (m << 13) };
	return v.f;
}
/* How a reading is kept when readings are put together: peaks, counts added up, or the latest */
static int x_agg(int i) {
	if (i >= X_NET0 && i < X_MNT0 && (i - X_NET0) % NET_F >= 4) return AGG_SUM;
	if (i >= X_MNT0 && i < X_DSK0) return AGG_LAST;
	return AGG_MAX;
}
/* Kept in history divided by this, to stay inside a 16-bit float: packets/s in thousands, kB/s as MB/s */
static float x_scale(int i) {
	if (i >= X_NET0 && i < X_MNT0) { int k = (i - X_NET0) % NET_F; return k == 2 || k == 3 ? 1000.0f : 1.0f; }
	if (i >= X_DSK0) { int k = (i - X_DSK0) % DSK_HF; return k < 2 ? 1000.0f : 1.0f; }
	return 1.0f;
}
static void x_fold(float* acc, const float* v) {
	for (int i = 0; i < X_N; i++) {
		if (isnan(v[i])) continue;
		switch (x_agg(i)) {
			case AGG_SUM: acc[i] = isnan(acc[i]) ? v[i] : acc[i] + v[i]; break;
			case AGG_LAST: acc[i] = v[i]; break;
			default: acc[i] = fmaxn(acc[i], v[i]);
		}
	}
}
static void x_clear(float* x) { for (int i = 0; i < X_N; i++) x[i] = NAN; }
static void hist_x(int slot, float* out) {             /* lock held */
	const guint16* h = &histX[(size_t)slot * X_N];
	for (int i = 0; i < X_N; i++) out[i] = h2f(h[i]) * x_scale(i);
}

/*----------------------------------------------------- CPU */

/* Each core's kind of work (user+nice, system+softirq, waiting on disk, interrupts), from its own cpuN line */
static unsigned long long prevCoreAgg[MAX_CORES][8];
static void core_work(int core, const unsigned long long* v, unsigned long long t, float* out) {
	if (prevCoreTotal[core] && t > prevCoreTotal[core]) {
		double dt = (double)(t - prevCoreTotal[core]), d[8];
		for (int i = 0; i < 8; i++) d[i] = v[i] > prevCoreAgg[core][i] ? (double)(v[i] - prevCoreAgg[core][i]) : 0;
		out[0] = (float)(100.0 * (d[0] + d[1]) / dt);
		out[1] = (float)(100.0 * (d[2] + d[6]) / dt);
		out[2] = (float)(100.0 * d[4] / dt);
		out[3] = (float)(100.0 * d[5] / dt);
	}
	memcpy(prevCoreAgg[core], v, sizeof(prevCoreAgg[core]));
}

static int read_cpu(unsigned long long* total, unsigned long long* idle, float* cores, unsigned long long* agg, float (*work)[4]) {
	FILE* f = fopen("/proc/stat", "r");
	if (!f) return 0;
	char line[512];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "cpu", 3) != 0) break;   /* the cpu lines come first */
		unsigned long long v[8] = { 0 }, t = 0;
		int core = -1;
		char* p = line + 3;
		if (isdigit((unsigned char)*p)) core = atoi(p);
		while (*p && *p != ' ') p++;
		if (sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4) continue;
		for (int i = 0; i < 8; i++) t += v[i];
		unsigned long long id = v[3] + v[4];
		if (core < 0) { *total = t; *idle = id; memcpy(agg, v, sizeof(v)); }
		else if (core < MAX_CORES) {
			core_work(core, v, t, work[core]);              /* before prevCoreTotal moves on */
			if (prevCoreTotal[core] && t > prevCoreTotal[core])
				cores[core] = 100.0f * (float)((t - prevCoreTotal[core]) - (id - prevCoreIdle[core])) / (float)(t - prevCoreTotal[core]);
			prevCoreTotal[core] = t; prevCoreIdle[core] = id;
			if (core + 1 > n) n = core + 1;
		}
	}
	fclose(f);
	return n;
}

static unsigned long long self_ticks(void) {
	FILE* f = fopen("/proc/self/stat", "r");
	if (!f) return 0;
	char buf[1024];
	unsigned long ut = 0, st = 0;
	if (fgets(buf, sizeof(buf), f)) {
		char* r = strrchr(buf, ')');
		if (r) sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
	}
	fclose(f);
	return (unsigned long long)ut + st;
}

/*----------------------------------------------------- CPU clock */

static void find_mhz(void) {
	for (int i = 0; i < MAX_CORES; i++) {
		char p[80];
		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
		if (read_long(p, -1) > 0) mhzN = i + 1;
	}
	LOG("CPU clock: %s\n", mhzN ? "readable" : "not readable");
}
static float read_mhz(void) {
	double sum = 0;
	int n = 0;
	for (int i = 0; i < mhzN; i++) {
		char p[80];
		snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
		long k = read_long(p, -1);
		if (k > 0) { sum += k / 1000.0; n++; }
	}
	return n ? (float)(sum / n) : NAN;
}

/*----------------------------------------------------- memory, in one pass of /proc/meminfo */

static void read_meminfo(Now* n) {
	FILE* f = fopen("/proc/meminfo", "r");
	if (!f) return;
	double cached = -1, buffers = -1, swapT = -1, swapF = -1, cmaxT = -1, cmaxA = -1;
	n->memTotalKb = n->memAvailKb = n->memFreeKb = -1;
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		char key[40];
		double v;
		if (sscanf(line, "%39[^:]: %lf", key, &v) != 2) continue;
		if (!strcmp(key, "MemTotal")) n->memTotalKb = v;
		else if (!strcmp(key, "MemAvailable")) n->memAvailKb = v;
		else if (!strcmp(key, "MemFree")) n->memFreeKb = v;
		else if (!strcmp(key, "Cached")) cached = v;
		else if (!strcmp(key, "Buffers")) buffers = v;
		else if (!strcmp(key, "SwapTotal")) swapT = v;
		else if (!strcmp(key, "SwapFree")) swapF = v;
		else if (!strcmp(key, "CmaxTotal")) cmaxT = v;          /* ARTPEC: memory set aside for video */
		else if (!strcmp(key, "CmaxAllocated")) cmaxA = v;
	}
	fclose(f);
	n->memCacheKb = cached + buffers;
	n->swapTotalKb = swapT > 0 ? swapT : 0;
	n->swapUsedKb = swapT > 0 && swapF >= 0 ? swapT - swapF : 0;
	n->videoTotalKb = cmaxT > 0 ? cmaxT : -1;
	n->videoUsedKb = cmaxT > 0 && cmaxA >= 0 ? cmaxA : -1;
}

/*----------------------------------------------------- temperature, fan and heater */

/* Plain words for a thermal zone's type (ARTPEC-8: top-thermal, CPU, Graphics, dlp-thermal, cdc-thermal, vip0-thermal) */
static void zone_name(const char* type, char* out, size_t n) {
	gchar* t = g_ascii_strdown(type, -1);
	const char* nice = g_str_has_prefix(t, "top") ? "Chip" : strstr(t, "dlp") ? "AI chip" : strstr(t, "cdc") ? "Video encoder" :
		strstr(t, "vip") ? "Image processor" : strstr(t, "cpu") ? "CPU" : strstr(t, "gpu") || strstr(t, "graphics") ? "Graphics" :
		strstr(t, "soc") ? "Chip" : strstr(t, "ddr") || strstr(t, "mem") ? "Memory" : strstr(t, "sensor") || strstr(t, "img") || strstr(t, "cmos") ? "Image sensor" :
		strstr(t, "board") || strstr(t, "pcb") ? "Main board" : NULL;
	if (nice) g_strlcpy(out, nice, n);
	else {
		/* Anything else: without "-thermal", first letter capital */
		gchar* s = g_strdup(type);
		size_t l = strlen(s);
		if (l > 8 && (g_str_has_suffix(s, "-thermal") || g_str_has_suffix(s, "_thermal"))) s[l - 8] = 0;
		for (char* c = s; *c; c++) if (*c == '_' || *c == '-') *c = ' ';
		if (s[0]) s[0] = g_ascii_toupper(s[0]);
		g_strlcpy(out, s[0] ? s : type, n);
		g_free(s);
	}
	g_free(t);
}
static void sens_unique(Now* n, int idx) {
	char base[32];
	g_strlcpy(base, n->sens[idx].name, sizeof(base));
	int same = 0;
	for (int i = 0; i < idx; i++) if (strncmp(n->sens[i].name, base, strlen(base)) == 0) same++;
	if (same) snprintf(n->sens[idx].name, sizeof(n->sens[idx].name), "%.18s %d", base, same + 1);
}

static void find_thermal(Now* n) {
	for (int i = 0; i < 32 && thermalN < MAX_SENS; i++) {
		char p[64], tp[64], type[48] = "";
		snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
		long v = read_long(p, -1000000);
		if (v <= -40000 || v >= 150000) continue;
		snprintf(tp, sizeof(tp), "/sys/class/thermal/thermal_zone%d/type", i);
		if (!read_line(tp, type, sizeof(type)) || !type[0]) snprintf(type, sizeof(type), "Sensor %d", thermalN + 1);
		g_strlcpy(thermal[thermalN], p, sizeof(thermal[0]));
		zone_name(type, n->sens[thermalN].name, sizeof(n->sens[0].name));
		n->sens[thermalN].high24 = NAN;
		sens_unique(n, thermalN);
		thermalN++;
	}
	n->sensN = thermalN;
	n->sensFromCamera = thermalN == 0;
	for (int i = 0; i < 8 && !fanPath[0]; i++)
		for (int k = 1; k <= 2 && !fanPath[0]; k++) {
			char p[64];
			snprintf(p, sizeof(p), "/sys/class/hwmon/hwmon%d/fan%d_input", i, k);
			if (read_long(p, -1) >= 0) g_strlcpy(fanPath, p, sizeof(fanPath));
		}
	LOG("Temperature: %d thermal zone(s) readable%s\n", thermalN, fanPath[0] ? ", and a fan" : "");
}

/* Every sensor into x; returns the hottest. No readable thermal zone: the camera's named sensors (every 30 s). */
static float read_temps(Now* n, float* x, const Slow* sl) {
	float best = NAN;
	if (thermalN) {
		for (int i = 0; i < thermalN; i++) {
			long v = read_long(thermal[i], -1000000);
			float c = v > -40000 && v < 150000 ? (float)v / 1000.0f : NAN;
			x[X_TEMP0 + i] = c;
			best = fmaxn(best, c);
		}
	} else if (sl->tempOk) {
		for (int i = 0; i < sl->vsN; i++) {
			int k = -1;
			for (int j = 0; j < n->sensN; j++) if (!strcmp(n->sens[j].name, sl->vsName[i])) k = j;
			if (k < 0 && n->sensN < MAX_SENS) {
				k = n->sensN++;
				g_strlcpy(n->sens[k].name, sl->vsName[i], sizeof(n->sens[k].name));
				n->sens[k].high24 = NAN;
			}
			if (k < 0) continue;
			x[X_TEMP0 + k] = sl->vsC[i];
			best = fmaxn(best, sl->vsC[i]);
		}
	}
	/* Fan: the kernel's speed if it has one, else the camera's temperature service; heater: the service */
	if (fanPath[0]) {
		long rpm = read_long(fanPath, -1);
		n->fanOk = TRUE;
		n->fanRunning = rpm > 0;
		x[X_FAN] = rpm >= 0 ? (float)rpm : NAN;
	} else {
		n->fanOk = sl->fanOk;
		n->fanRunning = sl->fanRunning;
		x[X_FAN] = sl->fanOk ? sl->fanRpm : NAN;
	}
	n->heaterOk = sl->heaterOk;
	n->heaterOn = sl->heaterOn;
	x[X_HEAT] = sl->heaterOk ? (sl->heaterOn ? 1.0f : 0.0f) : NAN;
	return best;
}

/*----------------------------------------------------- Ethernet, every interface */

typedef struct { char name[16]; unsigned long long c[8]; gint64 at; gboolean primed; } IfPriv;   /* c: as NET_F, as running totals */
static IfPriv ifp[MAX_IF];

static gboolean if_skip(const char* name) {
	static const char* skip[] = { "lo", "sit", "ip6tnl", "ip6gre", "tunl", "veth", "dummy", "ip_vti", "ip6_vti", "gre", "erspan", NULL };
	for (int i = 0; skip[i]; i++) if (g_str_has_prefix(name, skip[i])) return TRUE;
	return FALSE;
}
static gboolean is_eth(const char* name) { return g_str_has_prefix(name, "eth") || g_str_has_prefix(name, "en"); }

static void read_net(float* tx, float* rx, Now* n, float* x) {
	FILE* f = fopen("/proc/net/dev", "r");
	if (!f) return;
	typedef struct { char name[16]; unsigned long long c[8]; } Row;
	Row rows[16];
	int nr = 0;
	char line[512];
	while (nr < 16 && fgets(line, sizeof(line), f)) {
		char* colon = strchr(line, ':');
		if (!colon) continue;
		*colon = 0;
		char* name = line;
		while (*name == ' ') name++;
		if (if_skip(name)) continue;
		unsigned long long* c = rows[nr].c;
		/* rx: bytes packets errs drop fifo frame compressed multicast; tx: bytes packets errs drop ... */
		if (sscanf(colon + 1, "%llu %llu %llu %llu %*u %*u %*u %*u %llu %llu %llu %llu", &c[0], &c[2], &c[4], &c[5], &c[1], &c[3], &c[6], &c[7]) != 8) continue;
		g_strlcpy(rows[nr].name, name, sizeof(rows[nr].name));
		nr++;
	}
	fclose(f);
	if (tickN % 5 == 1)                                 /* link speed and up or down, every 5 s */
		for (int i = 0; i < n->ifN; i++) {
			char p[80], st[32];
			snprintf(p, sizeof(p), "/sys/class/net/%.15s/speed", ifp[i].name);
			long m = read_long(p, -1);
			n->ifs[i].linkMbps = m > 0 ? (float)m : NAN;
			snprintf(p, sizeof(p), "/sys/class/net/%.15s/operstate", ifp[i].name);
			if (!read_line(p, st, sizeof(st))) st[0] = 0;
			g_strlcpy(n->ifs[i].oper, st, sizeof(n->ifs[i].oper));
			/* Plain words: up, cable unplugged (lowerlayerdown), down, or unknown (dormant, unknown, ...) */
			g_strlcpy(n->ifs[i].state, !strcmp(st, "up") ? "Up" : !strcmp(st, "lowerlayerdown") ? "Cable unplugged" : !strcmp(st, "down") ? "Down" : "Unknown",
				sizeof(n->ifs[i].state));
		}
	gint64 t = g_get_monotonic_time();
	int best = -1;
	for (int i = 0; i < MAX_IF; i++) n->ifs[i].present = FALSE;
	for (int pass = 0; pass < 2; pass++)                /* Ethernet first, so eth0 takes the first place */
		for (int j = 0; j < nr; j++) {
			Row* r = &rows[j];
			if (is_eth(r->name) != (pass == 0)) continue;
			int k = -1;
			for (int i = 0; i < n->ifN; i++) if (!strcmp(ifp[i].name, r->name)) k = i;
			if (k < 0 && n->ifN < MAX_IF) {
				k = n->ifN++;
				memset(&ifp[k], 0, sizeof(ifp[k]));
				g_strlcpy(ifp[k].name, r->name, sizeof(ifp[k].name));
				g_strlcpy(n->ifs[k].name, r->name, sizeof(n->ifs[k].name));
				n->ifs[k].linkMbps = NAN;
			}
			if (k < 0) continue;
			IfPriv* p = &ifp[k];
			n->ifs[k].present = TRUE;
			float* o = &x[X_NET0 + k * NET_F];
			if (p->primed && t > p->at) {
				double sec = (double)(t - p->at) / 1e6;
				for (int q = 0; q < NET_F; q++) {
					if (r->c[q] < p->c[q]) continue;     /* the counter went round or was reset: skip this reading */
					double d = (double)(r->c[q] - p->c[q]);
					o[q] = (float)(q < 2 ? 8.0 * d / sec / 1e6 : q < 4 ? d / sec : d);   /* errors and drops: a count, not a rate */
				}
			}
			memcpy(p->c, r->c, sizeof(p->c));
			memcpy(n->ifs[k].tot, &r->c[4], sizeof(n->ifs[k].tot));
			p->at = t;
			p->primed = TRUE;
			if (is_eth(r->name) && (best < 0 || r->c[0] + r->c[1] > ifp[best].c[0] + ifp[best].c[1])) best = k;
		}
	if (best < 0) return;
	/* The busiest Ethernet interface is the one in the bar, as in 1.0 */
	g_strlcpy(n->netIf, ifp[best].name, sizeof(n->netIf));
	n->rxErr = ifp[best].c[4]; n->rxDrop = ifp[best].c[5]; n->txErr = ifp[best].c[6]; n->txDrop = ifp[best].c[7];
	if (!isnan(n->ifs[best].linkMbps)) n->linkMbps = n->ifs[best].linkMbps;
	*rx = x[X_NET0 + best * NET_F + 0];
	*tx = x[X_NET0 + best * NET_F + 1];
}

/*----------------------------------------------------- storage: every drive, and each device's activity */

static gboolean fs_use(const char* path, double* totalKb, double* freeKb) {
	struct statvfs v;
	if (statvfs(path, &v) != 0 || v.f_blocks == 0) return FALSE;
	*totalKb = (double)v.f_blocks * v.f_frsize / 1024.0;
	*freeKb = (double)v.f_bavail * v.f_frsize / 1024.0;
	return TRUE;
}

static gboolean sd_present(void) {
	struct stat a, b;
	return stat(SD_PATH, &a) == 0 && stat("/", &b) == 0 && a.st_dev != b.st_dev;
}

static gboolean fs_net(const char* fs) { return !strcmp(fs, "cifs") || !strcmp(fs, "smb3") || g_str_has_prefix(fs, "nfs"); }
static gboolean fs_wanted(const char* fs) {
	static const char* ok[] = { "ext2", "ext3", "ext4", "xfs", "f2fs", "vfat", "exfat", "ubifs", "btrfs", NULL };
	for (int i = 0; ok[i]; i++) if (!strcmp(fs, ok[i])) return TRUE;
	return fs_net(fs);
}

typedef struct { char src[128]; gboolean net; } MntPriv;
static MntPriv mntp[MAX_MNT];
static dev_t appDev = 0;                            /* the device the app's own folder is on */
static gboolean appDevOk = FALSE;
static double nasTotalKb[MAX_MNT], nasFreeKb[MAX_MNT];   /* network drives, measured on their own thread (slowLock) */

/* Plain words for a drive */
static void mnt_name(Now* n, int k) {
	MntPub* m = &n->mnts[k];
	struct stat st;
	if (mntp[k].net) {
		int same = 0;
		for (int i = 0; i < k; i++) if (!strcmp(n->mnts[i].kind, "nas")) same++;
		if (same) snprintf(m->name, sizeof(m->name), "NAS %d", same + 1); else g_strlcpy(m->name, "NAS", sizeof(m->name));
		g_strlcpy(m->kind, "nas", sizeof(m->kind));
	} else if (strstr(m->path, "SD_DISK")) {
		g_strlcpy(m->name, "SD card", sizeof(m->name)); g_strlcpy(m->kind, "sd", sizeof(m->kind));
	} else if (appDevOk && stat(m->path, &st) == 0 && st.st_dev == appDev) {
		g_strlcpy(m->name, "Flash (apps)", sizeof(m->name)); g_strlcpy(m->kind, "flash", sizeof(m->kind));
	} else if (!strcmp(m->path, "/")) {
		g_strlcpy(m->name, "System", sizeof(m->name)); g_strlcpy(m->kind, "other", sizeof(m->kind));
	} else {
		/* Plain "Built-in" (no folder names like "persistent"); a second one is "Built-in 2". Under /var/spool/storage: a USB drive. */
		gboolean usb = g_str_has_prefix(m->path, "/var/spool/storage/");
		const char* base = usb ? "USB drive" : "Built-in";
		int same = 0;
		for (int i = 0; i < k; i++) if (g_str_has_prefix(n->mnts[i].name, base)) same++;
		if (same) snprintf(m->name, sizeof(m->name), "%s %d", base, same + 1); else g_strlcpy(m->name, base, sizeof(m->name));
		g_strlcpy(m->kind, usb ? "usb" : "other", sizeof(m->kind));
	}
}

/* Every read-write drive in /proc/mounts, one per device, every 30 s (a card or NAS can come and go) */
static void scan_mounts(Now* n) {
	FILE* f = fopen("/proc/mounts", "r");
	if (!f) return;
	char line[1024], seen[16][128];
	int seenN = 0;
	for (int i = 0; i < n->mntN; i++) n->mnts[i].present = FALSE;
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		if (len && line[len - 1] != '\n' && !feof(f)) {        /* longer than the buffer: skip the whole line */
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n') {}
			continue;
		}
		/* source, mount point, file system, options: split on spaces; a field that doesn't fit skips the line */
		char src[128], path[160], fs[16], opts[256];
		char* fld[4] = { NULL, NULL, NULL, NULL };
		char* save = NULL;
		int nf = 0;
		for (char* tok = strtok_r(line, " \t\n", &save); tok && nf < 4; tok = strtok_r(NULL, " \t\n", &save)) fld[nf++] = tok;
		if (nf < 4 || strlen(fld[0]) >= sizeof(src) || strlen(fld[1]) >= sizeof(path) || strlen(fld[2]) >= sizeof(fs) || strlen(fld[3]) >= sizeof(opts)) continue;
		g_strlcpy(src, fld[0], sizeof(src)); g_strlcpy(path, fld[1], sizeof(path));
		g_strlcpy(fs, fld[2], sizeof(fs)); g_strlcpy(opts, fld[3], sizeof(opts));
		if (!fs_wanted(fs) || strncmp(opts, "rw", 2) != 0 || (opts[2] && opts[2] != ',')) continue;
		gboolean dup = FALSE;
		for (int i = 0; i < seenN; i++) if (!strcmp(seen[i], src)) dup = TRUE;
		if (dup) continue;
		if (seenN < 16) g_strlcpy(seen[seenN++], src, sizeof(seen[0]));
		int k = -1;
		for (int i = 0; i < n->mntN; i++) if (!strcmp(n->mnts[i].path, path)) k = i;
		if (k < 0) {
			if (n->mntN >= MAX_MNT) continue;
			k = n->mntN++;
			MntPub* m = &n->mnts[k];
			memset(m, 0, sizeof(*m));
			m->disk = -1; m->totalKb = m->freeKb = -1;
			g_strlcpy(m->path, path, sizeof(m->path));
			g_strlcpy(mntp[k].src, src, sizeof(mntp[k].src));
			mntp[k].net = fs_net(fs);
			mnt_name(n, k);
			LOG("Drive: %s (%s, %s)\n", m->name, path, fs);
		}
		n->mnts[k].present = TRUE;
	}
	fclose(f);
}

/* SD and eMMC wear, from the card itself: life_time 0x01 = 0-10 % used ... 0x0A = 90-100 %, 0x0B = past its life */
static void read_wear(DskPub* d) {
	d->life = 0; d->eol = 0;
	GDir* h = g_dir_open("/sys/class/mmc_host", 0, NULL);
	const gchar* host;
	while (h && (host = g_dir_read_name(h))) {
		gchar* hp = g_build_filename("/sys/class/mmc_host", host, NULL);
		GDir* c = g_dir_open(hp, 0, NULL);
		const gchar* card;
		while (c && (card = g_dir_read_name(c))) {
			if (!g_str_has_prefix(card, host) || card[strlen(host)] != ':') continue;
			gchar* bp = g_build_filename(hp, card, "block", d->dev, NULL);
			gboolean mine = g_file_test(bp, G_FILE_TEST_EXISTS);
			g_free(bp);
			if (!mine) continue;
			char v[32];
			unsigned a = 0, b = 0, e = 0;
			gchar* fp = g_build_filename(hp, card, "life_time", NULL);
			if (read_line(fp, v, sizeof(v)) && sscanf(v, "%x %x", &a, &b) >= 1) { unsigned m = a > b ? a : b; d->life = m >= 1 && m <= 11 ? (int)m : 0; }
			g_free(fp);
			fp = g_build_filename(hp, card, "pre_eol_info", NULL);
			if (read_line(fp, v, sizeof(v)) && sscanf(v, "%x", &e) == 1 && e >= 1 && e <= 3) d->eol = (int)e;
			g_free(fp);
		}
		if (c) g_dir_close(c);
		g_free(hp);
	}
	if (h) g_dir_close(h);
}

typedef struct { unsigned long long c[5]; gint64 at; gboolean primed; } DskPriv;   /* reads, sectors read, writes, sectors written, ms busy */
static DskPriv dskp[MAX_DSK];

static gboolean dsk_skip(const char* d) {
	static const char* skip[] = { "loop", "ram", "zram", "mtdblock", "dm-", "md", "nbd", "sr", NULL };
	for (int i = 0; skip[i]; i++) if (g_str_has_prefix(d, skip[i])) return TRUE;
	return strstr(d, "boot") || strstr(d, "rpmb");
}
/* Whole devices from /sys/block (partitions aren't there), every 30 s */
static void scan_disks(Now* n) {
	GDir* d = g_dir_open("/sys/block", 0, NULL);
	if (!d) return;
	for (int i = 0; i < n->dskN; i++) n->dsks[i].present = FALSE;
	const gchar* e;
	while ((e = g_dir_read_name(d))) {
		if (dsk_skip(e)) continue;
		int k = -1;
		for (int i = 0; i < n->dskN; i++) if (!strcmp(n->dsks[i].dev, e)) k = i;
		if (k < 0) {
			if (n->dskN >= MAX_DSK) continue;
			k = n->dskN++;
			memset(&n->dsks[k], 0, sizeof(n->dsks[k]));
			memset(&dskp[k], 0, sizeof(dskp[k]));
			g_strlcpy(n->dsks[k].dev, e, sizeof(n->dsks[k].dev));
			read_wear(&n->dsks[k]);
		}
		n->dsks[k].present = TRUE;
	}
	g_dir_close(d);
}

/* Which device each drive is on; a device takes the name of its drive (the SD card, the flash) */
static void link_drives(Now* n) {
	for (int k = 0; k < n->mntN; k++) {
		n->mnts[k].disk = -1;
		const char* s = mntp[k].src;
		if (!g_str_has_prefix(s, "/dev/")) continue;
		s += 5;
		for (int i = 0; i < n->dskN; i++) {
			size_t l = strlen(n->dsks[i].dev);
			if (strncmp(s, n->dsks[i].dev, l) == 0 && (s[l] == 0 || isdigit((unsigned char)s[l]) || (s[l] == 'p' && isdigit((unsigned char)s[l + 1])))) n->mnts[k].disk = i;
		}
	}
	for (int i = 0; i < n->dskN; i++) {
		DskPub* d = &n->dsks[i];
		d->name[0] = 0;
		for (int pass = 0; pass < 2 && !d->name[0]; pass++)
			for (int k = 0; k < n->mntN && !d->name[0]; k++) {
				const MntPub* m = &n->mnts[k];
				if (m->disk == i && m->present && (pass || !strcmp(m->kind, "sd") || !strcmp(m->kind, "flash"))) g_strlcpy(d->name, m->name, sizeof(d->name));
			}
		if (!d->name[0]) g_strlcpy(d->name, g_str_has_prefix(d->dev, "mmcblk0") ? "Built-in flash" : g_str_has_prefix(d->dev, "mmcblk") ? "SD card" :
			g_str_has_prefix(d->dev, "sd") ? "USB drive" : d->dev, sizeof(d->name));
	}
}

/* Space on each drive: local ones every 5 s here, network ones from their own thread */
static void measure_mounts(Now* n, float* x) {
	for (int k = 0; k < n->mntN; k++) {
		MntPub* m = &n->mnts[k];
		if (!m->present) m->totalKb = m->freeKb = -1;
		else if (mntp[k].net) {
			g_mutex_lock(&slowLock);
			m->totalKb = nasTotalKb[k]; m->freeKb = nasFreeKb[k];
			g_mutex_unlock(&slowLock);
		} else if (tickN % 5 == 1 && !fs_use(m->path, &m->totalKb, &m->freeKb)) m->totalKb = m->freeKb = -1;
		x[X_MNT0 + k] = m->totalKb > 0 ? (float)(100.0 * (1.0 - m->freeKb / m->totalKb)) : NAN;
	}
}

/* Read and write activity per device, from /proc/diskstats */
static void read_disks(Now* n, float* x, float* iops) {
	FILE* f = fopen("/proc/diskstats", "r");
	if (!f) return;
	gint64 t = g_get_monotonic_time();
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char name[32];
		unsigned long long c[5];
		/* major minor name reads merged sectors ms writes merged sectors ms in-flight ms-busy ... */
		int got = sscanf(line, "%*u %*u %31s %llu %*u %llu %*u %llu %*u %llu %*u %*u %llu", name, &c[0], &c[1], &c[2], &c[3], &c[4]);
		if (got < 6) continue;
		int k = -1;
		for (int i = 0; i < n->dskN; i++) if (!strcmp(n->dsks[i].dev, name)) k = i;
		if (k < 0) continue;
		DskPriv* p = &dskp[k];
		float* o = &x[X_DSK0 + k * DSK_HF];
		gboolean up = TRUE;
		for (int q = 0; q < 5; q++) if (c[q] < p->c[q]) up = FALSE;
		if (p->primed && t > p->at && up) {
			double sec = (double)(t - p->at) / 1e6;
			o[0] = (float)((double)(c[1] - p->c[1]) * 0.5 / sec);            /* 512-byte sectors to kB/s */
			o[1] = (float)((double)(c[3] - p->c[3]) * 0.5 / sec);
			o[2] = (float)fmin(100.0, (double)(c[4] - p->c[4]) / (sec * 10.0));
			iops[k] = (float)((double)(c[0] - p->c[0] + c[2] - p->c[2]) / sec);
		}
		memcpy(p->c, c, sizeof(p->c));
		p->at = t;
		p->primed = TRUE;
	}
	fclose(f);
	return;
}

/*----------------------------------------------------- AI chip */

static unsigned long long dlpuPrev = 0;
static gint64 dlpuAt = 0;

/* One /proc/interrupts line: the sum of its per-core counts, and its description (controller, number, name) in lower case.
 * FALSE for lines without a count, such as the header. */
static gboolean irq_line(const char* line, unsigned long long* sum, char* desc, size_t n) {
	const char* p = strchr(line, ':');
	if (!p) return FALSE;
	p++;
	int counts = 0;
	*sum = 0;
	for (;;) {
		char* e;
		unsigned long long v = strtoull(p, &e, 10);
		if (e == p) break;
		*sum += v;
		counts++;
		p = e;
	}
	if (!counts) return FALSE;
	while (isspace((unsigned char)*p)) p++;
	size_t k = 0;
	for (; *p && *p != '\n' && k + 1 < n; p++) {            /* runs of spaces become one */
		if (isspace((unsigned char)*p)) { if (k && desc[k - 1] != ' ') desc[k++] = ' '; }
		else desc[k++] = (char)g_ascii_tolower(*p);
	}
	desc[k] = 0;
	return TRUE;
}
/* Which kind of AI chip a line counts, or NULL */
static const char* dlpu_kind(const char* desc) {
	if (strstr(desc, "ethosn")) return "ARTPEC";
	if (strstr(desc, "cavalry_job_done")) return "Ambarella";
	/* ARTPEC-8 shows no AI chip work counter: vip0/vip1 run a flat ~240 a second on every camera (the video pipeline),
	 * and the pd_dlp_* lines are power switches that never move. So the AI chip stays hidden there. */
	return NULL;
}

/* At start: write every interrupt that may be the AI chip to the camera log, to find the right one on new models */
static void log_dlpu_candidates(void) {
	FILE* f = fopen("/proc/interrupts", "r");
	if (!f) { LOG("AI chip: /proc/interrupts is not readable\n"); return; }
	static const char* words[] = { "dlp", "dlpu", "npu", "ethos", "nn", "cavalry", "larod", "vip", "cnn", "accel", NULL };
	char line[1024], desc[160];
	GString* out = g_string_new("");
	int found = 0;
	while (fgets(line, sizeof(line), f) && out->len < 900) {
		unsigned long long sum;
		if (!irq_line(line, &sum, desc, sizeof(desc))) continue;
		gboolean hit = FALSE;
		for (int i = 0; words[i] && !hit; i++) hit = strstr(desc, words[i]) != NULL;
		if (!hit) continue;
		g_string_append_printf(out, "%s%.60s = %llu", found ? "; " : "", desc, sum);
		found++;
	}
	fclose(f);
	LOG("AI chip: interrupts that may be it: %s\n", found ? out->str : "none");
	g_string_free(out, TRUE);
}

/* AI chip jobs a second: the chip's work interrupts (ARTPEC: ethosn; Ambarella: cavalry_job_done; none readable on ARTPEC-8),
 * added together. Looked for once a minute until found. */
static float read_dlpu(Now* n) {
	if (!n->dlpu[0] && tickN % 60 != 1) return NAN;
	FILE* f = fopen("/proc/interrupts", "r");
	if (!f) return NAN;
	char line[1024], desc[160];
	unsigned long long sum = 0;
	const char* kind = NULL;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long s;
		if (!irq_line(line, &s, desc, sizeof(desc))) continue;
		const char* k = dlpu_kind(desc);
		if (!k) continue;
		kind = k;
		sum += s;
	}
	fclose(f);
	gint64 t = g_get_monotonic_time();
	if (!kind) { n->dlpu[0] = 0; dlpuAt = 0; return NAN; }
	if (!n->dlpu[0]) LOG("AI chip: counting %s jobs from /proc/interrupts\n", kind);
	g_strlcpy(n->dlpu, kind, sizeof(n->dlpu));
	float r = dlpuAt && t > dlpuAt && sum >= dlpuPrev ? (float)((double)(sum - dlpuPrev) / ((double)(t - dlpuAt) / 1e6)) : NAN;
	dlpuPrev = sum;
	dlpuAt = t;
	return r;
}

/*----------------------------------------------------- slow sources: their own threads, every 30 s */

/* temperaturecontrol.cgi?action=statusall: Sensor.S0.Name=..., Sensor.S0.Celsius=..., Fan.F0.Status=..., Heater.H0.Status=... */
static gboolean poll_temp(Slow* r) {
	r->tempOk = r->fanOk = r->heaterOk = r->fanRunning = r->heaterOn = FALSE;
	r->vsN = 0;
	r->fanRpm = NAN;
	char* x = ACAP_VAPIX_Get_Background("temperaturecontrol.cgi?action=statusall");
	if (!x) return FALSE;
	char ids[MAX_SENS][16];
	char* save = NULL;
	for (char* l = strtok_r(x, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
		char* eq = strchr(l, '=');
		if (!eq) continue;
		*eq = 0;
		const char* val = eq + 1;
		char grp[16], id[16], field[32];
		if (sscanf(l, "%15[^.].%15[^.].%31s", grp, id, field) != 3) continue;
		gchar* fl = g_ascii_strdown(field, -1);
		gboolean on = !g_ascii_strcasecmp(val, "Running") || !g_ascii_strcasecmp(val, "On") || !g_ascii_strcasecmp(val, "true");
		if (!g_ascii_strcasecmp(grp, "Sensor")) {
			int k = -1;
			for (int i = 0; i < r->vsN; i++) if (!strcmp(ids[i], id)) k = i;
			if (k < 0 && r->vsN < MAX_SENS) { k = r->vsN++; g_strlcpy(ids[k], id, sizeof(ids[k])); r->vsName[k][0] = 0; r->vsC[k] = NAN; }
			if (k >= 0 && !strcmp(fl, "name")) g_strlcpy(r->vsName[k], val, sizeof(r->vsName[k]));
			if (k >= 0 && !strcmp(fl, "celsius")) r->vsC[k] = (float)atof(val);
		} else if (!g_ascii_strcasecmp(grp, "Fan")) {
			r->fanOk = TRUE;
			if (strstr(fl, "status") || strstr(fl, "state")) r->fanRunning = r->fanRunning || on;
			if ((strstr(fl, "rpm") || strstr(fl, "speed")) && isdigit((unsigned char)val[0])) r->fanRpm = fmaxn(r->fanRpm, (float)atof(val));
		} else if (!g_ascii_strcasecmp(grp, "Heater")) {
			r->heaterOk = TRUE;
			if (strstr(fl, "status") || strstr(fl, "state")) r->heaterOn = r->heaterOn || on;
		}
		g_free(fl);
	}
	free(x);
	for (int i = 0; i < r->vsN; i++) if (!r->vsName[i][0]) snprintf(r->vsName[i], sizeof(r->vsName[i]), "Sensor %d", i + 1);
	r->tempOk = r->vsN > 0 || r->fanOk || r->heaterOk;
	return r->tempOk;
}

/* Units come from the field name only: "...mW" or "milliwatt" is milliwatts, anything else watts */
static double to_watts(const char* lowKey, double v) { return strstr(lowKey, "mw") || strstr(lowKey, "milliwatt") ? v / 1000.0 : v; }

/* Fallback for firmware whose answer looks different: the draw anywhere in it, and a class that isn't negative */
static void power_walk(cJSON* o, float* w, char* cls, size_t clsN) {
	cJSON* c;
	cJSON_ArrayForEach(c, o) {
		if (cJSON_IsObject(c) || cJSON_IsArray(c)) { power_walk(c, w, cls, clsN); continue; }
		if (!c->string) continue;
		gchar* k = g_ascii_strdown(c->string, -1);
		if (strstr(k, "class")) {
			if (!cls[0] && cJSON_IsNumber(c) && c->valueint >= 0) snprintf(cls, clsN, "%u", (unsigned)c->valueint % 1000u);
			else if (!cls[0] && cJSON_IsString(c)) g_strlcpy(cls, c->valuestring, clsN);
		} else if (cJSON_IsNumber(c) && isnan(*w) && !strstr(k, "max") && !strstr(k, "limit") && !strstr(k, "version") && !strstr(k, "average") && !strstr(k, "request") &&
			(strstr(k, "consum") || strstr(k, "draw") || strstr(k, "usage") || strstr(k, "currentpower") || strstr(k, "watt") || !strcmp(k, "power"))) {
			*w = (float)to_watts(k, c->valuedouble);
		}
		g_free(k);
	}
}
static float jnum(cJSON* o, const char* key) {
	cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
}
static char powerVer[16] = "";
static gboolean power_version(void) {
	/* The newest version the camera speaks (the Q1808-LE answers 1.7) */
	char* v = ACAP_VAPIX_Post_Background("power-settings.cgi", "{\"method\":\"getSupportedVersions\"}");
	cJSON* jv = v ? cJSON_Parse(v) : NULL;
	free(v);
	cJSON* list = cJSON_GetObjectItem(cJSON_GetObjectItem(jv, "data"), "apiVersions");
	cJSON* e;
	double best = -1;
	cJSON_ArrayForEach(e, list) if (cJSON_IsString(e) && atof(e->valuestring) > best) { best = atof(e->valuestring); g_strlcpy(powerVer, e->valuestring, sizeof(powerVer)); }
	cJSON_Delete(jv);
	return best >= 0;
}
/* getPowerStatus, e.g. {"data":{"usage":{"currentPower":12.05,"averagePower":12.25,"maxPower":18.02},"psePoeClass":6,"lldpPoeClass":-1,"powerRequested":51.0}} */
static gboolean poll_power(Slow* r) {
	r->powerOk = FALSE;
	r->powerW = r->powerAvgW = r->powerMaxW = r->powerReqW = NAN;
	r->powerCls[0] = 0;
	/* Once an hour: does the camera keep its own power history? */
	static gint64 capAt = 0;
	gint64 tm = g_get_monotonic_time();
	if (!capAt || tm - capAt > (gint64)3600 * G_USEC_PER_SEC) {
		capAt = tm;
		if (!powerVer[0] && !power_version()) g_strlcpy(powerVer, "1.0", sizeof(powerVer));
		gchar* cb = g_strdup_printf("{\"apiVersion\":\"%s\",\"method\":\"getCapabilities\"}", powerVer);
		char* cx = ACAP_VAPIX_Post_Background("power-settings.cgi", cb);
		g_free(cb);
		cJSON* cj = cx ? cJSON_Parse(cx) : NULL;
		free(cx);
		r->powerHist = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(cj, "data"), "powerHistorySupport"));
		if (cj) r->powerCapKnown = TRUE;
		cJSON_Delete(cj);
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!powerVer[0] && !power_version()) g_strlcpy(powerVer, "1.0", sizeof(powerVer));
		gchar* body = g_strdup_printf("{\"apiVersion\":\"%s\",\"method\":\"getPowerStatus\"}", powerVer);
		char* x = ACAP_VAPIX_Post_Background("power-settings.cgi", body);
		g_free(body);
		cJSON* j = x ? cJSON_Parse(x) : NULL;
		free(x);
		if (!j) return FALSE;
		if (cJSON_GetObjectItem(j, "error")) {             /* the version may have changed: ask again once */
			cJSON_Delete(j);
			powerVer[0] = 0;
			if (attempt) return FALSE;
			continue;
		}
		cJSON* data = cJSON_GetObjectItem(j, "data");
		cJSON* usage = cJSON_GetObjectItem(data, "usage");
		r->powerW = jnum(usage, "currentPower");
		r->powerAvgW = jnum(usage, "averagePower");
		r->powerMaxW = jnum(usage, "maxPower");
		r->powerReqW = jnum(data, "powerRequested");
		float pse = jnum(data, "psePoeClass"), lldp = jnum(data, "lldpPoeClass");
		float cls = !isnan(pse) && pse >= 0 ? pse : !isnan(lldp) && lldp >= 0 ? lldp : NAN;
		if (!isnan(cls)) snprintf(r->powerCls, sizeof(r->powerCls), "%u", (unsigned)cls % 1000u);
		if (isnan(r->powerW) && data) power_walk(data, &r->powerW, r->powerCls, sizeof(r->powerCls));
		cJSON_Delete(j);
		r->powerOk = !isnan(r->powerW);
		return r->powerOk;
	}
	return FALSE;
}

/* A forgiving reader for the recorder's PoE answers: every name="value" and <name>value</name> in [p, end) */
typedef void (*KvFn)(const char* key, const char* val, void* u);
static void xml_pairs(const char* start, const char* end, KvFn fn, void* u) {
	char key[48], val[64];
	for (const char* p = start; p < end; p++) {
		const char *ks = NULL, *ke = NULL, *vs = NULL, *ve = NULL;
		if (*p == '=' && p + 1 < end && p[1] == '"') {
			ke = p; ks = p;
			while (ks > start && (isalnum((unsigned char)ks[-1]) || ks[-1] == '_' || ks[-1] == '-')) ks--;
			vs = p + 2;
			ve = memchr(vs, '"', (size_t)(end - vs));
			if (!ve) return;
		} else if (*p == '<' && p + 1 < end && isalpha((unsigned char)p[1])) {
			ks = p + 1; ke = ks;
			while (ke < end && (isalnum((unsigned char)*ke) || *ke == '_' || *ke == '-')) ke++;
			if (ke >= end || *ke != '>') continue;
			vs = ke + 1; ve = vs;
			while (ve < end && *ve != '<') ve++;
		} else continue;
		while (vs < ve && isspace((unsigned char)*vs)) vs++;
		if (ke <= ks || ve <= vs) continue;
		g_strlcpy(key, ks, MIN(sizeof(key), (size_t)(ke - ks) + 1));
		g_strlcpy(val, vs, MIN(sizeof(val), (size_t)(ve - vs) + 1));
		fn(key, val, u);
		if (*p == '=') p = ve;
	}
}
static float watts(const char* key, const char* val) {
	double v = atof(val);
	return (float)to_watts(key, v);               /* key is in lower case */
}
static void port_kv(const char* key, const char* val, void* u) {
	PortPub* pt = u;
	gchar* k = g_ascii_strdown(key, -1);
	gboolean numv = isdigit((unsigned char)val[0]) || val[0] == '.';
	if (strstr(k, "class")) g_strlcpy(pt->cls, val, sizeof(pt->cls));
	else if (strstr(k, "status") || strstr(k, "state")) g_strlcpy(pt->state, val, sizeof(pt->state));
	else if (numv && (strstr(k, "limit") || strstr(k, "max") || strstr(k, "alloc") || strstr(k, "budget"))) pt->limitW = watts(k, val);
	else if (numv && (strstr(k, "power") || strstr(k, "consum") || strstr(k, "usage") || strstr(k, "draw") || strstr(k, "watt"))) pt->w = watts(k, val);
	else if (numv && (!strcmp(k, "port") || !strcmp(k, "id") || strstr(k, "portnumber") || strstr(k, "portid") || !strcmp(k, "number") || !strcmp(k, "index"))) pt->port = atoi(val);
	g_free(k);
}
static void limit_kv(const char* key, const char* val, void* u) {
	float* w = u;
	gchar* k = g_ascii_strdown(key, -1);
	if (isnan(*w) && isdigit((unsigned char)val[0]) && (strstr(k, "limit") || strstr(k, "power") || strstr(k, "total"))) *w = watts(k, val);
	g_free(k);
}
/* Recorders: each PoE port, and the total the recorder can give. Other cameras answer neither. */
static gboolean poll_poe(Slow* r) {
	r->poeOk = FALSE;
	r->portsN = 0;
	r->budgetW = NAN;
	char* x = ACAP_VAPIX_Get_Background("nvr/poe/getportstatuses.cgi");
	if (!x) return FALSE;
	const char* end = x + strlen(x);
	for (const char* p = strchr(x, '<'); p && r->portsN < MAX_PORTS; p = strchr(p + 1, '<')) {
		if (!isalpha((unsigned char)p[1])) continue;
		const char* te = p + 1;
		while (te < end && (isalnum((unsigned char)*te) || *te == '_' || *te == '-')) te++;
		gchar* tag = g_strndup(p + 1, (gsize)(te - p - 1));
		gchar* low = g_ascii_strdown(tag, -1);
		/* One port: a tag with "port" in its name, not a list of them */
		gboolean one = strstr(low, "port") && !g_str_has_suffix(low, "s") && !strstr(low, "response") && !strstr(low, "success") && !strstr(low, "list");
		g_free(low);
		const char* gt = strchr(te, '>');
		if (!one || !gt) { g_free(tag); continue; }
		const char* be = gt + 1;
		if (gt[-1] != '/') {                        /* not <Port .../>: up to </Port> */
			gchar* close = g_strdup_printf("</%s>", tag);
			const char* c = strstr(gt, close);
			g_free(close);
			be = c ? c : gt + 1;
		}
		g_free(tag);
		PortPub pt;
		memset(&pt, 0, sizeof(pt));
		pt.w = pt.limitW = NAN;
		xml_pairs(p, be, port_kv, &pt);
		if (isnan(pt.w) && !pt.state[0]) continue;
		if (!pt.port) pt.port = r->portsN + 1;
		r->ports[r->portsN++] = pt;
		p = be - 1;                                  /* go on from the end of this port (be is never past the final NUL) */
	}
	free(x);
	x = ACAP_VAPIX_Get_Background("nvr/poe/gettotalpowerlimit.cgi");
	if (x) { xml_pairs(x, x + strlen(x), limit_kv, &r->budgetW); free(x); }
	r->poeOk = r->portsN > 0;
	return r->poeOk;
}


/*----------------------------------------------------- what the camera itself reports: streams, disks, its audit log */

/* All read on the slow thread through the background connection, kept under slowLock in fixed lists
 * (nothing grows), and sent to the page by the camera endpoint. */
#define MAX_STREAMS 16
#define MAX_CDISKS  4
#define MAX_EVENTS  50
typedef struct {
	int id, srcPort, dstPort;
	gboolean encrypted, multicast;
	char direction[12], state[16], media[12], mime[32], transport[12], proto[16], src[48], dst[48], agent[128], path[96], res[16], codec[12], fps[8], cam[4];
} StreamInfo;
typedef struct {
	char id[24];
	float wear, tempC;
	int health;                                          /* 1 passed, 0 failed, -1 not reported */
	double limitKb, usedKb;                              /* recordings: allowed (0 no limit, -1 unknown), used (-1 unknown) */
} CamDisk;
/* 1.6.1: also where it came from, which camera service wrote it and what it says */
typedef struct { gint64 t; char kind[20]; char user[32]; char from[48]; char service[24]; char area[32]; char what[120]; } CamEvent;
static StreamInfo streams[MAX_STREAMS];
static int streamN = 0;
static gboolean streamsOk = FALSE;
static CamDisk cdisks[MAX_CDISKS];
static int cdiskN = 0;
static CamEvent events[MAX_EVENTS];                      /* newest last */
static int eventN = 0;
static gboolean eventsOk = FALSE;
static gint64 eventsClearedAt = 0;                       /* Settings › Clear camera events list (slowLock) */

static void jstr(cJSON* o, const char* key, char* out, size_t n) {
	cJSON* v = cJSON_GetObjectItem(o, key);
	if (cJSON_IsString(v)) g_strlcpy(out, v->valuestring, n);
	else if (cJSON_IsNumber(v)) snprintf(out, n, "%g", v->valuedouble);
	else out[0] = 0;
}
/* streamstatus.cgi getAllStreams: who is watching or listening, every 30 s */
static void poll_streams(void) {
	char* x = ACAP_VAPIX_Post_Background("streamstatus.cgi", "{\"apiVersion\":\"1.0\",\"method\":\"getAllStreams\"}");
	cJSON* j = x ? cJSON_Parse(x) : NULL;
	free(x);
	cJSON* list = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "data"), "streams");
	StreamInfo tmp[MAX_STREAMS];
	int n = 0;
	cJSON* s;
	cJSON_ArrayForEach(s, list) {
		if (n >= MAX_STREAMS) break;
		StreamInfo* t = &tmp[n++];
		memset(t, 0, sizeof(*t));
		t->id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(s, "id"));
		t->encrypted = cJSON_IsTrue(cJSON_GetObjectItem(s, "encrypted"));
		t->multicast = cJSON_IsTrue(cJSON_GetObjectItem(s, "multicast"));
		jstr(s, "direction", t->direction, sizeof(t->direction)); jstr(s, "state", t->state, sizeof(t->state));
		jstr(s, "media", t->media, sizeof(t->media)); jstr(s, "mime", t->mime, sizeof(t->mime));
		jstr(s, "transport_protocol", t->transport, sizeof(t->transport));          /* TCP, UDP */
		if (!t->transport[0]) jstr(s, "transport", t->transport, sizeof(t->transport));
		jstr(s, "stream_protocol", t->proto, sizeof(t->proto));                    /* RTSP, HTTP, ... */
		jstr(s, "source_address", t->src, sizeof(t->src)); jstr(s, "destination_address", t->dst, sizeof(t->dst));
		double sp = cJSON_GetNumberValue(cJSON_GetObjectItem(s, "source_port")), dp = cJSON_GetNumberValue(cJSON_GetObjectItem(s, "destination_port"));
		t->srcPort = isnan(sp) ? 0 : (int)sp; t->dstPort = isnan(dp) ? 0 : (int)dp;
		jstr(s, "user_agent", t->agent, sizeof(t->agent));
		jstr(s, "path", t->path, sizeof(t->path));
		if (!t->path[0]) jstr(s, "uri", t->path, sizeof(t->path));
		cJSON* o = cJSON_GetObjectItem(s, "options");
		/* the options belong to the whole request: an audio stream takes the audio codec, never the video one */
		gboolean audio = !g_ascii_strcasecmp(t->media, "audio");
		if (!g_ascii_strcasecmp(t->media, "video")) jstr(o, "resolution", t->res, sizeof(t->res));
		jstr(o, audio ? "audiocodec" : "videocodec", t->codec, sizeof(t->codec));
		if (!g_ascii_strcasecmp(t->media, "video")) jstr(o, "fps", t->fps, sizeof(t->fps));
		jstr(o, "camera", t->cam, sizeof(t->cam));
	}
	gboolean ok = cJSON_IsArray(list);
	cJSON_Delete(j);
	g_mutex_lock(&slowLock);
	memcpy(streams, tmp, sizeof(StreamInfo) * (size_t)n);
	streamN = n;
	streamsOk = ok;
	g_mutex_unlock(&slowLock);
}

/* One <disk ...> of gethealth.cgi or getlimit.cgi: the name="value" pairs we need */
typedef struct { char id[24]; double wear, temp, health, size, used; } DiskKv;
static void disk_kv(const char* key, const char* val, void* u) {
	DiskKv* d = u;
	gchar* k = g_ascii_strdown(key, -1);
	double v = atof(val);
	if (!strcmp(k, "diskid")) g_strlcpy(d->id, val, sizeof(d->id));
	else if (!strcmp(k, "wear")) d->wear = v;
	else if (!strcmp(k, "temperature")) d->temp = v;
	else if (!strcmp(k, "overallhealth")) d->health = v;
	else if (!strcmp(k, "size")) d->size = v;
	else if (!strcmp(k, "usedsize") || !strcmp(k, "usedspace")) d->used = v;
	g_free(k);
}
/* Each <disk ...> block (self-closing or up to </disk>) of an answer */
static int disk_blocks(const char* x, DiskKv* out, int cap) {
	int n = 0;
	const char* end = x + strlen(x);
	for (const char* p = strstr(x, "<disk"); p && n < cap; p = strstr(p + 1, "<disk")) {
		if (isalnum((unsigned char)p[5])) continue;       /* <disks>, <diskinfo>: not one disk */
		const char* gt = strchr(p, '>');
		if (!gt) break;
		const char* be = gt + 1;
		if (gt[-1] != '/') { const char* c = strstr(gt, "</disk>"); be = c ? c : gt + 1; }
		DiskKv* d = &out[n];
		memset(d, 0, sizeof(*d));
		d->wear = d->temp = d->health = d->size = d->used = NAN;
		xml_pairs(p, be, disk_kv, d);
		if (d->id[0]) n++;
		if (be >= end) break;
	}
	return n;
}
/* disks/gethealth.cgi (wear, temperature, health) and record/storage/getlimit.cgi (space allowed for recordings), every 5 min */
static void poll_disks(void) {
	CamDisk tmp[MAX_CDISKS];
	int n = 0;
	DiskKv kv[MAX_CDISKS];
	char* x = ACAP_VAPIX_Get_Background("disks/gethealth.cgi?diskid=all");
	int hn = x ? disk_blocks(x, kv, MAX_CDISKS) : 0;
	free(x);
	for (int i = 0; i < hn; i++) {
		CamDisk* d = &tmp[n++];
		memset(d, 0, sizeof(*d));
		g_strlcpy(d->id, kv[i].id, sizeof(d->id));
		d->wear = (float)kv[i].wear;
		d->tempC = isnan(kv[i].temp) ? NAN : (float)(kv[i].temp > 200 ? kv[i].temp - 273.15 : kv[i].temp);   /* Kelvin */
		d->health = isnan(kv[i].health) ? -1 : (int)kv[i].health;
		d->limitKb = d->usedKb = -1;
	}
	if (!n) {                                             /* no health answer: still ask about the SD card's recordings */
		memset(&tmp[0], 0, sizeof(tmp[0]));
		g_strlcpy(tmp[0].id, "SD_DISK", sizeof(tmp[0].id));
		tmp[0].wear = tmp[0].tempC = NAN; tmp[0].health = -1; tmp[0].limitKb = tmp[0].usedKb = -1;
		n = 1;
	}
	gboolean any = hn > 0;
	for (int i = 0; i < n; i++) {
		gchar* q = g_strdup_printf("record/storage/getlimit.cgi?schemaversion=1&diskid=%s", tmp[i].id);
		char* y = ACAP_VAPIX_Get_Background(q);
		g_free(q);
		DiskKv lk[1];
		if (y && disk_blocks(y, lk, 1) == 1) {
			tmp[i].limitKb = isnan(lk[0].size) ? -1 : lk[0].size;
			tmp[i].usedKb = isnan(lk[0].used) ? -1 : lk[0].used;
			any = TRUE;
		} else if (y) {                                   /* not in a <disk> element: the pairs anywhere in the answer */
			DiskKv a;
			memset(&a, 0, sizeof(a));
			a.size = a.used = NAN;
			xml_pairs(y, y + strlen(y), disk_kv, &a);
			if (!isnan(a.size)) { tmp[i].limitKb = a.size; tmp[i].usedKb = isnan(a.used) ? -1 : a.used; any = TRUE; }
		}
		free(y);
	}
	g_mutex_lock(&slowLock);
	memcpy(cdisks, tmp, sizeof(CamDisk) * (size_t)n);
	cdiskN = any ? n : 0;
	g_mutex_unlock(&slowLock);
}

/* An audit log line in plain words: a kind, and the user name if the line gives one. Never the rest of the line
 * (it can hold addresses, tokens or other secrets). */
static void audit_summary(const char* line, char* kind, size_t kn, char* user, size_t un) {
	gchar* l = g_ascii_strdown(line, -1);
	gboolean login = strstr(l, "login") || strstr(l, "logged in") || strstr(l, "authenticat") || strstr(l, "session opened");
	gboolean fail = strstr(l, "fail") || strstr(l, "denied") || strstr(l, "invalid") || strstr(l, "wrong");
	const char* k = login && fail ? "Failed login" : login ? "Login" : strstr(l, "firmware") && (strstr(l, "upgrad") || strstr(l, "updat")) ? "Firmware upgrade"
		: strstr(l, "reboot") || strstr(l, "restart") || strstr(l, "shutdown") || strstr(l, "power cycle") ? "Restart"
		: strstr(l, "param") || strstr(l, "setting") || strstr(l, "config") ? "Settings changed" : "Other";
	g_strlcpy(kind, k, kn);
	user[0] = 0;
	/* user=NAME, user 'NAME', user "NAME", user NAME, for user NAME */
	const char* u = strstr(l, "user");
	if (u) {
		const char* p = line + (u - l) + 4;
		while (*p == '=' || *p == ':' || *p == ' ' || *p == '\'' || *p == '"' || *p == '(') p++;
		size_t i = 0;
		while (p[i] && i + 1 < un && !isspace((unsigned char)p[i]) && !strchr("'\",;)]", p[i])) { user[i] = p[i]; i++; }
		user[i] = 0;
		gchar* ul = g_ascii_strdown(user, -1);
		if (strstr(ul, "pass") || strstr(ul, "token") || strstr(ul, "=") || i >= un - 1) user[0] = 0;   /* never a secret, never a guess */
		g_free(ul);
	}
	g_free(l);
}
/* 1.6.1: the rest of an audit line: "... httpd[123]: Authentication: root@192.0.2.67:57821 Authentication successful (HTTP/S)."
 * gives service httpd, area Authentication, user root, from 192.0.2.67 (no port), what "Authentication successful (HTTP/S)".
 * Anything that may be a secret (pass..., token, key=) cuts the text there. */
static void audit_detail(const char* line, CamEvent* e) {
	e->from[0] = e->service[0] = e->area[0] = e->what[0] = 0;
	const char* b = strstr(line, "] ");                      /* after "[ INFO    ]" */
	const char* s = b ? b + 2 : line;
	while (*s == ' ') s++;
	const char* c = strstr(s, ": ");
	if (!c) { g_strlcpy(e->what, s, sizeof(e->what)); goto clean; }
	size_t n = (size_t)(c - s);
	const char* br = memchr(s, '[', n);
	g_strlcpy(e->service, s, MIN(sizeof(e->service), (br ? (size_t)(br - s) : n) + 1));
	s = c + 2;
	c = strstr(s, ": ");
	if (c && (size_t)(c - s) < sizeof(e->area) && !memchr(s, '@', (size_t)(c - s))) { g_strlcpy(e->area, s, (size_t)(c - s) + 1); s = c + 2; }
	/* user@address:port at the start */
	const char* at = strchr(s, '@'), *sp = strchr(s, ' ');
	if (at && (!sp || at < sp)) {
		if (!e->user[0]) g_strlcpy(e->user, s, MIN(sizeof(e->user), (size_t)(at - s) + 1));
		const char* a = at + 1, *end = sp ? sp : a + strlen(a);
		const char* colon = memchr(a, ':', (size_t)(end - a));
		if (*a == '[') { const char* rb = memchr(a, ']', (size_t)(end - a)); if (rb) { a++; colon = rb; } }
		g_strlcpy(e->from, a, MIN(sizeof(e->from), (size_t)((colon ? colon : end) - a) + 1));
		s = sp ? sp + 1 : end;
	}
	g_strlcpy(e->what, s, sizeof(e->what));
clean:
	g_strchomp(e->what);
	{
		gchar* lw = g_ascii_strdown(e->what, -1);
		const char* bad[] = { "pass", "token", "secret", "key=" };
		for (int i = 0; i < 4; i++) { const char* p = strstr(lw, bad[i]); if (p) { e->what[p - lw] = 0; g_strlcat(e->what, "…", sizeof(e->what)); break; } }
		g_free(lw);
	}
	gchar* ul = g_ascii_strdown(e->user, -1);
	if (strstr(ul, "pass") || strstr(ul, "token")) e->user[0] = 0;
	g_free(ul);
}
/* A syslog line's time: ISO "2026-11-24T20:30:00[.123][+01:00]" or "Nov 24 20:30:00" (this year), as unix seconds */
static gint64 audit_time(const char* line) {
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	int y, mo, d, h, mi, s;
	if (sscanf(line, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
		tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
		const char* z = strchr(line + 19, ' ');
		const char* o = line + 19;
		while (*o == '.' || isdigit((unsigned char)*o)) o++;
		if ((*o == '+' || *o == '-') && (!z || o < z)) {
			int oh = 0, om = 0;
			sscanf(o + 1, "%d:%d", &oh, &om);
			gint64 t = (gint64)timegm(&tm) - (*o == '+' ? 1 : -1) * (oh * 3600 + om * 60);
			return t;
		}
		if (*o == 'Z') return (gint64)timegm(&tm);
		tm.tm_isdst = -1;
		return (gint64)mktime(&tm);
	}
	static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
	char mon[4] = "";
	if (sscanf(line, "%3s %d %d:%d:%d", mon, &d, &h, &mi, &s) == 5) {
		const char* f = strstr(M, mon);
		if (!f || mon[0] == 0) return 0;
		time_t nowt = time(NULL);
		struct tm nt;
		localtime_r(&nowt, &nt);
		tm.tm_year = nt.tm_year; tm.tm_mon = (int)(f - M) / 3; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s; tm.tm_isdst = -1;
		gint64 t = (gint64)mktime(&tm);
		if (t > (gint64)nowt + 86400) { tm.tm_year--; tm.tm_isdst = -1; t = (gint64)mktime(&tm); }   /* December's lines read in January */
		return t;
	}
	return 0;
}
/* auditlog.cgi?tail=200, every 60 s: the last 50 events, newest last */
static void poll_audit(void) {
	char* x = ACAP_VAPIX_Get_Background("auditlog.cgi?tail=200");
	if (!x) { g_mutex_lock(&slowLock); eventsOk = FALSE; g_mutex_unlock(&slowLock); return; }
	g_mutex_lock(&slowLock);
	eventsOk = TRUE;
	gint64 newest = eventN ? events[eventN - 1].t : 0;
	char* save = NULL;
	for (char* l = strtok_r(x, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
		gint64 t = audit_time(l);
		if (!t || t < newest || t <= eventsClearedAt) continue;   /* cleared in Settings: not listed again */
		CamEvent e;
		memset(&e, 0, sizeof(e));
		e.t = t;
		audit_summary(l, e.kind, sizeof(e.kind), e.user, sizeof(e.user));
		audit_detail(l, &e);
		gboolean seen = FALSE;                            /* the same second, kind and user is the same event */
		for (int i = 0; i < eventN; i++) if (events[i].t == e.t && !strcmp(events[i].kind, e.kind) && !strcmp(events[i].user, e.user) && !strcmp(events[i].from, e.from) && !strcmp(events[i].what, e.what)) seen = TRUE;
		if (seen) continue;
		if (eventN == MAX_EVENTS) { memmove(events, events + 1, sizeof(CamEvent) * (MAX_EVENTS - 1)); eventN--; }
		events[eventN++] = e;
	}
	g_mutex_unlock(&slowLock);
	free(x);
}
/* Clear the camera events list: everything up to `before` (unix s) stays out, also when the audit log still has it */
void sampler_clear_events(gint64 before) {
	g_mutex_lock(&slowLock);
	eventsClearedAt = before;
	eventN = 0;
	g_mutex_unlock(&slowLock);
}

/* 1.6.0 Time sync and Security, as the camera's own pages show them, every 60 s (fixed fields, nothing grows) */
typedef struct {
	gboolean ok, enabled, synced;
	double offsetMs;                                     /* the camera's timeOffset; its own page shows it as ms */
	double nextS; gint64 readAt;                         /* timeToNextSync and when it was read (monotonic) */
	char source[12], servers[200];
} CamTime;
typedef struct {
	gboolean ok;
	int access;                                          /* 0 unknown, 1 HTTP only, 2 HTTPS only, 3 both */
	int oldTls, ssh, bonjour, upnp, wsd, zeroconf, unsignedOk;   /* 1 on, 0 off, -1 the camera has no such setting */
} CamSec;
static CamTime camTime;
static CamSec camSec;

static void add_server(char* out, size_t n, const char* s) {
	if (!s || !*s) return;
	size_t l = strlen(s);
	for (const char* p = strstr(out, s); p; p = strstr(p + 1, s))          /* already listed */
		if ((p == out || p[-1] == ' ') && (p[l] == ',' || p[l] == 0)) return;
	if (*out) g_strlcat(out, ", ", n);
	g_strlcat(out, s, n);
}
static void add_servers(char* out, size_t n, cJSON* list) {
	cJSON* it;
	cJSON_ArrayForEach(it, list) if (cJSON_IsString(it)) add_server(out, n, it->valuestring);
}
/* ntp.cgi getNTPInfo; servers as the camera lists them: DHCP = advertised and manual, static = manual, secure = NTS KE */
static void poll_time(void) {
	CamTime t;
	memset(&t, 0, sizeof(t));
	char* x = ACAP_VAPIX_Post_Background("ntp.cgi", "{\"apiVersion\":\"1.0\",\"method\":\"getNTPInfo\"}");
	cJSON* j = x ? cJSON_Parse(x) : NULL;
	free(x);
	cJSON* c = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "data"), "client");
	if (c) {
		t.ok = TRUE;
		t.enabled = cJSON_IsTrue(cJSON_GetObjectItem(c, "enabled"));
		t.synced = cJSON_IsTrue(cJSON_GetObjectItem(c, "synced"));
		cJSON* o = cJSON_GetObjectItem(c, "timeOffset"), *nx = cJSON_GetObjectItem(c, "timeToNextSync");
		t.offsetMs = cJSON_IsNumber(o) ? o->valuedouble : NAN;
		t.nextS = cJSON_IsNumber(nx) ? nx->valuedouble : NAN;
		t.readAt = g_get_monotonic_time();
		jstr(c, "serversSource", t.source, sizeof(t.source));
		if (!g_ascii_strcasecmp(t.source, "secure")) add_servers(t.servers, sizeof(t.servers), cJSON_GetObjectItem(c, "staticNTSKEServers"));
		else {
			if (g_ascii_strcasecmp(t.source, "static")) add_servers(t.servers, sizeof(t.servers), cJSON_GetObjectItem(c, "advertisedServers"));
			add_servers(t.servers, sizeof(t.servers), cJSON_GetObjectItem(c, "staticServers"));
		}
	}
	cJSON_Delete(j);
	g_mutex_lock(&slowLock);
	camTime = t;
	g_mutex_unlock(&slowLock);
}
/* "root.X=yes" in a param.cgi answer: 1 yes/true, 0 no/false, -1 not there */
static int param_on(const char* x, const char* name) {
	char key[96];
	snprintf(key, sizeof(key), "\n%s=", name);
	if (!x) return -1;
	const char* p = strstr(x, key);
	if (p) p += strlen(key);
	else if (!strncmp(x, key + 1, strlen(key + 1))) p = x + strlen(key + 1);   /* the first line */
	else return -1;
	return !g_ascii_strncasecmp(p, "yes", 3) || !g_ascii_strncasecmp(p, "true", 4) ? 1 : 0;
}
/* The camera's Security card: web access (virtualhost), TLS 1.0/1.1, SSH, discovery (param.cgi), unsigned apps */
static void poll_security(void) {
	CamSec s;
	memset(&s, 0, sizeof(s));
	char* x = ACAP_VAPIX_Get_Background("param.cgi?action=list&group=root.HTTPS.AllowTLS1,root.HTTPS.AllowTLS11,root.Network.SSH.Enabled,"
		"root.Network.Bonjour.Enabled,root.Network.UPnP.Enabled,root.WebService.DiscoveryMode.Discoverable,root.Network.ZeroConf.Enabled,root.System.BoaGroupPolicy.admin");
	if (x) {
		s.ok = TRUE;
		int t1 = param_on(x, "root.HTTPS.AllowTLS1"), t11 = param_on(x, "root.HTTPS.AllowTLS11");
		s.oldTls = t1 < 0 && t11 < 0 ? -1 : (t1 == 1 || t11 == 1);
		s.ssh = param_on(x, "root.Network.SSH.Enabled");
		s.bonjour = param_on(x, "root.Network.Bonjour.Enabled");
		s.upnp = param_on(x, "root.Network.UPnP.Enabled");
		s.wsd = param_on(x, "root.WebService.DiscoveryMode.Discoverable");
		s.zeroconf = param_on(x, "root.Network.ZeroConf.Enabled");
		const char* b = strstr(x, "root.System.BoaGroupPolicy.admin=");          /* fallback: http, https or both */
		if (b) { b += strlen("root.System.BoaGroupPolicy.admin="); s.access = !strncmp(b, "both", 4) ? 3 : !strncmp(b, "https", 5) ? 2 : !strncmp(b, "http", 4) ? 1 : 0; }
	}
	free(x);
	/* The web server's own hosts: plain HTTP and HTTPS, each active or not (the camera's "Allow access through") */
	x = ACAP_VAPIX_Get_Background("../config/rest/virtualhost/v1");
	cJSON* j = x ? cJSON_Parse(x) : NULL;
	free(x);
	cJSON* hs = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "data"), "defaulthosts");
	if (cJSON_IsArray(hs)) {
		int http = 0, https = 0;
		cJSON* h;
		cJSON_ArrayForEach(h, hs) if (cJSON_IsTrue(cJSON_GetObjectItem(h, "active"))) { if (cJSON_IsTrue(cJSON_GetObjectItem(h, "secure"))) https = 1; else http = 1; }
		if (http || https) s.access = (http ? 1 : 0) | (https ? 2 : 0);
	}
	cJSON_Delete(j);
	x = ACAP_VAPIX_Get_Background("applications/config.cgi?action=get&name=AllowUnsigned");
	s.unsignedOk = !x ? -1 : strstr(x, "value=\"true\"") ? 1 : strstr(x, "value=\"false\"") ? 0 : -1;
	free(x);
	g_mutex_lock(&slowLock);
	camSec = s;
	g_mutex_unlock(&slowLock);
}
static void add_onoff(cJSON* o, const char* key, int v) { if (v >= 0) cJSON_AddBoolToObject(o, key, v); }

/* The camera endpoint: streams, disk health and recording space, the audit log events, time sync and security */
cJSON* sampler_camera_json(void) {
	cJSON* out = cJSON_CreateObject();
	g_mutex_lock(&slowLock);
	if (camTime.ok) {
		cJSON* o = cJSON_AddObjectToObject(out, "time");
		cJSON_AddBoolToObject(o, "enabled", camTime.enabled);
		cJSON_AddBoolToObject(o, "synced", camTime.synced);
		if (!isnan(camTime.nextS)) {
			double left = camTime.nextS - (double)(g_get_monotonic_time() - camTime.readAt) / G_USEC_PER_SEC;
			cJSON_AddNumberToObject(o, "nextSync", left > 0 ? left : 0);
		}
		add_num(o, "offsetMs", camTime.offsetMs);
		cJSON_AddStringToObject(o, "source", camTime.source);
		cJSON* a = cJSON_AddArrayToObject(o, "servers");
		gchar** sv = g_strsplit(camTime.servers, ", ", 16);
		for (int i = 0; sv && sv[i]; i++) if (*sv[i]) cJSON_AddItemToArray(a, cJSON_CreateString(sv[i]));
		g_strfreev(sv);
	}
	if (camSec.ok) {
		cJSON* o = cJSON_AddObjectToObject(out, "security");
		if (camSec.access) cJSON_AddStringToObject(o, "access", camSec.access == 3 ? "both" : camSec.access == 2 ? "https" : "http");
		add_onoff(o, "oldTls", camSec.oldTls);
		add_onoff(o, "ssh", camSec.ssh);
		add_onoff(o, "unsigned", camSec.unsignedOk);
		cJSON* a = cJSON_AddArrayToObject(o, "discovery");
		const char* names[4] = { "Bonjour\xC2\xAE", "UPnP\xC2\xAE", "WS-Discovery", "ZeroConf" };
		int vals[4] = { camSec.bonjour, camSec.upnp, camSec.wsd, camSec.zeroconf };
		for (int i = 0; i < 4; i++) if (vals[i] >= 0) { cJSON* d = cJSON_CreateObject(); cJSON_AddStringToObject(d, "name", names[i]); cJSON_AddBoolToObject(d, "on", vals[i]); cJSON_AddItemToArray(a, d); }
	}
	if (streamsOk) {
		cJSON* a = cJSON_AddArrayToObject(out, "streams");
		for (int i = 0; i < streamN; i++) {
			const StreamInfo* s = &streams[i];
			cJSON* o = cJSON_CreateObject();
			cJSON_AddNumberToObject(o, "id", s->id);
			cJSON_AddStringToObject(o, "direction", s->direction); cJSON_AddStringToObject(o, "state", s->state);
			cJSON_AddStringToObject(o, "media", s->media); cJSON_AddStringToObject(o, "mime", s->mime);
			cJSON_AddBoolToObject(o, "encrypted", s->encrypted); cJSON_AddBoolToObject(o, "multicast", s->multicast);
			cJSON_AddStringToObject(o, "transport", s->transport); cJSON_AddStringToObject(o, "protocol", s->proto);
			cJSON_AddStringToObject(o, "src", s->src); cJSON_AddNumberToObject(o, "srcPort", s->srcPort);
			cJSON_AddStringToObject(o, "dst", s->dst); cJSON_AddNumberToObject(o, "dstPort", s->dstPort);
			cJSON_AddStringToObject(o, "agent", s->agent); cJSON_AddStringToObject(o, "path", s->path);
			cJSON_AddStringToObject(o, "resolution", s->res); cJSON_AddStringToObject(o, "codec", s->codec);
			cJSON_AddStringToObject(o, "fps", s->fps); cJSON_AddStringToObject(o, "camera", s->cam);
			cJSON_AddItemToArray(a, o);
		}
	}
	if (cdiskN) {
		cJSON* a = cJSON_AddArrayToObject(out, "disks");
		for (int i = 0; i < cdiskN; i++) {
			const CamDisk* d = &cdisks[i];
			cJSON* o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "id", d->id);
			add_num(o, "wear", d->wear); add_num(o, "tempC", d->tempC);
			if (d->health >= 0) cJSON_AddNumberToObject(o, "health", d->health);
			if (d->limitKb >= 0) add_num(o, "recLimitKb", d->limitKb);
			if (d->usedKb >= 0) add_num(o, "recUsedKb", d->usedKb);
			cJSON_AddItemToArray(a, o);
		}
	}
	if (eventsOk) {
		if (eventsClearedAt) cJSON_AddNumberToObject(out, "eventsClearedAt", (double)eventsClearedAt);
		cJSON* a = cJSON_AddArrayToObject(out, "events");
		for (int i = 0; i < eventN; i++) {
			cJSON* o = cJSON_CreateObject();
			cJSON_AddNumberToObject(o, "t", (double)events[i].t);
			cJSON_AddStringToObject(o, "kind", events[i].kind);
			if (events[i].user[0]) cJSON_AddStringToObject(o, "user", events[i].user);
			if (events[i].from[0]) cJSON_AddStringToObject(o, "from", events[i].from);
			if (events[i].service[0]) cJSON_AddStringToObject(o, "service", events[i].service);
			if (events[i].area[0]) cJSON_AddStringToObject(o, "area", events[i].area);
			if (events[i].what[0]) cJSON_AddStringToObject(o, "what", events[i].what);
			cJSON_AddItemToArray(a, o);
		}
	}
	g_mutex_unlock(&slowLock);
	return out;
}

static gpointer slow_thread(gpointer user_data) {
	(void)user_data;
	gint64 next[3] = { 0, 0, 0 };
	int fails[3] = { 0, 0, 0 };
	gboolean everOk[3] = { FALSE, FALSE, FALSE };      /* a source that answered once is never put on the hourly ask */
	g_usleep(2 * G_USEC_PER_SEC);                    /* let the first reading say whether the SD card is visible */
	for (;;) {
		Slow r;
		g_mutex_lock(&slowLock);
		r = slow;
		gboolean wantSd = slowWantSd;
		g_mutex_unlock(&slowLock);
		gint64 t = g_get_monotonic_time();
		SamplerSettings c = cfg_get();
		for (int s = 0; s < 3; s++) {
			if (t < next[s]) continue;
			/* A group switched off in Settings is not asked at all; it is asked at once when switched on again */
			gboolean want = s == 0 ? (c.collect & G_TEMP) != 0 : (c.collect & G_POWER) && c.powerSource != PS_OFF;
			if (!want) {
				if (s == 0) { r.tempOk = r.fanOk = r.heaterOk = FALSE; r.vsN = 0; }
				else if (s == 1) r.powerOk = FALSE;
				else { r.poeOk = FALSE; r.portsN = 0; }
				next[s] = 0;
				continue;
			}
			/* Read into a copy: a source that stops answering keeps its last good values through 2 failed asks */
			Slow tmp = r;
			gboolean ok = s == 0 ? poll_temp(&tmp) : s == 1 ? poll_power(&tmp) : poll_poe(&tmp);
			fails[s] = ok ? 0 : fails[s] + 1;
			if (ok || fails[s] > 2 || !everOk[s]) r = tmp;
			everOk[s] = everOk[s] || ok;
			gboolean hourly = fails[s] >= 2 && !everOk[s];
			if (hourly && fails[s] == 2) LOG("%s: no answer twice, asking once an hour from now\n", s == 0 ? "Temperature service" : s == 1 ? "Power" : "PoE ports");
			/* From the start of this round, so the next round (30 s on) is due; an hour once it never answered */
			next[s] = t + (gint64)(hourly ? 3600 : SLOW_EVERY) * G_USEC_PER_SEC;
		}
		/* What the camera itself reports: streams every 30 s, its audit log every 60 s, disk health and recording space every 5 min */
		static int loopN = 0;
		if (c.collect & G_CONNS) poll_streams();
		if (loopN % 2 == 0 && (c.collect & G_EVENTS)) poll_audit();
		if (loopN % 10 == 0 && (c.collect & G_STORAGE)) poll_disks();
		poll_time();                                     /* Overview: Time sync every 30 s (the page counts down between), Security every 60 s */
		if (loopN % 2 == 0) poll_security();
		loopN++;
		r.sdTotalKb = r.sdFreeKb = -1;
		if (wantSd) {
			/* The app may not see the card's folder: ask the camera's disk list (sizes in kB) */
			char* x = ACAP_VAPIX_Get_Background("disks/list.cgi?diskid=SD_DISK");
			const char* d = x ? strstr(x, "diskid=\"SD_DISK\"") : NULL;
			const char* ts = d ? strstr(d, "totalsize=\"") : NULL;
			const char* fs = d ? strstr(d, "freesize=\"") : NULL;
			const char* st = d ? strstr(d, "status=\"") : NULL;
			if (ts && fs && st && strncmp(st + 8, "OK", 2) == 0 && atof(ts + 11) > 0) { r.sdTotalKb = atof(ts + 11); r.sdFreeKb = atof(fs + 10); }
			free(x);
		}
		g_mutex_lock(&slowLock);
		slow = r;
		g_mutex_unlock(&slowLock);
		g_usleep((gulong)SLOW_EVERY * G_USEC_PER_SEC);
	}
	return NULL;
}

/* Network drives get a thread of their own: a NAS that went away can hang statvfs, and then only this stops */
static gpointer nas_thread(gpointer user_data) {
	(void)user_data;
	for (;;) {
		char paths[MAX_MNT][160];
		g_mutex_lock(&slowLock);
		memcpy(paths, slowNetPath, sizeof(paths));
		g_mutex_unlock(&slowLock);
		for (int k = 0; k < MAX_MNT; k++) {
			double t = -1, f = -1;
			if (paths[k][0] && !fs_use(paths[k], &t, &f)) t = f = -1;
			g_mutex_lock(&slowLock);
			nasTotalKb[k] = t; nasFreeKb[k] = f;
			g_mutex_unlock(&slowLock);
		}
		g_usleep((gulong)SLOW_EVERY * G_USEC_PER_SEC);
	}
	return NULL;
}

/*----------------------------------------------------- apps you installed vs Axis's own apps */

/* Packages made by Axis (from the camera's app list, replaced on each poll; main loop only). Their processes are the
 * camera's: not in "apps you installed" totals or per-core lines. */
static GHashTable* axisApps = NULL;
void sampler_set_axis_apps(GPtrArray* ids) {
	GHashTable* t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint i = 0; ids && i < ids->len; i++) g_hash_table_add(t, g_strdup(g_ptr_array_index(ids, i)));
	if (ids) g_ptr_array_free(ids, TRUE);
	if (axisApps) g_hash_table_destroy(axisApps);
	axisApps = t;
}
static gboolean own_app(const char* app) { return app && app[0] && !(axisApps && g_hash_table_contains(axisApps, app)); }

/*----------------------------------------------------- processes */

static int by_cpu(const void* a, const void* b) {
	const ProcSum* x = a; const ProcSum* y = b;
	if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
	return x->memKb < y->memKb ? 1 : (x->memKb > y->memKb ? -1 : 0);
}
static int by_mem(const void* a, const void* b) {
	const ProcSum* x = a; const ProcSum* y = b;
	if (x->memKb != y->memKb) return x->memKb < y->memKb ? 1 : -1;
	return x->cpu < y->cpu ? 1 : (x->cpu > y->cpu ? -1 : 0);
}

static gboolean not_seen(gpointer k, gpointer v, gpointer u) { (void)k; (void)u; return !((PidInfo*)v)->seen; }

/* An app's CPU per core: each of its threads' CPU time since the last scan, counted on the core
 * the thread last ran on. Threads move between cores, so it is close, not exact. */
typedef struct { guint64 ticks; int scan; } TidPrev;
static int scanNo = 0;
static void app_threads(int pid, double* perCore) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task", pid);
	GDir* d = g_dir_open(path, 0, NULL);
	const gchar* e;
	while (d && (e = g_dir_read_name(d))) {
		int tid = atoi(e);
		char p2[80], buf[1024];
		snprintf(p2, sizeof(p2), "/proc/%d/task/%d/stat", pid, tid);
		FILE* f = fopen(p2, "r");
		if (!f) continue;
		gboolean got = fgets(buf, sizeof(buf), f) != NULL;
		fclose(f);
		char* r = got ? strrchr(buf, ')') : NULL;
		if (!r) continue;
		/* Fields after the name: 3 state ... 14 utime, 15 stime ... 39 processor */
		unsigned long long ut = 0, st = 0;
		int cpu = -1, field = 3;
		char* save = NULL;
		for (char* t = strtok_r(r + 2, " ", &save); t; t = strtok_r(NULL, " ", &save), field++) {
			if (field == 14) ut = strtoull(t, NULL, 10);
			else if (field == 15) st = strtoull(t, NULL, 10);
			else if (field == 39) { cpu = atoi(t); break; }
		}
		guint64 ticks = ut + st;
		TidPrev* tp = g_hash_table_lookup(tids, GINT_TO_POINTER(tid));
		if (tp && ticks >= tp->ticks && cpu >= 0 && cpu < MAX_CORES) perCore[cpu] += (double)(ticks - tp->ticks);
		if (!tp) { tp = g_new0(TidPrev, 1); g_hash_table_insert(tids, GINT_TO_POINTER(tid), tp); }
		tp->ticks = ticks; tp->scan = scanNo;
	}
	if (d) g_dir_close(d);
}
static gboolean tid_old(gpointer k, gpointer v, gpointer u) { (void)k; (void)u; return ((TidPrev*)v)->scan != scanNo; }
static void clear_seen(gpointer k, gpointer v, gpointer u) { (void)k; (void)u; ((PidInfo*)v)->seen = FALSE; }

/* All processes, added together by name. CPU is the share of the whole camera since the last scan. */
static GArray* scan_processes(unsigned long long total, double uptime, int* procs, int* threadsOut, double* appCoreTicks) {
	scanNo++;
	double dt = prevScanTotal && total > prevScanTotal ? (double)(total - prevScanTotal) : 0;
	prevScanTotal = total;
	GHashTable* byName = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GArray* sums = g_array_new(FALSE, TRUE, sizeof(ProcSum));
	g_hash_table_foreach(pids, clear_seen, NULL);
	int seen = 0, threads = 0;
	GDir* d = g_dir_open("/proc", 0, NULL);
	const gchar* e;
	while (d && (e = g_dir_read_name(d))) {
		if (!isdigit((unsigned char)e[0])) continue;
		int pid = atoi(e);
		char path[64], buf[1024];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE* f = fopen(path, "r");
		if (!f) continue;
		gboolean got = fgets(buf, sizeof(buf), f) != NULL;
		fclose(f);
		char* l = got ? strchr(buf, '(') : NULL;
		char* r = got ? strrchr(buf, ')') : NULL;
		unsigned long ut = 0, st = 0;
		long thr = 0;
		unsigned long long start = 0;
		char state = 0;
		if (!l || !r || r < l || sscanf(r + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %ld %*d %llu", &state, &ut, &st, &thr, &start) != 5) continue;
		seen++;
		threads += (int)thr;
		PidInfo* pi = g_hash_table_lookup(pids, GINT_TO_POINTER(pid));
		if (!pi || pi->start != start) {
			/* New process: its full name and its app, from the command line (read once) */
			pi = g_new0(PidInfo, 1);
			pi->start = start;
			size_t n = (size_t)(r - l - 1);
			if (n >= sizeof(pi->name)) n = sizeof(pi->name) - 1;
			memcpy(pi->name, l + 1, n);
			snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
			f = fopen(path, "r");
			if (f) {
				char cmd[256];
				size_t k = fread(cmd, 1, sizeof(cmd) - 1, f);
				fclose(f);
				cmd[k] = 0;
				if (strlen(pi->name) == 15 && k > 0) {        /* names are cut to 15 letters */
					const char* base = strrchr(cmd, '/');
					base = base ? base + 1 : cmd;
					if (strncmp(base, pi->name, 15) == 0 && strlen(base) > 15) g_strlcpy(pi->name, base, sizeof(pi->name));
				}
				const char* m = strstr(cmd, "/usr/local/packages/");
				if (m) {
					m += strlen("/usr/local/packages/");
					size_t j = 0;
					while (m[j] && m[j] != '/' && j < sizeof(pi->app) - 1) { pi->app[j] = m[j]; j++; }
					pi->app[j] = 0;
				}
			}
			char* at = strchr(pi->name, '@');             /* vipd@0, vipd@1 ... count as one */
			if (at && at != pi->name) *at = 0;
			char* sl = strchr(pi->name, '/');             /* kernel threads: kworker/0:1, irq/45-x ... count as one */
			if (sl && sl != pi->name) *sl = 0;
			pi->prevTicks = (guint64)ut + st;
			g_hash_table_replace(pids, GINT_TO_POINTER(pid), pi);
		}
		pi->seen = TRUE;
		if (own_app(pi->app)) app_threads(pid, appCoreTicks);   /* apps you installed, per core */
		guint64 ticks = (guint64)ut + st;
		ProcSum p;
		memset(&p, 0, sizeof(p));
		g_strlcpy(p.name, pi->name, sizeof(p.name));
		g_strlcpy(p.app, pi->app, sizeof(p.app));
		p.count = 1;
		p.dCount = state == 'D';                     /* waiting on the disk (uninterruptible) right now */
		p.threads = (int)thr;
		p.ageS = uptime - (double)start / (double)hz;
		if (dt > 0 && ticks >= pi->prevTicks) p.cpu = 100.0 * (double)(ticks - pi->prevTicks) / dt;
		pi->prevTicks = ticks;
		/* Private memory (resident minus shared), so shared libraries are not counted again and again */
		snprintf(path, sizeof(path), "/proc/%d/statm", pid);
		f = fopen(path, "r");
		if (f) {
			long size = 0, res = 0, sh = 0;
			if (fscanf(f, "%ld %ld %ld", &size, &res, &sh) == 3 && res > sh) p.memKb = (double)(res - sh) * pageKb;
			fclose(f);
		}
		gpointer idx;
		if (g_hash_table_lookup_extended(byName, p.name, NULL, &idx)) {
			ProcSum* s = &g_array_index(sums, ProcSum, GPOINTER_TO_UINT(idx));
			s->count++; s->dCount += p.dCount; s->cpu += p.cpu; s->memKb += p.memKb; s->threads += p.threads;
			if (p.ageS > s->ageS) s->ageS = p.ageS;
			if (!s->app[0] && p.app[0]) g_strlcpy(s->app, p.app, sizeof(s->app));
		} else {
			g_array_append_val(sums, p);
			g_hash_table_insert(byName, g_strdup(p.name), GUINT_TO_POINTER(sums->len - 1));
		}
	}
	if (d) g_dir_close(d);
	g_hash_table_destroy(byName);
	g_hash_table_foreach_remove(pids, not_seen, NULL);
	g_hash_table_foreach_remove(tids, tid_old, NULL);
	g_array_sort(sums, by_cpu);
	*procs = seen;
	*threadsOut = threads;
	return sums;
}

/* Keep the top 10 of a scan in a history point (lock held) */
static void keep_top(GArray* list, TopEntry* top, guint8* n, float* other, gboolean mem) {
	*n = 0; *other = 0;
	for (guint i = 0; i < list->len; i++) {
		ProcSum* p = &g_array_index(list, ProcSum, i);
		float v = (float)(mem ? p->memKb : p->cpu);
		if (i < TOPN) { top[i].name = name_id(p->name); top[i].owner = name_id(p->app); top[i].val = v; (*n)++; }
		else *other += v;
	}
}

/* Keep the busiest connections in a history point (lock held). The rest of the Ethernet total
 * is "everything else": UDP, connections that closed, and traffic between the readings. */
/* Add seconds blocked on the disk to a small list by name; a full list replaces its smallest entry when this is bigger */
static void io_add(TopEntry* t, int* n, int cap, guint16 name, guint16 owner, float sec) {
	for (int i = 0; i < *n; i++) if (t[i].name == name) { t[i].val += sec; return; }
	if (*n < cap) { t[*n].name = name; t[*n].owner = owner; t[*n].val = sec; (*n)++; return; }
	int m = 0;
	for (int i = 1; i < *n; i++) if (t[i].val < t[m].val) m = i;
	if (t[m].val < sec) { t[m].name = name; t[m].owner = owner; t[m].val = sec; }
}

static void keep_net(GArray* l, HistPoint* p, float total) {
	p->netN = 0;
	float sum = 0;
	for (guint i = 0; i < l->len && i < TOPN; i++) {
		NetUse* u = &g_array_index(l, NetUse, i);
		if (u->tx + u->rx <= 0) break;
		gchar* label = g_strdup_printf("%s · %s", u->service, u->peer);
		p->netTop[i].name = name_id(label); p->netTop[i].owner = 0; p->netTop[i].val = (float)(u->tx + u->rx);
		g_free(label);
		sum += p->netTop[i].val;
		p->netN++;
	}
	p->netOther = isnan(total) || total < sum ? 0 : total - sum;
}

static void remap(TopEntry* t, int n, guint16* map, GPtrArray* keep) {
	for (int i = 0; i < n; i++) {
		guint16* ids[2] = { &t[i].name, &t[i].owner };
		for (int k = 0; k < 2; k++) {
			guint16 old = *ids[k];
			if (!old) continue;
			if (!map[old]) { map[old] = (guint16)keep->len; g_ptr_array_add(keep, g_strdup(name_of(old))); }
			*ids[k] = map[old];
		}
	}
}
static void prune_names(void) {
	guint before = names->len;
	guint16* map = g_new0(guint16, names->len);
	GPtrArray* keep = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(keep, g_strdup(""));
	for (int i = 0; i < histCount; i++) {
		HistPoint* p = &hist[(histHead - 1 - i + HIST_N) % HIST_N];
		remap(p->cpuTop, p->cpuN, map, keep); remap(p->memTop, p->memN, map, keep); remap(p->netTop, p->netN, map, keep); remap(p->ioTop, p->ioN, map, keep);
	}
	remap(cur.cpuTop, cur.cpuN, map, keep); remap(cur.memTop, cur.memN, map, keep); remap(cur.netTop, cur.netN, map, keep); remap(cur.ioTop, cur.ioN, map, keep);
	for (int i = 0; i < LIVE_N; i++) {
		LiveTop* l = &liveTop[i];
		if (!l->seq) continue;
		remap(l->cpu, l->cpuN, map, keep); remap(l->mem, l->memN, map, keep); remap(l->net, l->netN, map, keep); remap(l->io, l->ioN, map, keep);
	}
	g_free(map);
	g_hash_table_remove_all(nameIdx);
	g_ptr_array_free(names, TRUE);
	names = keep;
	for (guint i = 1; i < names->len; i++) g_hash_table_insert(nameIdx, g_ptr_array_index(names, i), GUINT_TO_POINTER(i));
	LOG("Name list pruned: %u -> %u names\n", before, names->len);
}

/*----------------------------------------------------- the tick */

void sampler_init(void) {
	g_mutex_init(&lock);
	pageKb = sysconf(_SC_PAGESIZE) / 1024;
	hz = sysconf(_SC_CLK_TCK);
	if (hz <= 0) hz = 100;
	hist = g_new0(HistPoint, HIST_N);
	names = g_ptr_array_new_with_free_func(g_free);
	nameIdx = g_hash_table_new(g_str_hash, g_str_equal);   /* keys are owned by `names` */
	g_ptr_array_add(names, g_strdup(""));               /* index 0: no name, or the camera itself */
	pids = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	tids = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	scan = g_array_new(FALSE, TRUE, sizeof(ProcSum));
	memset(&cur, 0, sizeof(cur));
	cur.cpu = cur.apps = cur.mem = cur.flash = cur.sd = cur.tx = cur.rx = cur.temp = NAN;
	cur.bestCpu = -1;
	cur.bestNet = -1;
	/* 1.1: history columns, the lists' first members, and the slow sources' threads */
	histX = g_new(guint16, (gsize)HIST_N * X_N);
	for (gsize i = 0; i < (gsize)HIST_N * X_N; i++) histX[i] = 0x7E00;
	x_clear(curX);
	long np = sysconf(_SC_NPROCESSORS_CONF);
	coreHN = np < 1 ? 1 : np > CW_MAX ? CW_MAX : (int)np;
	histC = g_new(guint16, (gsize)HIST_N * coreHN * CW_F);
	for (gsize i = 0; i < (gsize)HIST_N * coreHN * CW_F; i++) histC[i] = 0x7E00;
	for (int q = 0; q < CW_MAX * CW_F; q++) curC[q] = NAN;
	g_mutex_init(&slowLock);
	g_mutex_init(&phLock);
	g_mutex_init(&cfgLock);
	g_mutex_init(&storeLock);
	memset(&slow, 0, sizeof(slow));
	slow.sdTotalKb = slow.sdFreeKb = -1;
	slow.fanRpm = slow.powerW = slow.budgetW = slow.powerAvgW = slow.powerMaxW = slow.powerReqW = NAN;
	for (int i = 0; i < MAX_MNT; i++) nasTotalKb[i] = nasFreeKb[i] = -1;
	struct stat st;
	if (stat(".", &st) == 0) { appDev = st.st_dev; appDevOk = TRUE; }
	startMono = g_get_monotonic_time();
	startUnix = (gint64)time(NULL);
	memset(&now, 0, sizeof(now));
	now.videoTotalKb = now.videoUsedKb = -1;
	now.mhz = now.powerW = now.budgetW = now.powerAvgW = now.powerMaxW = now.powerReqW = NAN;
	find_thermal(&now);
	find_mhz();
	log_dlpu_candidates();
	g_thread_unref(g_thread_new("slow-sources", slow_thread, NULL));
	g_thread_unref(g_thread_new("network-drives", nas_thread, NULL));
}

/* After each history point: each sensor's 24 h high, and each interface's errors and drops in the last hour (lock held) */
static void hist_summaries(void) {
	float hi[MAX_SENS], ed[MAX_IF][2];
	for (int i = 0; i < MAX_SENS; i++) hi[i] = NAN;
	memset(ed, 0, sizeof(ed));
	for (int k = 0; k < histCount; k++) {
		const guint16* h = &histX[(size_t)((histHead - 1 - k + HIST_N) % HIST_N) * X_N];
		for (int i = 0; i < now.sensN; i++) hi[i] = fmaxn(hi[i], h2f(h[X_TEMP0 + i]));
		if (k >= 360) continue;                         /* the last hour */
		for (int i = 0; i < now.ifN; i++) {
			const guint16* q = &h[X_NET0 + i * NET_F];
			float e = h2f(q[4]) + h2f(q[6]), d = h2f(q[5]) + h2f(q[7]);
			if (!isnan(e)) ed[i][0] += e;
			if (!isnan(d)) ed[i][1] += d;
		}
	}
	for (int i = 0; i < now.sensN; i++) now.sens[i].high24 = hi[i];
	for (int i = 0; i < now.ifN; i++) { now.ifs[i].errorsHour = ed[i][0]; now.ifs[i].dropsHour = ed[i][1]; }
}

/* 1.5.0: how long the heater has been on, or when it was last on (lock held, after each reading). The first time its
 * state is known, the history (read back from the card or flash after a restart) says how long it already was. */
static void heater_track(Now* n, gint64 t) {
	static int known = 0, was = 0;
	if (!n->heaterOk) { known = 0; n->heatSince = 0; return; }
	int on = n->heaterOn ? 1 : 0;
	if (!known) {
		if (tickN < 3 || !histSettled) return;              /* once history is read back (a late card: up to 2 min) */
		known = 1;
		n->heatSince = n->heatLastOff = n->heatLastFor = 0;
		gint64 since = t, end = 0, start = 0, prev = t;
		for (int k = 0; k < histCount; k++) {              /* newest first; a gap (the app was not running) ends a run */
			int slot = (histHead - 1 - k + HIST_N) % HIST_N;
			float h = h2f(histX[(size_t)slot * X_N + X_HEAT]);
			gboolean pOn = !isnan(h) && h > 0.5f, gap = prev - hist[slot].t > 2 * HIST_STEP;
			prev = hist[slot].t;
			if (on) { if (!pOn || gap) break; since = hist[slot].t - HIST_STEP; continue; }
			if (!end) { if (pOn) { end = hist[slot].t; start = end - HIST_STEP; } continue; }
			if (!pOn || gap) break;
			start = hist[slot].t - HIST_STEP;
		}
		if (on) n->heatSince = since;
		else if (end) { n->heatLastOff = end; n->heatLastFor = end - start; }
		was = on;
		return;
	}
	if (on && !was) n->heatSince = t;
	if (!on && was) { n->heatLastOff = t; n->heatLastFor = n->heatSince ? t - n->heatSince : 0; n->heatSince = 0; }
	was = on;
}

/*----------------------------------------------------- settings, and history on the SD card (1.3.0) */
/* With an SD card this app can write, history is kept on the card in three fixed-size ring files: 10 s points for
 * 24 h, 1 min points for 7 days and 5 min points for 30 days (only the ones the chosen "Keep history" needs). Each
 * file is set aside in full when made, so nothing grows; 10 s points are written six at a time (once a minute).
 * The card folder must not be on the app's own device. 1.5.0: without a card, the 10 s ring (24 h, about 2.2 MB) is
 * kept in the app's own storage (localdata, the built-in flash), so history survives restarts; 7 and 30 days stay
 * SD-only. If that file can't be made or written, history is memory only, with the reason, tried again after an hour. */
#define TIERS    3
#define CARD_DIR SD_PATH "/areas/resource_monitor"
static const guint32 TIER_STEP[TIERS] = { 10, 60, 300 }, TIER_CAP[TIERS] = { 8640, 10080, 8640 };
static const int TIER_BATCH[TIERS] = { 6, 1, 1 };
static const char* TIER_FILE[TIERS] = { "history-10s.ring", "history-1min.ring", "history-5min.ring" };
typedef struct { guint32 t; guint16 b[8]; guint16 x[X_N]; guint16 c[CW_MAX * CW_F]; } HistRec;   /* b: cpu apps mem flash sd tx rx temp */
typedef struct { float b[8], x[X_N], c[CW_MAX * CW_F]; int n; guint32 t; } TierAcc;
static Ring ring[TIERS];
static gboolean ringOn[TIERS];
static int ringWhere[TIERS];                /* 1 the SD card, 2 the built-in flash (10 s ring only) */
#define FLASH_RING "localdata/history-10s.ring"
static char flashWhy[200] = "";             /* why the flash ring isn't used ("" when fine) */
static gint64 flashFailAt = 0;              /* monotonic µs it last failed; tried again an hour later */
static TierAcc tacc[TIERS];                 /* 1 min and 5 min points being built (main loop, under lock) */
static int cardState = 3;                   /* 0 no card, 1 a card this app can write, 2 a card it can't, 3 not checked yet */
static char cardWhy[200] = "";
static gint64 cardAt = 0;

static void tacc_clear(TierAcc* a) {
	for (int i = 0; i < 8; i++) a->b[i] = NAN;
	x_clear(a->x);
	for (int i = 0; i < CW_MAX * CW_F; i++) a->c[i] = NAN;
	a->n = 0; a->t = 0;
}
/* A point into a longer one: peaks, counts added up, drive space the latest */
static void tacc_fold(TierAcc* a, const float* b, const float* x, const float* c, guint32 t) {
	for (int i = 0; i < 8; i++) a->b[i] = i == 3 || i == 4 ? (isnan(b[i]) ? a->b[i] : b[i]) : fmaxn(a->b[i], b[i]);
	x_fold(a->x, x);
	for (int i = 0; i < CW_MAX * CW_F; i++) a->c[i] = fmaxn(a->c[i], c[i]);
	a->n++; a->t = t;
}
static void rec_make(HistRec* r, const TierAcc* a) {
	memset(r, 0, sizeof(*r));
	r->t = a->t;
	for (int i = 0; i < 8; i++) r->b[i] = f2h(a->b[i]);
	for (int i = 0; i < X_N; i++) r->x[i] = f2h(a->x[i] / x_scale(i));
	for (int i = 0; i < CW_MAX * CW_F; i++) r->c[i] = f2h(a->c[i]);
}
static guint32 store_layout(void) {                 /* a file made for another build's point layout is made again */
	guint32 h = 2166136261u, v[5] = { X_N, CW_MAX, CW_F, (guint32)sizeof(HistRec), 1 };
	for (int i = 0; i < 5; i++) { h ^= v[i]; h *= 16777619u; }
	return h;
}

/* Is there a card folder this app can write? (main loop, at most once a minute) */
static void card_check(void) {
	struct stat st;
	int was = cardState;
	if (!sd_present()) { cardState = 0; g_strlcpy(cardWhy, "No SD card in the camera.", sizeof(cardWhy)); }
	else if (stat(CARD_DIR, &st) != 0 && mkdir(CARD_DIR, 0700) != 0) {
		cardState = 2; g_snprintf(cardWhy, sizeof(cardWhy), "The SD card is there, but the camera gives this app no folder on it (%s).", CARD_DIR);
	} else if (stat(CARD_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
		cardState = 2; g_strlcpy(cardWhy, "The app's folder on the SD card can't be read.", sizeof(cardWhy));
	} else if (appDevOk && st.st_dev == appDev) {
		cardState = 2; g_strlcpy(cardWhy, "That folder is on the built-in flash, not the card: history is never written to flash.", sizeof(cardWhy));
	} else {
		gchar* p = g_build_filename(CARD_DIR, ".write-test", NULL);
		FILE* f = fopen(p, "w");
		gboolean ok = f && fputc('x', f) != EOF;
		if (f && fclose(f) != 0) ok = FALSE;
		unlink(p);
		g_free(p);
		if (ok) { cardState = 1; cardWhy[0] = 0; }
		else { cardState = 2; g_strlcpy(cardWhy, "The SD card is read-only or full.", sizeof(cardWhy)); }
	}
	if (cardState != was) LOG("History: %s\n", cardState == 1 ? "SD card folder " CARD_DIR " can be written" : cardWhy);
}

/* Put the 10 s points kept on the card back into memory, at start (main loop) */
typedef struct { guint32 since, before; } LoadCtx;     /* points from since, older than before (0: any) */
static void load_cb(const void* rp, void* u) {
	const HistRec* r = rp;
	if (r->t < ((LoadCtx*)u)->since || (((LoadCtx*)u)->before && r->t >= ((LoadCtx*)u)->before)) return;
	HistPoint* p = &hist[histHead];
	memset(p, 0, sizeof(*p));
	p->t = r->t;
	p->cpu = h2f(r->b[0]); p->apps = h2f(r->b[1]); p->mem = h2f(r->b[2]); p->flash = h2f(r->b[3]);
	p->sd = h2f(r->b[4]); p->tx = h2f(r->b[5]); p->rx = h2f(r->b[6]); p->temp = h2f(r->b[7]);
	p->bestCpu = -1; p->bestNet = -1;              /* the top 10 lists of points before a restart are not kept */
	memcpy(&histX[(size_t)histHead * X_N], r->x, sizeof(guint16) * X_N);
	memcpy(&histC[(size_t)histHead * coreHN * CW_F], r->c, sizeof(guint16) * (size_t)coreHN * CW_F);
	histHead = (histHead + 1) % HIST_N;
	if (histCount < HIST_N) histCount++;
}

/* Open the rings the settings and the card call for, close the others (main loop) */
/* 1.5.1: the SD card can be mounted a while after the app starts. Until the card's state is definite (a card this app
 * can write, or CARD_WAIT_S after start), history is not put on the built-in flash, and the card is checked every 10 s. */
#define CARD_WAIT_S 120
static void store_sync(const SamplerSettings* c, gboolean force) {
	gint64 tm = g_get_monotonic_time();
	gboolean settled = cardState == 1 || (cardState != 3 && tm - startMono > (gint64)CARD_WAIT_S * G_USEC_PER_SEC);
	if (!force && cardAt && tm - cardAt < (settled ? 60 : 10) * G_USEC_PER_SEC) return;
	cardAt = tm;
	card_check();
	settled = cardState == 1 || (cardState != 3 && tm - startMono > (gint64)CARD_WAIT_S * G_USEC_PER_SEC);
	int want = cardState == 1 ? (c->historyDays >= 30 ? 3 : c->historyDays >= 7 ? 2 : 1) : 0;
	gboolean load = FALSE;
	g_mutex_lock(&storeLock);
	gboolean flashOk = !flashFailAt || tm - flashFailAt > (gint64)3600 * G_USEC_PER_SEC;
	for (int i = 0; i < TIERS; i++) {
		/* where this tier goes: the card; or, for the 10 s ring without a card, the built-in flash */
		int where = i < want ? 1 : i == 0 && cardState != 1 && flashOk && settled ? 2 : 0;
		gboolean need = where != 0;
		if (ringOn[i] && ringWhere[i] != where) {
			ring_close(&ring[i]);
			ringOn[i] = FALSE;
			ringWhere[i] = 0;
			if (!need && i > 0 && cardState == 1) {     /* kept shorter now: what was kept longer is deleted */
				gchar* p = g_build_filename(CARD_DIR, TIER_FILE[i], NULL);
				unlink(p);
				g_free(p);
				LOG("History: %s deleted (Keep history is now shorter)\n", TIER_FILE[i]);
			}
		}
		if (!ringOn[i] && need) {
			gchar* p = where == 2 ? g_strdup(FLASH_RING) : g_build_filename(CARD_DIR, TIER_FILE[i], NULL);
			int made = 0;
			if (ring_open(&ring[i], p, TIER_STEP[i], TIER_CAP[i], sizeof(HistRec), store_layout(), TIER_BATCH[i], &made)) {
				ringOn[i] = TRUE;
				ringWhere[i] = where;
				if (where == 2) { flashWhy[0] = 0; flashFailAt = 0; }
				tacc_clear(&tacc[i]);
				if (i == 0 && !made) load = TRUE;
				LOG("History: %s %s (%.1f MB)\n", made ? "made" : "opened", p, ring_file_bytes(TIER_CAP[i], sizeof(HistRec)) / 1048576.0);
			} else {
				LOG("History: could not open %s: %s\n", p, g_strerror(errno));
				if (where == 2) {
					g_snprintf(flashWhy, sizeof(flashWhy), "The history file on the built-in flash could not be made (%s), so history is kept in memory only.", g_strerror(errno));
					flashFailAt = tm ? tm : 1;
				}
			}
			g_free(p);
		}
	}
	g_mutex_unlock(&storeLock);
	if (!load) { if (settled) histSettled = TRUE; return; }
	/* Into an empty history (at start); or, when every point in memory is from this run (the card came late), the
	 * points kept before them go in first. Never over points from an earlier load. */
	g_mutex_lock(&lock);
	int keep = histCount;
	gint64 oldest = keep ? hist[(histHead - keep + HIST_N) % HIST_N].t : 0;
	if (keep == 0 || (keep <= 360 && oldest >= startUnix)) {
		HistPoint* sp = keep ? g_new(HistPoint, keep) : NULL;
		guint16* sx = keep ? g_new(guint16, (gsize)keep * X_N) : NULL;
		guint16* sc = keep ? g_new(guint16, (gsize)keep * coreHN * CW_F) : NULL;
		for (int k = 0; k < keep; k++) {                  /* this run's points, oldest first */
			int slot = (histHead - keep + k + HIST_N) % HIST_N;
			sp[k] = hist[slot];
			memcpy(&sx[(size_t)k * X_N], &histX[(size_t)slot * X_N], sizeof(guint16) * X_N);
			memcpy(&sc[(size_t)k * coreHN * CW_F], &histC[(size_t)slot * coreHN * CW_F], sizeof(guint16) * (size_t)coreHN * CW_F);
		}
		histHead = histCount = 0;
		LoadCtx lc = { (guint32)(time(NULL) - (time_t)HIST_N * HIST_STEP), keep ? (guint32)oldest : 0 };
		ring_read(&ring[0], HIST_N - (guint32)keep, load_cb, &lc);
		int got = histCount;
		for (int k = 0; k < keep; k++) {                  /* then this run's points again, after them */
			hist[histHead] = sp[k];
			memcpy(&histX[(size_t)histHead * X_N], &sx[(size_t)k * X_N], sizeof(guint16) * X_N);
			memcpy(&histC[(size_t)histHead * coreHN * CW_F], &sc[(size_t)k * coreHN * CW_F], sizeof(guint16) * (size_t)coreHN * CW_F);
			histHead = (histHead + 1) % HIST_N;
			if (histCount < HIST_N) histCount++;
		}
		g_free(sp); g_free(sx); g_free(sc);
		if (got) { hist_summaries(); LOG("History: %d points read back from the %s\n", got, ringWhere[0] == 2 ? "built-in flash" : "card"); }
	}
	if (settled) histSettled = TRUE;
	g_mutex_unlock(&lock);
}

/* A finished 10 s point into the rings, and into the 1 min and 5 min points being built (main loop, lock held) */
static void store_point(const HistPoint* p, const float* x, const float* c) {
	float b[8] = { p->cpu, p->apps, p->mem, p->flash, p->sd, p->tx, p->rx, p->temp };
	float cc[CW_MAX * CW_F];
	for (int i = 0; i < CW_MAX * CW_F; i++) cc[i] = i < coreHN * CW_F ? c[i] : NAN;
	g_mutex_lock(&storeLock);
	for (int i = 0; i < TIERS; i++) {
		if (!ringOn[i]) continue;
		TierAcc* a = &tacc[i];
		if (i == 0) tacc_clear(a);
		tacc_fold(a, b, x, cc, (guint32)p->t);
		if (a->n * TIER_STEP[0] < TIER_STEP[i]) continue;
		HistRec r;
		rec_make(&r, a);
		if (!ring_add(&ring[i], &r)) {
			if (ringWhere[i] == 2) {                    /* the built-in flash: memory only from now, and say why */
				g_snprintf(flashWhy, sizeof(flashWhy), "Writing history to the built-in flash failed (%s), so history is kept in memory only.", g_strerror(errno));
				LOG("History: %s\n", flashWhy);
				ring_close(&ring[i]);
				ringOn[i] = FALSE;
				ringWhere[i] = 0;
				flashFailAt = g_get_monotonic_time();
			} else {
				static gint64 warned = 0;
				if (!warned || g_get_monotonic_time() - warned > (gint64)3600 * G_USEC_PER_SEC) { warned = g_get_monotonic_time(); LOG("History: could not write to the SD card: %s\n", g_strerror(errno)); }
			}
			cardAt = 0;                                 /* check the card again on the next reading */
		}
		tacc_clear(a);
	}
	g_mutex_unlock(&storeLock);
}

/* 1.4.0: groups the enabled alerts watch are read even when neither shown nor sent */
static unsigned alertNeed = 0, baseCollect = G_ALL;
void sampler_set_need(unsigned groups) {
	g_mutex_lock(&cfgLock);
	alertNeed = groups & G_ALL;
	cfg.collect = (baseCollect | alertNeed) | G_CPU | G_MEM;
	cfgDirty = TRUE;
	g_mutex_unlock(&cfgLock);
}

void sampler_apply(const SamplerSettings* s) {
	g_mutex_lock(&cfgLock);
	cfg = *s;
	if (cfg.historyDays != 7 && cfg.historyDays != 30) cfg.historyDays = 1;
	baseCollect = cfg.collect & G_ALL;
	cfg.collect = (baseCollect | alertNeed) | G_CPU | G_MEM;   /* CPU and memory are always read: every other figure needs them */
	cfgDirty = TRUE;
	g_mutex_unlock(&cfgLock);
}

int sampler_card_ok(void) { return cardState == 1; }

int sampler_power_log_known(void) {
	g_mutex_lock(&slowLock);
	int r = slow.powerCapKnown ? (slow.powerHist ? 1 : 0) : -1;
	g_mutex_unlock(&slowLock);
	return r;
}

cJSON* sampler_store_json(void) {
	SamplerSettings c = cfg_get();
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "card", cardState == 1 ? "ok" : cardState == 0 ? "none" : cardState == 2 ? "blocked" : "checking");
	if (cardWhy[0]) cJSON_AddStringToObject(o, "why", cardWhy);
	cJSON_AddNumberToObject(o, "days", c.historyDays);
	cJSON* t = cJSON_AddArrayToObject(o, "tiers");
	g_mutex_lock(&storeLock);
	/* 1.5.0: where the 24 h of 10 s points are: "card", "flash" (built-in, kept across restarts) or "memory" */
	cJSON_AddStringToObject(o, "where", ringOn[0] ? (ringWhere[0] == 2 ? "flash" : "card") : "memory");
	if (flashWhy[0]) cJSON_AddStringToObject(o, "flashWhy", flashWhy);
	cJSON_AddNumberToObject(o, "flashBytes", (double)ring_file_bytes(TIER_CAP[0], sizeof(HistRec)));
	for (int i = 0; i < TIERS; i++) {
		cJSON* e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "step", TIER_STEP[i]);
		cJSON_AddNumberToObject(e, "seconds", (double)TIER_STEP[i] * TIER_CAP[i]);
		cJSON_AddNumberToObject(e, "bytes", (double)ring_file_bytes(TIER_CAP[i], sizeof(HistRec)));
		cJSON_AddBoolToObject(e, "on", ringOn[i]);
		cJSON_AddNumberToObject(e, "points", ringOn[i] ? ring_count(&ring[i]) : 0);
		cJSON_AddItemToArray(t, e);
	}
	g_mutex_unlock(&storeLock);
	cJSON_AddNumberToObject(o, "memoryBytes", (double)HIST_N * (sizeof(HistPoint) + sizeof(guint16) * (X_N + (size_t)coreHN * CW_F)));
	return o;
}

void sampler_stop(void) {
	g_mutex_lock(&storeLock);
	for (int i = 0; i < TIERS; i++) if (ringOn[i]) { ring_close(&ring[i]); ringOn[i] = FALSE; ringWhere[i] = 0; }
	g_mutex_unlock(&storeLock);
}

/* Settings › Clear history: every point in memory, on the built-in flash and on the SD card (the files keep their size).
 * Live readings stay. */
void sampler_clear_history(void) {
	g_mutex_lock(&lock);
	histHead = histCount = 0;
	memset(hist, 0, sizeof(HistPoint) * HIST_N);
	for (gsize i = 0; i < (gsize)HIST_N * X_N; i++) histX[i] = 0x7E00;
	for (gsize i = 0; i < (gsize)HIST_N * coreHN * CW_F; i++) histC[i] = 0x7E00;
	hist_summaries();
	g_mutex_lock(&storeLock);
	for (int i = 0; i < TIERS; i++) { if (ringOn[i]) ring_clear(&ring[i]); tacc_clear(&tacc[i]); }
	if (!(ringOn[0] && ringWhere[0] == 2)) unlink(FLASH_RING);   /* not in use now (a card took over): no old points left */
	g_mutex_unlock(&storeLock);
	g_mutex_unlock(&lock);
}

void sampler_tick(void) {
	tickN++;
	Sample s;
	memset(&s, 0, sizeof(s));
	for (int i = 0; i < MAX_CORES; i++) { s.cores[i] = NAN; for (int q = 0; q < 4; q++) s.coreWork[i][q] = NAN; }
	s.cpu = s.apps = s.mem = s.flash = s.sd = s.tx = s.rx = s.temp = NAN;
	x_clear(s.x);
	for (int i = 0; i < MAX_DSK; i++) s.iops[i] = NAN;
	s.tms = g_get_real_time() / 1000;
	static Now n;                                       /* static: it is a few kB */
	g_mutex_lock(&lock);
	n = now;
	g_mutex_unlock(&lock);
	Slow sl;
	g_mutex_lock(&slowLock);
	sl = slow;
	g_mutex_unlock(&slowLock);
	/* Settings: what to read, and where history is kept (from the second reading, once the lists are known) */
	SamplerSettings cf;
	gboolean dirty = FALSE;
	g_mutex_lock(&cfgLock);
	cf = cfg;
	if (tickN >= 2) { dirty = cfgDirty; cfgDirty = FALSE; }
	g_mutex_unlock(&cfgLock);
	if (tickN >= 2) store_sync(&cf, dirty);
	const unsigned col = cf.collect;

	unsigned long long total = 0, idle = 0, agg[8] = { 0 };
	int cores = read_cpu(&total, &idle, s.cores, agg, s.coreWork);
	if (prevTotal && total > prevTotal) {
		double dt = (double)(total - prevTotal);
		s.cpu = (float)(100.0 * (dt - (double)(idle - prevIdle)) / dt);
		/* Kind of work: user (+nice), system (+softirq), waiting on disk, interrupts, steal */
		double d[8];
		for (int i = 0; i < 8; i++) d[i] = agg[i] > prevAgg[i] ? (double)(agg[i] - prevAgg[i]) : 0;   /* iowait can step back */
		s.x[X_USER] = (float)(100.0 * (d[0] + d[1]) / dt);
		s.x[X_SYS] = (float)(100.0 * (d[2] + d[6]) / dt);
		s.x[X_WAIT] = (float)(100.0 * d[4] / dt);
		s.x[X_IRQ] = (float)(100.0 * d[5] / dt);
		s.x[X_STEAL] = (float)(100.0 * d[7] / dt);
	}
	memcpy(prevAgg, agg, sizeof(prevAgg));
	n.mhz = s.x[X_MHZ] = read_mhz();
	unsigned long long self = self_ticks();
	if (prevSelfTotal && total > prevSelfTotal && self >= prevSelf) s.meCpu = (float)(100.0 * (double)(self - prevSelf) / (double)(total - prevSelfTotal));
	prevSelf = self; prevSelfTotal = total;
	prevTotal = total; prevIdle = idle;
	n.cores = cores;

	long rss = read_kb("/proc/self/status", "RssAnon:");
	s.meMemKb = rss >= 0 ? (float)rss : NAN;
	s.x[X_SELF_CPU] = s.meCpu; s.x[X_SELF_MEM] = rss >= 0 ? (float)(rss / 1024.0) : NAN;
	read_meminfo(&n);
	if (n.memTotalKb > 0 && n.memAvailKb >= 0) s.mem = (float)(100.0 * (n.memTotalKb - n.memAvailKb) / n.memTotalKb);
	if (n.memTotalKb > 0) {
		s.x[X_CACHE] = (float)(100.0 * n.memCacheKb / n.memTotalKb);
		if (n.videoUsedKb >= 0) s.x[X_VIDEO] = (float)(100.0 * n.videoUsedKb / n.memTotalKb);
	}
	if (n.swapTotalKb > 0) s.x[X_SWAP] = (float)(100.0 * n.swapUsedKb / n.swapTotalKb);

	if (col & G_STORAGE) {                              /* storage switched off in Settings: none of this is read */
		if (fs_use(".", &n.flashTotalKb, &n.flashFreeKb)) s.flash = (float)(100.0 * (1.0 - n.flashFreeKb / n.flashTotalKb));
		static gboolean sdDirect = FALSE;
		if (tickN % 10 == 1) {                              /* the SD card can come and go */
			sdDirect = sd_present() && fs_use(SD_PATH, &n.sdTotalKb, &n.sdFreeKb);
			g_mutex_lock(&slowLock);
			slowWantSd = !sdDirect;                         /* then the slow thread asks the camera's disk list */
			g_mutex_unlock(&slowLock);
		}
		if (sdDirect) fs_use(SD_PATH, &n.sdTotalKb, &n.sdFreeKb);
		else { n.sdTotalKb = sl.sdTotalKb; n.sdFreeKb = sl.sdFreeKb; }
		if (n.sdTotalKb > 0) s.sd = (float)(100.0 * (1.0 - n.sdFreeKb / n.sdTotalKb));

		/* Every drive and device: the lists every 30 s, card wear every 10 min, space every 5 s, activity every second */
		if (tickN % 30 == 1) {
			scan_disks(&n);
			scan_mounts(&n);
			link_drives(&n);
			g_mutex_lock(&slowLock);
			for (int k = 0; k < MAX_MNT; k++) g_strlcpy(slowNetPath[k], k < n.mntN && mntp[k].net && n.mnts[k].present ? n.mnts[k].path : "", sizeof(slowNetPath[k]));
			g_mutex_unlock(&slowLock);
		}
		if (tickN % 600 == 0) for (int i = 0; i < n.dskN; i++) read_wear(&n.dsks[i]);
		measure_mounts(&n, s.x);
		read_disks(&n, s.x, s.iops);
	} else {
		g_mutex_lock(&slowLock);
		slowWantSd = FALSE;
		memset(slowNetPath, 0, sizeof(slowNetPath));
		g_mutex_unlock(&slowLock);
	}

	if (col & G_NET) read_net(&s.tx, &s.rx, &n, s.x);
	if (col & G_TEMP) s.temp = read_temps(&n, s.x, &sl);

	/* Power: the camera's own draw, or on a recorder what its PoE ports give out */
	n.powerOk = sl.powerOk; n.poeOk = sl.poeOk;
	n.powerHist = sl.powerOk && sl.powerHist && cf.powerSource != PS_APP;   /* "Recorded by Resource Monitor" keeps its own even then */
	n.powerW = sl.powerW; n.budgetW = sl.budgetW;
	n.powerAvgW = sl.powerAvgW; n.powerMaxW = sl.powerMaxW; n.powerReqW = sl.powerReqW;
	g_strlcpy(n.powerCls, sl.powerCls, sizeof(n.powerCls));
	n.portsN = sl.portsN;
	memcpy(n.ports, sl.ports, sizeof(n.ports));
	if (sl.powerOk) s.x[X_POWER] = sl.powerW;
	else if (sl.poeOk) {
		float w = 0;
		for (int i = 0; i < sl.portsN; i++) if (!isnan(sl.ports[i].w)) w += sl.ports[i].w;
		s.x[X_POWER] = w;
	}
	if (col & G_DLPU) s.x[X_INFER] = read_dlpu(&n);
	else n.dlpu[0] = 0;

	FILE* f = fopen("/proc/loadavg", "r");
	if (f) { if (fscanf(f, "%lf %lf %lf", &n.load1, &n.load5, &n.load15) != 3) n.load1 = 0; fclose(f); }
	s.x[X_L1] = (float)n.load1; s.x[X_L5] = (float)n.load5; s.x[X_L15] = (float)n.load15;
	f = fopen("/proc/uptime", "r");
	if (f) { if (fscanf(f, "%lf", &n.uptime) != 1) n.uptime = 0; fclose(f); }

	GArray* fresh = NULL;
	GArray* freshNet = NULL;
	int procs = 0, threads = 0;
	if (tickN - lastScanTick >= scanEvery || !scan->len) {
		gint64 scanStart = g_get_monotonic_time();
		lastScanTick = tickN;
		double appCoreTicks[MAX_CORES] = { 0 };
		fresh = scan_processes(total, n.uptime, &procs, &threads, appCoreTicks);
		for (int c = 0; c < cores && c < MAX_CORES; c++) {
			unsigned long long dc = scanCoreTotal[c] && prevCoreTotal[c] > scanCoreTotal[c] ? prevCoreTotal[c] - scanCoreTotal[c] : 0;
			n.coresApps[c] = dc ? (float)fmin(100.0, 100.0 * appCoreTicks[c] / (double)dc) : NAN;
			scanCoreTotal[c] = prevCoreTotal[c];
		}
		freshNet = (col & G_CONNS) ? netconn_scan() : NULL;   /* connections switched off: not read */
		double appsCpu = 0, appsMem = 0;
		for (guint i = 0; i < fresh->len; i++) {
			ProcSum* p = &g_array_index(fresh, ProcSum, i);
			if (own_app(p->app)) { appsCpu += p->cpu; appsMem += p->memKb; }   /* Axis's own apps count as the camera */
		}
		n.appsCpu = appsCpu; n.appsMemKb = appsMem;
		n.procs = procs; n.threads = threads; n.names = fresh->len;
		double took = (double)(g_get_monotonic_time() - scanStart) / 1e6;
		if (took > 0.4 && scanEvery < SCAN_MAX) { scanEvery = scanEvery * 2 > SCAN_MAX ? SCAN_MAX : scanEvery * 2; LOG("Process scan took %.2f s: scanning every %d s\n", took, scanEvery); }
		else if (took < 0.15 && scanEvery > SCAN_EVERY) scanEvery = SCAN_EVERY;
	}
	s.apps = (float)n.appsCpu;
	for (int c = 0; c < MAX_CORES; c++) s.coresApps[c] = c < cores ? n.coresApps[c] : NAN;

	g_mutex_lock(&lock);
	if (col & G_TEMP) heater_track(&n, s.tms / 1000);
	now = n;
	s.seq = ++liveSeq;
	live[s.seq % LIVE_N] = s;
	if (fresh) {
		g_array_free(scan, TRUE);
		scan = fresh;
		scanProcs = procs;
		/* History: keep the top 10 of the busiest scan in this point, and the latest by memory */
		if ((col & G_PROCS) && !isnan(s.cpu) && s.cpu > cur.bestCpu) { cur.bestCpu = s.cpu; keep_top(scan, cur.cpuTop, &cur.cpuN, &cur.cpuOther, FALSE); }
		if (col & G_PROCS) {
			GArray* m = g_array_sized_new(FALSE, FALSE, sizeof(ProcSum), scan->len);
			g_array_append_vals(m, scan->data, scan->len);
			g_array_sort(m, by_mem);
			keep_top(m, cur.memTop, &cur.memN, &cur.memOther, TRUE);
			g_array_free(m, TRUE);
		}
		if (net) g_array_free(net, TRUE);
		net = freshNet;
		netOk = freshNet != NULL;
		/* Connection rates are over the 2 s since the last scan: compare them with the Ethernet total over the same time */
		float tr = (isnan(s.tx) ? 0 : s.tx) + (isnan(s.rx) ? 0 : s.rx);
		const Sample* pv = &live[(s.seq - 1 + LIVE_N) % LIVE_N];
		float trAvg = (tr + (pv->seq == s.seq - 1 ? (isnan(pv->tx) ? 0 : pv->tx) + (isnan(pv->rx) ? 0 : pv->rx) : tr)) / 2;
		if (net && tr > cur.bestNet) { cur.bestNet = tr; keep_net(net, &cur, trAvg); }
		LiveTop* lt = &liveTop[s.seq % LIVE_N];
		memset(lt, 0, sizeof(*lt));
		lt->seq = s.seq;
		if (col & G_PROCS) {
			keep_top(scan, lt->cpu, &lt->cpuN, &lt->cpuOther, FALSE);
			GArray* m2 = g_array_sized_new(FALSE, FALSE, sizeof(ProcSum), scan->len);
			g_array_append_vals(m2, scan->data, scan->len);
			g_array_sort(m2, by_mem);
			keep_top(m2, lt->mem, &lt->memN, &lt->memOther, TRUE);
			g_array_free(m2, TRUE);
		}
		if (net) {
			HistPoint tmp;
			keep_net(net, &tmp, trAvg);
			memcpy(lt->net, tmp.netTop, sizeof(lt->net));
			lt->netN = tmp.netN; lt->netOther = tmp.netOther; lt->hasNet = 1;
		}
		/* Waiting on the disk: names with a process in state D in this scan, blocked for the scan interval */
		if (col & G_PROCS) {                            /* the process lists are only kept when Processes is on */
			lt->ioN = 0;
			for (guint i = 0; i < scan->len; i++) {
				ProcSum* p = &g_array_index(scan, ProcSum, i);
				if (!p->dCount) continue;
				guint16 nm = name_id(p->name), ow = name_id(p->app);
				int ln = lt->ioN;
				io_add(lt->io, &ln, IOTOP, nm, ow, (float)scanEvery);
				lt->ioN = (guint8)ln;
				int cn = cur.ioN;
				io_add(cur.ioTop, &cn, IOTOP, nm, ow, (float)scanEvery);
				cur.ioN = (guint8)cn;
			}
		}
	}
	cur.cpu = fmaxn(cur.cpu, s.cpu); cur.apps = fmaxn(cur.apps, s.apps); cur.mem = fmaxn(cur.mem, s.mem);
	cur.tx = fmaxn(cur.tx, s.tx); cur.rx = fmaxn(cur.rx, s.rx); cur.temp = fmaxn(cur.temp, s.temp);
	cur.flash = s.flash; cur.sd = s.sd;
	x_fold(curX, s.x);
	if (n.powerHist) curX[X_POWER] = NAN;               /* the camera keeps its own power log: no second copy in ours */
	for (int c = 0; c < coreHN; c++) {                  /* each core's peaks in this point */
		for (int q = 0; q < 4; q++) curC[c * CW_F + q] = fmaxn(curC[c * CW_F + q], s.coreWork[c][q]);
		curC[c * CW_F + 4] = fmaxn(curC[c * CW_F + 4], s.coresApps[c]);
	}
	gint64 nowUs = g_get_monotonic_time();
	if (!lastPruneUs) lastPruneUs = nowUs;
	static guint keptNames = 0;   /* after the last prune: prune early only when the list has doubled since */
	if (nowUs - lastPruneUs > (gint64)86400 * G_USEC_PER_SEC || (names->len > 4000 && names->len > 2 * keptNames)) { prune_names(); keptNames = names->len; lastPruneUs = nowUs; }
	if (++curTicks >= HIST_STEP) {
		cur.t = s.tms / 1000;
		hist[histHead] = cur;
		guint16* hx = &histX[(size_t)histHead * X_N];
		for (int i = 0; i < X_N; i++) hx[i] = f2h(curX[i] / x_scale(i));
		store_point(&cur, curX, curC);                  /* on the SD card too, when history is kept there */
		x_clear(curX);
		guint16* hc = &histC[(size_t)histHead * coreHN * CW_F];
		for (int q = 0; q < coreHN * CW_F; q++) { hc[q] = f2h(curC[q]); curC[q] = NAN; }
		histHead = (histHead + 1) % HIST_N;
		if (histCount < HIST_N) histCount++;
		hist_summaries();
		memset(&cur, 0, sizeof(cur));
		cur.cpu = cur.apps = cur.mem = cur.flash = cur.sd = cur.tx = cur.rx = cur.temp = NAN;
		cur.bestCpu = -1;
		cur.bestNet = -1;
		curTicks = 0;
	}
	g_mutex_unlock(&lock);
}

/*----------------------------------------------------- JSON for the page */

static cJSON* proc_json(const ProcSum* p) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "name", p->name);
	cJSON_AddStringToObject(o, "app", p->app);
	cJSON_AddNumberToObject(o, "count", p->count);
	add_num(o, "cpu", p->cpu);
	add_num(o, "memKb", p->memKb);
	cJSON_AddNumberToObject(o, "threads", p->threads);
	cJSON_AddNumberToObject(o, "ageS", round(p->ageS));
	return o;
}

static void arr_num(cJSON* a, float v) {
	cJSON_AddItemToArray(a, isnan(v) || isinf(v) ? cJSON_CreateNull() : cJSON_CreateNumber(round(v * 100.0) / 100.0));
}
/* The 1.1 readings, the same shape in live readings and history points (lock held).
 * nets: per interface [rx Mbit/s, tx Mbit/s, rx packets/s, tx packets/s, rx errors, rx drops, tx errors, tx drops]
 * (errors and drops are counts in the reading or point); disks: per device [read kB/s, write kB/s, busy %, (live) IOPS]. */
static void x_json(cJSON* o, const float* x, const float* iops) {
	cJSON* a = cJSON_AddArrayToObject(o, "work");
	for (int i = X_USER; i <= X_STEAL; i++) arr_num(a, x[i]);
	add_num(o, "mhz", x[X_MHZ]);
	a = cJSON_AddArrayToObject(o, "load");
	arr_num(a, x[X_L1]); arr_num(a, x[X_L5]); arr_num(a, x[X_L15]);
	add_num(o, "memCache", x[X_CACHE]); add_num(o, "memVideo", x[X_VIDEO]); add_num(o, "swap", x[X_SWAP]);
	add_num(o, "fan", x[X_FAN]); add_num(o, "power", x[X_POWER]); add_num(o, "infer", x[X_INFER]); add_num(o, "heat", x[X_HEAT]);
	add_num(o, "selfCpu", x[X_SELF_CPU]); add_num(o, "selfMemMb", x[X_SELF_MEM]);
	a = cJSON_AddArrayToObject(o, "temps");
	for (int i = 0; i < now.sensN; i++) arr_num(a, x[X_TEMP0 + i]);
	a = cJSON_AddArrayToObject(o, "nets");
	for (int i = 0; i < now.ifN; i++) {
		cJSON* b = cJSON_CreateArray();
		for (int q = 0; q < NET_F; q++) arr_num(b, x[X_NET0 + i * NET_F + q]);
		cJSON_AddItemToArray(a, b);
	}
	a = cJSON_AddArrayToObject(o, "mounts");
	for (int i = 0; i < now.mntN; i++) arr_num(a, x[X_MNT0 + i]);
	a = cJSON_AddArrayToObject(o, "disks");
	for (int i = 0; i < now.dskN; i++) {
		cJSON* b = cJSON_CreateArray();
		for (int q = 0; q < DSK_HF; q++) arr_num(b, x[X_DSK0 + i * DSK_HF + q]);
		if (iops) arr_num(b, iops[i]);
		cJSON_AddItemToArray(a, b);
	}
}

static cJSON* top_json(const TopEntry* t, int n);
static cJSON* sample_json(const Sample* s, int cores) {
	cJSON* o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "seq", s->seq);
	cJSON_AddNumberToObject(o, "t", (double)s->tms / 1000.0);
	add_num(o, "cpu", s->cpu); add_num(o, "apps", s->apps); add_num(o, "mem", s->mem);
	add_num(o, "flash", s->flash); add_num(o, "sd", s->sd);
	add_num(o, "tx", s->tx); add_num(o, "rx", s->rx); add_num(o, "temp", s->temp);
	cJSON* c = cJSON_AddArrayToObject(o, "cores");
	for (int i = 0; i < cores && i < MAX_CORES; i++) cJSON_AddItemToArray(c, isnan(s->cores[i]) ? cJSON_CreateNull() : cJSON_CreateNumber(round(s->cores[i] * 10) / 10));
	cJSON* ca = cJSON_AddArrayToObject(o, "coresApps");
	for (int i = 0; i < cores && i < MAX_CORES; i++) cJSON_AddItemToArray(ca, isnan(s->coresApps[i]) ? cJSON_CreateNull() : cJSON_CreateNumber(round(s->coresApps[i] * 10) / 10));
	add_num(o, "meCpu", s->meCpu); add_num(o, "meMemKb", s->meMemKb);
	cJSON* cw = cJSON_AddArrayToObject(o, "coresWork");      /* each core: [user, system, waiting on disk, interrupts] */
	for (int i = 0; i < cores && i < MAX_CORES; i++) {
		cJSON* b = cJSON_CreateArray();
		for (int q = 0; q < 4; q++) arr_num(b, s->coreWork[i][q]);
		cJSON_AddItemToArray(cw, b);
	}
	x_json(o, s->x, s->iops);
	const LiveTop* lt = &liveTop[s->seq % LIVE_N];
	if (lt->seq == s->seq) {
		cJSON* tp = cJSON_AddObjectToObject(o, "top");
		cJSON_AddItemToObject(tp, "cpuTop", top_json(lt->cpu, lt->cpuN)); add_num(tp, "cpuOther", lt->cpuOther);
		cJSON_AddItemToObject(tp, "memTop", top_json(lt->mem, lt->memN)); add_num(tp, "memOther", lt->memOther);
		if (lt->hasNet) { cJSON_AddItemToObject(tp, "netTop", top_json(lt->net, lt->netN)); add_num(tp, "netOther", lt->netOther); }
		if (lt->ioN) cJSON_AddItemToObject(tp, "ioTop", top_json(lt->io, lt->ioN));
	}
	return o;
}

cJSON* sampler_live_json(long since, long max) {
	cJSON* out = cJSON_CreateObject();
	g_mutex_lock(&lock);
	cJSON_AddNumberToObject(out, "seq", liveSeq);
	cJSON* arr = cJSON_AddArrayToObject(out, "samples");
	long from = liveSeq - LIVE_N + 1;
	if (since + 1 > from) from = since + 1;
	if (max > 0 && liveSeq - max + 1 > from) from = liveSeq - max + 1;
	if (from < 1) from = 1;
	for (long q = from; q <= liveSeq; q++) cJSON_AddItemToArray(arr, sample_json(&live[q % LIVE_N], now.cores));
	cJSON* n = cJSON_AddObjectToObject(out, "now");
	cJSON_AddNumberToObject(n, "cores", now.cores);
	add_num(n, "memTotalKb", now.memTotalKb); add_num(n, "memAvailKb", now.memAvailKb);
	add_num(n, "memFreeKb", now.memFreeKb); add_num(n, "memCacheKb", now.memCacheKb);
	add_num(n, "appsMemKb", now.appsMemKb); add_num(n, "appsCpu", now.appsCpu);
	add_num(n, "flashTotalKb", now.flashTotalKb); add_num(n, "flashFreeKb", now.flashFreeKb);
	if (now.sdTotalKb > 0) { add_num(n, "sdTotalKb", now.sdTotalKb); add_num(n, "sdFreeKb", now.sdFreeKb); }
	cJSON_AddStringToObject(n, "netIf", now.netIf);
	add_num(n, "linkMbps", now.linkMbps);
	cJSON_AddNumberToObject(n, "rxErrors", (double)(now.rxErr + now.rxDrop));
	cJSON_AddNumberToObject(n, "txErrors", (double)(now.txErr + now.txDrop));
	add_num(n, "load1", now.load1); add_num(n, "load5", now.load5); add_num(n, "load15", now.load15);
	cJSON_AddNumberToObject(n, "uptime", round(now.uptime));
	cJSON_AddNumberToObject(n, "processes", now.procs);
	cJSON_AddNumberToObject(n, "threads", now.threads);
	cJSON_AddNumberToObject(n, "names", now.names);
	/* 1.1: what each list's places are, and the slow sources */
	add_num(n, "mhz", now.mhz);
	add_num(n, "swapTotalKb", now.swapTotalKb); add_num(n, "swapUsedKb", now.swapUsedKb);
	if (now.videoTotalKb > 0) { add_num(n, "videoTotalKb", now.videoTotalKb); add_num(n, "videoUsedKb", now.videoUsedKb); }
	cJSON_AddStringToObject(n, "tempSource", now.sensFromCamera ? "camera" : "zones");
	cJSON* l = cJSON_AddArrayToObject(n, "sensors");
	for (int i = 0; i < now.sensN; i++) {
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", now.sens[i].name);
		add_num(e, "high24", now.sens[i].high24);
		cJSON_AddItemToArray(l, e);
	}
	if (now.fanOk) cJSON_AddBoolToObject(cJSON_AddObjectToObject(n, "fan"), "running", now.fanRunning);
	if (now.heaterOk) {                                 /* 1.5.0: since (on), lastOn and lastFor (off), unix seconds */
		cJSON* h = cJSON_AddObjectToObject(n, "heater");
		cJSON_AddBoolToObject(h, "on", now.heaterOn);
		if (now.heaterOn && now.heatSince) cJSON_AddNumberToObject(h, "since", (double)now.heatSince);
		if (!now.heaterOn && now.heatLastOff) { cJSON_AddNumberToObject(h, "lastOn", (double)now.heatLastOff); cJSON_AddNumberToObject(h, "lastFor", (double)now.heatLastFor); }
	}
	l = cJSON_AddArrayToObject(n, "nets");
	for (int i = 0; i < now.ifN; i++) {
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", now.ifs[i].name);
		cJSON_AddStringToObject(e, "state", now.ifs[i].state);        /* plain words */
		cJSON_AddStringToObject(e, "operstate", now.ifs[i].oper);
		add_num(e, "linkMbps", now.ifs[i].linkMbps);
		cJSON_AddBoolToObject(e, "present", now.ifs[i].present);
		add_num(e, "errorsHour", now.ifs[i].errorsHour); add_num(e, "dropsHour", now.ifs[i].dropsHour);
		cJSON_AddItemToArray(l, e);
	}
	l = cJSON_AddArrayToObject(n, "mounts");
	for (int i = 0; i < now.mntN; i++) {
		const MntPub* m = &now.mnts[i];
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", m->name);
		cJSON_AddStringToObject(e, "path", m->path);
		cJSON_AddStringToObject(e, "kind", m->kind);
		cJSON_AddBoolToObject(e, "present", m->present);
		if (m->totalKb > 0) { add_num(e, "totalKb", m->totalKb); add_num(e, "freeKb", m->freeKb); }
		cJSON_AddNumberToObject(e, "disk", m->disk);
		cJSON_AddItemToArray(l, e);
	}
	l = cJSON_AddArrayToObject(n, "disks");
	for (int i = 0; i < now.dskN; i++) {
		const DskPub* d = &now.dsks[i];
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", d->name);
		cJSON_AddStringToObject(e, "dev", d->dev);
		cJSON_AddBoolToObject(e, "present", d->present);
		if (d->life) {                                  /* "Card life used 10–20 %": lifeLow is the lower end */
			gchar* t = d->life >= 11 ? g_strdup("over 100 %") : g_strdup_printf("%d\xE2\x80\x93%d %%", (d->life - 1) * 10, d->life * 10);
			cJSON_AddStringToObject(e, "life", t);
			cJSON_AddNumberToObject(e, "lifeLow", (d->life - 1) * 10);
			g_free(t);
		}
		if (d->eol) cJSON_AddStringToObject(e, "eol", d->eol == 1 ? "Normal" : d->eol == 2 ? "Warning" : "Urgent");
		cJSON_AddItemToArray(l, e);
	}
	if (now.powerOk || now.poeOk) {
		cJSON* p = cJSON_AddObjectToObject(n, "power");
		cJSON_AddStringToObject(p, "kind", now.powerOk ? "camera" : "ports");
		if (now.powerHist) cJSON_AddBoolToObject(p, "history", TRUE);   /* the camera keeps its own power log: see the power endpoint */
		add_num(p, "w", now.powerOk ? now.powerW : NAN);
		if (now.powerOk) { add_num(p, "avgW", now.powerAvgW); add_num(p, "maxW", now.powerMaxW); add_num(p, "requestedW", now.powerReqW); }
		if (now.powerCls[0]) cJSON_AddStringToObject(p, "class", now.powerCls);
		if (!isnan(now.budgetW)) add_num(p, "budgetW", now.budgetW);
		cJSON* ports = cJSON_AddArrayToObject(p, "ports");
		for (int i = 0; i < now.portsN; i++) {
			const PortPub* q = &now.ports[i];
			cJSON* e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "port", q->port);
			add_num(e, "w", q->w); add_num(e, "limitW", q->limitW);
			cJSON_AddStringToObject(e, "class", q->cls);
			cJSON_AddStringToObject(e, "state", q->state);
			cJSON_AddItemToArray(ports, e);
		}
	}
	if (now.dlpu[0]) cJSON_AddStringToObject(cJSON_AddObjectToObject(n, "dlpu"), "kind", now.dlpu);
	/* The five busiest by CPU and by memory, for the list beside the graph */
	cJSON* tc = cJSON_AddArrayToObject(out, "topCpu");
	for (guint i = 0; i < scan->len && i < 5; i++) cJSON_AddItemToArray(tc, proc_json(&g_array_index(scan, ProcSum, i)));
	GArray* m = g_array_sized_new(FALSE, FALSE, sizeof(ProcSum), scan->len);
	g_array_append_vals(m, scan->data, scan->len);
	g_array_sort(m, by_mem);
	cJSON_AddBoolToObject(n, "netConnOk", netOk);
	/* 1.3.0: what Settings switched on, and how long history is kept (the 7 d / 30 d buttons follow it) */
	{
		SamplerSettings c = cfg_get();
		cJSON* k = cJSON_AddObjectToObject(n, "settings");
		cJSON_AddNumberToObject(k, "collect", c.collect);
		cJSON_AddNumberToObject(k, "powerSource", c.powerSource);
		cJSON_AddStringToObject(k, "tempUnit", c.tempF ? "F" : "C");   /* 1.6.1: how the page shows temperatures */
		cJSON_AddNumberToObject(k, "historyDays", ringOn[2] ? 30 : ringOn[1] ? 7 : 1);
		cJSON_AddBoolToObject(k, "onCard", ringOn[0] && ringWhere[0] == 1);
		cJSON_AddBoolToObject(k, "onFlash", ringOn[0] && ringWhere[0] == 2);
	}
	cJSON* tn = cJSON_AddArrayToObject(out, "topNet");
	for (guint i = 0; net && i < net->len && i < 5; i++) {
		NetUse* u = &g_array_index(net, NetUse, i);
		cJSON* o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "service", u->service);
		cJSON_AddStringToObject(o, "peer", u->peer);
		add_num(o, "tx", u->tx); add_num(o, "rx", u->rx);
		cJSON_AddNumberToObject(o, "conns", u->conns);
		cJSON_AddItemToArray(tn, o);
	}
	cJSON* tm = cJSON_AddArrayToObject(out, "topMem");
	for (guint i = 0; i < m->len && i < 5; i++) cJSON_AddItemToArray(tm, proc_json(&g_array_index(m, ProcSum, i)));
	g_array_free(m, TRUE);
	g_mutex_unlock(&lock);
	return out;
}

static cJSON* top_json(const TopEntry* t, int n) {
	cJSON* a = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON* e = cJSON_CreateArray();
		cJSON_AddItemToArray(e, cJSON_CreateString(name_of(t[i].name)));
		cJSON_AddItemToArray(e, cJSON_CreateString(name_of(t[i].owner)));
		cJSON_AddItemToArray(e, cJSON_CreateNumber(round(t[i].val * 100) / 100));
		cJSON_AddItemToArray(a, e);
	}
	return a;
}

/* 7 and 30 days, from the card: the points grouped to at most `points`, peaks kept (web thread). NULL when not kept. */
typedef struct { guint32 since; int per, k, n, max; TierAcc* out; TierAcc cur; } LongCtx;
static void long_cb(const void* rp, void* u) {
	LongCtx* c = u;
	const HistRec* r = rp;
	if (r->t < c->since || c->n >= c->max) return;
	float b[8], x[X_N], cc[CW_MAX * CW_F];
	for (int i = 0; i < 8; i++) b[i] = h2f(r->b[i]);
	for (int i = 0; i < X_N; i++) x[i] = h2f(r->x[i]) * x_scale(i);
	for (int i = 0; i < CW_MAX * CW_F; i++) cc[i] = h2f(r->c[i]);
	tacc_fold(&c->cur, b, x, cc, r->t);
	if (++c->k >= c->per) { c->out[c->n++] = c->cur; tacc_clear(&c->cur); c->k = 0; }
}
static cJSON* long_history_json(int seconds, int points) {
	int tier = seconds <= 7 * 86400 ? 1 : 2;
	g_mutex_lock(&storeLock);
	if (!ringOn[tier]) { g_mutex_unlock(&storeLock); return NULL; }
	guint32 want = (guint32)seconds / TIER_STEP[tier];
	LongCtx c;
	memset(&c, 0, sizeof(c));
	c.since = (guint32)(time(NULL) - seconds);
	c.per = (int)((want + (guint32)points - 1) / (guint32)points);
	if (c.per < 1) c.per = 1;
	c.max = points + 1;
	c.out = g_new(TierAcc, c.max);
	tacc_clear(&c.cur);
	ring_read(&ring[tier], want, long_cb, &c);
	if (c.k && c.n < c.max) c.out[c.n++] = c.cur;
	guint32 kept = ring_count(&ring[tier]) * TIER_STEP[tier];
	g_mutex_unlock(&storeLock);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddNumberToObject(out, "seconds", seconds);
	cJSON_AddNumberToObject(out, "step", c.per * TIER_STEP[tier]);
	cJSON_AddNumberToObject(out, "kept", kept);
	cJSON_AddStringToObject(out, "source", "card");
	cJSON* arr = cJSON_AddArrayToObject(out, "points");
	g_mutex_lock(&lock);                                /* x_json reads how many sensors, interfaces and drives there are */
	for (int i = 0; i < c.n; i++) {
		const TierAcc* a = &c.out[i];
		cJSON* o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "t", a->t);
		add_num(o, "cpu", a->b[0]); add_num(o, "apps", a->b[1]); add_num(o, "mem", a->b[2]);
		add_num(o, "flash", a->b[3]); add_num(o, "sd", a->b[4]);
		add_num(o, "tx", a->b[5]); add_num(o, "rx", a->b[6]); add_num(o, "temp", a->b[7]);
		x_json(o, a->x, NULL);
		cJSON* cw = cJSON_AddArrayToObject(o, "coresWork");
		for (int k = 0; k < coreHN; k++) {
			cJSON* b = cJSON_CreateArray();
			for (int q = 0; q < CW_F; q++) arr_num(b, a->c[k * CW_F + q]);
			cJSON_AddItemToArray(cw, b);
		}
		cJSON_AddItemToArray(arr, o);
	}
	g_mutex_unlock(&lock);
	g_free(c.out);
	return out;
}

cJSON* sampler_history_json(int seconds, int points) {
	if (points < 10) points = 10;
	if (points > 400) points = 400;
	if (seconds > HIST_N * HIST_STEP) {                 /* 7 and 30 days: from the SD card, when kept there */
		cJSON* o = long_history_json(seconds > 30 * 86400 ? 30 * 86400 : seconds, points);
		if (o) return o;
	}
	if (seconds < 60) seconds = 60;
	if (seconds > HIST_N * HIST_STEP) seconds = HIST_N * HIST_STEP;
	if (points < 10) points = 10;
	if (points > 400) points = 400;
	cJSON* out = cJSON_CreateObject();
	cJSON_AddNumberToObject(out, "seconds", seconds);
	cJSON* arr = cJSON_AddArrayToObject(out, "points");
	g_mutex_lock(&lock);
	int want = seconds / HIST_STEP, have = histCount < want ? histCount : want;
	int per = (want + points - 1) / points;            /* history points per graph point */
	if (per < 1) per = 1;
	cJSON_AddNumberToObject(out, "step", per * HIST_STEP);
	cJSON_AddNumberToObject(out, "kept", histCount * HIST_STEP);
	/* Oldest first, in groups of `per`; each group keeps its peaks and the top 10 at its busiest */
	int first = (histHead - have + HIST_N) % HIST_N;
	for (int g = 0; g < have; g += per) {
		HistPoint agg;
		memset(&agg, 0, sizeof(agg));
		agg.cpu = agg.apps = agg.mem = agg.flash = agg.sd = agg.tx = agg.rx = agg.temp = NAN;
		const HistPoint* bc = NULL; const HistPoint* bm = NULL; const HistPoint* bn = NULL;
		TopEntry ioAcc[32];
		int ioN = 0;
		float ax[X_N], px[X_N], ac[CW_MAX * CW_F];
		x_clear(ax);
		for (int q = 0; q < CW_MAX * CW_F; q++) ac[q] = NAN;
		for (int k = g; k < g + per && k < have; k++) {
			const HistPoint* p = &hist[(first + k) % HIST_N];
			hist_x((first + k) % HIST_N, px);
			x_fold(ax, px);
			const guint16* hc = &histC[(size_t)((first + k) % HIST_N) * coreHN * CW_F];
			for (int q = 0; q < coreHN * CW_F; q++) ac[q] = fmaxn(ac[q], h2f(hc[q]));
			if (!bc || (!isnan(p->cpu) && (isnan(bc->cpu) || p->cpu > bc->cpu))) bc = p;
			if (!bm || (!isnan(p->mem) && (isnan(bm->mem) || p->mem > bm->mem))) bm = p;
			if (!bn || p->bestNet > bn->bestNet) bn = p;
			for (int d = 0; d < p->ioN; d++) io_add(ioAcc, &ioN, 32, p->ioTop[d].name, p->ioTop[d].owner, p->ioTop[d].val);
			agg.cpu = fmaxn(agg.cpu, p->cpu); agg.apps = fmaxn(agg.apps, p->apps); agg.mem = fmaxn(agg.mem, p->mem);
			agg.tx = fmaxn(agg.tx, p->tx); agg.rx = fmaxn(agg.rx, p->rx); agg.temp = fmaxn(agg.temp, p->temp);
			agg.flash = p->flash; agg.sd = p->sd; agg.t = p->t;
		}
		cJSON* o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "t", (double)agg.t);
		add_num(o, "cpu", agg.cpu); add_num(o, "apps", agg.apps); add_num(o, "mem", agg.mem);
		add_num(o, "flash", agg.flash); add_num(o, "sd", agg.sd);
		add_num(o, "tx", agg.tx); add_num(o, "rx", agg.rx); add_num(o, "temp", agg.temp);
		x_json(o, ax, NULL);
		if (coreHN) {                                   /* each core: [user, system, waiting on disk, interrupts, apps you installed], peaks */
			cJSON* cw = cJSON_AddArrayToObject(o, "coresWork");
			for (int c = 0; c < coreHN; c++) {
				cJSON* b = cJSON_CreateArray();
				for (int q = 0; q < CW_F; q++) arr_num(b, ac[c * CW_F + q]);
				cJSON_AddItemToArray(cw, b);
			}
		}
		if (bc) { cJSON_AddItemToObject(o, "cpuTop", top_json(bc->cpuTop, bc->cpuN)); add_num(o, "cpuOther", bc->cpuOther); }
		if (bm) { cJSON_AddItemToObject(o, "memTop", top_json(bm->memTop, bm->memN)); add_num(o, "memOther", bm->memOther); }
		if (bn && bn->bestNet >= 0) { cJSON_AddItemToObject(o, "netTop", top_json(bn->netTop, bn->netN)); add_num(o, "netOther", bn->netOther); }
		if (ioN) {                                      /* waiting on the disk over the group: the 5 names blocked longest */
			for (int a = 0; a < ioN; a++) for (int b2 = a + 1; b2 < ioN; b2++) if (ioAcc[b2].val > ioAcc[a].val) { TopEntry t = ioAcc[a]; ioAcc[a] = ioAcc[b2]; ioAcc[b2] = t; }
			cJSON_AddItemToObject(o, "ioTop", top_json(ioAcc, ioN < IOTOP ? ioN : IOTOP));
		}
		cJSON_AddItemToArray(arr, o);
	}
	g_mutex_unlock(&lock);
	return out;
}

cJSON* sampler_processes_json(void) {
	cJSON* out = cJSON_CreateObject();
	g_mutex_lock(&lock);
	cJSON_AddNumberToObject(out, "interval", scanEvery);
	cJSON_AddNumberToObject(out, "processes", scanProcs);
	cJSON_AddNumberToObject(out, "threads", now.threads);
	cJSON* l = cJSON_AddArrayToObject(out, "list");
	for (guint i = 0; i < scan->len; i++) cJSON_AddItemToArray(l, proc_json(&g_array_index(scan, ProcSum, i)));
	g_mutex_unlock(&lock);
	return out;
}


/*----------------------------------------------------- the camera's own power log */

/* Cameras that keep a power history (power-settings.cgi getPowerHistory) answer the page's 1 h to 30 d
 * views from it, so Resource Monitor doesn't keep a second copy. Asked when the page asks, kept 60 s. */
#define PH_SLOTS 4

static struct { int seconds; gint64 at; cJSON* json; } phCache[PH_SLOTS];

/* "11/24/2026 20:30:00" in the camera's local time, as unix seconds; 0 when it can't be read */
static gint64 cam_time(const char* s) {
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	if (!s || sscanf(s, "%d/%d/%d %d:%d:%d", &tm.tm_mon, &tm.tm_mday, &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
	tm.tm_mon -= 1; tm.tm_year -= 1900; tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	return t == (time_t)-1 ? 0 : (gint64)t;
}

cJSON* sampler_power_history_json(int seconds) {
	if (seconds < 60) seconds = 60;
	if (seconds > 2592000) seconds = 2592000;          /* the camera keeps 30 days at most */
	gint64 nowUs = g_get_monotonic_time();
	g_mutex_lock(&phLock);
	for (int i = 0; i < PH_SLOTS; i++)
		if (phCache[i].json && phCache[i].seconds == seconds && nowUs - phCache[i].at < 60 * G_USEC_PER_SEC) {
			cJSON* c = cJSON_Duplicate(phCache[i].json, 1);
			g_mutex_unlock(&phLock);
			return c;
		}
	g_mutex_unlock(&phLock);
	cJSON* out = cJSON_CreateObject();
	g_mutex_lock(&lock);
	gboolean has = now.powerHist;
	g_mutex_unlock(&lock);
	if (!has) { cJSON_AddBoolToObject(out, "ok", FALSE); cJSON_AddStringToObject(out, "why", "This camera keeps no power history"); return out; }
	if (!powerVer[0]) power_version();
	gchar* body = g_strdup_printf("{\"apiVersion\":\"%s\",\"method\":\"getPowerHistory\",\"params\":{\"requestedTimeSpan\":%d}}", powerVer[0] ? powerVer : "1.7", seconds);
	char* x = ACAP_VAPIX_Post_Background("power-settings.cgi", body);
	g_free(body);
	cJSON* j = x ? cJSON_Parse(x) : NULL;
	free(x);
	cJSON* d = cJSON_GetObjectItem(j, "data"), * m = cJSON_GetObjectItem(d, "powerMeasurements");
	int n = cJSON_GetArraySize(m);
	if (!j || !cJSON_IsArray(m) || n < 1) {
		cJSON_Delete(j);
		cJSON_AddBoolToObject(out, "ok", FALSE);
		cJSON_AddStringToObject(out, "why", "The camera did not answer with its power history");
		return out;
	}
	double span = cJSON_GetNumberValue(cJSON_GetObjectItem(d, "acquiredTimeSpan"));
	if (isnan(span) || span <= 0) span = seconds;
	double step = span / n;
	gint64 t = (gint64)time(NULL), end = cam_time(cJSON_GetStringValue(cJSON_GetObjectItem(d, "endTimeStamp")));
	/* The camera's local time: if it doesn't fit the clock here (another time zone), keep only its place in the step */
	if (!end || end > t + 120 || t - end > step + 300) {
		gint64 off = end ? (((t - end) % (gint64)step) + (gint64)step) % (gint64)step : 0;
		end = t - off;
	}
	cJSON_AddBoolToObject(out, "ok", TRUE);
	cJSON_AddStringToObject(out, "source", "camera");
	cJSON_AddNumberToObject(out, "seconds", round(span));
	cJSON_AddNumberToObject(out, "step", round(step));
	cJSON_AddNumberToObject(out, "end", (double)end);
	add_num(out, "average", cJSON_GetNumberValue(cJSON_GetObjectItem(d, "powerAverage")));
	cJSON* pts = cJSON_AddArrayToObject(out, "points");
	for (int i = 0; i < n; i++) {
		cJSON* e = cJSON_CreateObject(), * v = cJSON_GetArrayItem(m, i);
		cJSON_AddNumberToObject(e, "t", (double)(end - (gint64)round((n - 1 - i) * step)));
		add_num(e, "power", cJSON_IsNumber(v) ? v->valuedouble : NAN);
		cJSON_AddItemToArray(pts, e);
	}
	cJSON_Delete(j);
	/* Keep it a minute; the oldest place is reused, so the cache never grows */
	g_mutex_lock(&phLock);
	int k = 0;
	for (int i = 1; i < PH_SLOTS; i++) if (phCache[i].at < phCache[k].at) k = i;
	cJSON_Delete(phCache[k].json);
	phCache[k].json = cJSON_Duplicate(out, 1);
	phCache[k].seconds = seconds;
	phCache[k].at = nowUs;
	g_mutex_unlock(&phLock);
	return out;
}

cJSON* sampler_app_usage_json(void) {
	cJSON* out = cJSON_CreateObject();
	g_mutex_lock(&lock);
	for (guint i = 0; i < scan->len; i++) {
		ProcSum* p = &g_array_index(scan, ProcSum, i);
		if (!p->app[0]) continue;
		cJSON* a = cJSON_GetObjectItem(out, p->app);
		if (!a) {
			a = cJSON_AddObjectToObject(out, p->app);
			cJSON_AddNumberToObject(a, "cpu", 0); cJSON_AddNumberToObject(a, "memKb", 0);
			cJSON_AddNumberToObject(a, "processes", 0); cJSON_AddNumberToObject(a, "ageS", 0);
		}
		cJSON_SetNumberValue(cJSON_GetObjectItem(a, "cpu"), cJSON_GetObjectItem(a, "cpu")->valuedouble + p->cpu);
		cJSON_SetNumberValue(cJSON_GetObjectItem(a, "memKb"), cJSON_GetObjectItem(a, "memKb")->valuedouble + p->memKb);
		cJSON_SetNumberValue(cJSON_GetObjectItem(a, "processes"), cJSON_GetObjectItem(a, "processes")->valuedouble + p->count);
		if (p->ageS > cJSON_GetObjectItem(a, "ageS")->valuedouble) cJSON_SetNumberValue(cJSON_GetObjectItem(a, "ageS"), round(p->ageS));
	}
	g_mutex_unlock(&lock);
	return out;
}

/*----------------------------------------------------- 1.3.2: the Prometheus page */
/* The latest reading in the Prometheus text format: only the groups whose Send switch is on (prom.c) */
/* The latest reading for the ways out (device names are the caller's) */
void sampler_snapshot(void* ps, unsigned send) {
	PromSnap* p = ps;                               /* zeroed by the caller */
	p->send = send;
	p->t = (long long)time(NULL);
	g_mutex_lock(&lock);
	Sample s;
	if (liveSeq) s = live[liveSeq % LIVE_N];
	else { memset(&s, 0, sizeof(s)); s.cpu = s.apps = NAN; x_clear(s.x); for (int i = 0; i < MAX_CORES; i++) s.cores[i] = NAN; }
	const double KB = 1024.0;
	const Now* n = &now;
	p->cpu = s.cpu; p->apps = s.apps;
	p->coreN = n->cores < PROM_CORES ? n->cores : PROM_CORES;
	for (int i = 0; i < p->coreN; i++) p->cores[i] = s.cores[i];
	p->work[0] = s.x[X_USER]; p->work[1] = s.x[X_SYS]; p->work[2] = s.x[X_WAIT]; p->work[3] = s.x[X_IRQ];
	p->load[0] = n->load1; p->load[1] = n->load5; p->load[2] = n->load15;
	p->memTotal = n->memTotalKb > 0 ? n->memTotalKb * KB : NAN;
	p->memUsed = n->memTotalKb > 0 && n->memAvailKb >= 0 ? (n->memTotalKb - n->memAvailKb) * KB : NAN;
	p->memAvail = n->memAvailKb >= 0 ? n->memAvailKb * KB : NAN;
	p->memCache = n->memCacheKb >= 0 ? n->memCacheKb * KB : NAN;
	p->memVideo = n->videoUsedKb >= 0 ? n->videoUsedKb * KB : NAN;
	p->swapUsed = n->swapTotalKb > 0 ? n->swapUsedKb * KB : NAN;
	p->appsMem = n->appsMemKb * KB;
	gboolean sdMount = FALSE;
	for (int i = 0; i < n->mntN && p->driveN < PROM_DRIVES; i++) {
		const MntPub* m = &n->mnts[i];
		if (!m->present || m->totalKb <= 0) continue;
		if (!strcmp(m->kind, "sd")) sdMount = TRUE;
		g_strlcpy(p->driveName[p->driveN], m->name, sizeof(p->driveName[0]));   /* plain names: "SD card", "Built-in", "NAS" */
		p->driveTotal[p->driveN] = m->totalKb * KB;
		p->driveUsed[p->driveN] = (m->totalKb - m->freeKb) * KB;
		p->driveN++;
	}
	if (!sdMount && n->sdTotalKb > 0 && p->driveN < PROM_DRIVES) {
		g_strlcpy(p->driveName[p->driveN], "SD card", sizeof(p->driveName[0]));
		p->driveTotal[p->driveN] = n->sdTotalKb * KB;
		p->driveUsed[p->driveN] = (n->sdTotalKb - n->sdFreeKb) * KB;
		p->driveN++;
	}
	for (int i = 0; i < n->dskN && p->devN < PROM_DEVS; i++) {
		if (!n->dsks[i].present) continue;
		g_strlcpy(p->devName[p->devN], n->dsks[i].dev, sizeof(p->devName[0]));
		p->devRead[p->devN] = s.x[X_DSK0 + i * DSK_HF + 0] * KB;
		p->devWrite[p->devN] = s.x[X_DSK0 + i * DSK_HF + 1] * KB;
		p->devN++;
	}
	for (int i = 0; i < n->ifN && p->ifN < PROM_IFS; i++) {
		if (!n->ifs[i].present) continue;
		g_strlcpy(p->ifName[p->ifN], n->ifs[i].name, sizeof(p->ifName[0]));
		p->ifRx[p->ifN] = s.x[X_NET0 + i * NET_F + 0] * 1e6;
		p->ifTx[p->ifN] = s.x[X_NET0 + i * NET_F + 1] * 1e6;
		memcpy(p->ifTot[p->ifN], n->ifs[i].tot, sizeof(p->ifTot[0]));
		p->ifN++;
	}
	p->sensN = n->sensN < PROM_SENS ? n->sensN : PROM_SENS;
	for (int i = 0; i < p->sensN; i++) { g_strlcpy(p->sensName[i], n->sens[i].name, sizeof(p->sensName[0])); p->sensC[i] = s.x[X_TEMP0 + i]; }
	p->fanRpm = s.x[X_FAN];
	p->powerW = s.x[X_POWER];
	p->hasAi = n->dlpu[0] != 0;
	p->aiJobs = s.x[X_INFER];
	g_mutex_unlock(&lock);
}
char* sampler_metrics(unsigned send, const char* model, const char* os, const char* appVer, int* count) {
	PromSnap* p = g_new0(PromSnap, 1);
	g_strlcpy(p->model, model ? model : "", sizeof(p->model));
	g_strlcpy(p->os, os ? os : "", sizeof(p->os));
	g_strlcpy(p->appVer, appVer ? appVer : "", sizeof(p->appVer));
	sampler_snapshot(p, send);
	char* out = prom_render(p, count);
	g_free(p);
	return out;
}
