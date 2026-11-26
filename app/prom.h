/* The metrics sent out (1.3.2 Prometheus page, 1.3.3 MQTT and InfluxDB): a snapshot of the latest reading, walked
 * once into the same metric names and labels for every way out. Only the groups whose Send switch is on; never
 * process names, addresses or user names. Plain C (testable anywhere). */
#ifndef PROM_H
#define PROM_H
#include "cJSON.h"

/* The same group bits as sampler.h (G_CPU ... G_EVENTS) */
#define PG_CPU     1
#define PG_MEM     2
#define PG_STORAGE 4
#define PG_NET     8
#define PG_TEMP    16
#define PG_POWER   32
#define PG_DLPU    64
#define PG_LINKS   1024                             /* 1.9.0: links to other devices, from Port Testing */

#define PROM_CORES  16
#define PROM_DRIVES 12
#define PROM_DEVS   4
#define PROM_IFS    4
#define PROM_SENS   8
#define PROM_LINKS  16

typedef struct {
	unsigned send;                                  /* PG_* bits */
	char model[64], os[32], appVer[24], serial[32];
	long long t;                                    /* unix seconds of the reading */
	/* NAN where not known */
	double cpu, apps, cores[PROM_CORES], work[4], load[3];          /* %; work: user, system, iowait, irq */
	int coreN;
	double memUsed, memAvail, memCache, memVideo, memTotal, swapUsed, appsMem;   /* bytes */
	int driveN; char driveName[PROM_DRIVES][40]; double driveUsed[PROM_DRIVES], driveTotal[PROM_DRIVES];
	int devN; char devName[PROM_DEVS][16]; double devRead[PROM_DEVS], devWrite[PROM_DEVS];   /* bytes/s */
	int ifN; char ifName[PROM_IFS][16]; double ifRx[PROM_IFS], ifTx[PROM_IFS];             /* bits/s */
	unsigned long long ifTot[PROM_IFS][4];          /* rx errors, rx drops, tx errors, tx drops (counters) */
	int sensN; char sensName[PROM_SENS][32]; double sensC[PROM_SENS];
	double fanRpm, powerW, aiJobs;
	int hasAi;
	/* 1.9.0: each device Port Testing has a link to: round trip now and its 95th percentile (ms), loss (%) */
	int linkN; char linkName[PROM_LINKS][64]; double linkRtt[PROM_LINKS], linkP95[PROM_LINKS], linkLoss[PROM_LINKS];
} PromSnap;

/* The Prometheus text format 0.0.4 (malloc'd, to free); *count: how many samples */
char* prom_render(const PromSnap* p, int* count);
/* One JSON object per group that is sent: {"cpu": {"t":..., "cpu_percent": 43.7, "cpu_core_percent": {"1": 40}, ...}, ...}.
 * Names are the Prometheus names without "axis_rm_" (or "resource_monitor_"); one label makes an object by its value, two labels a nested one. */
cJSON* prom_json(const PromSnap* p);
/* InfluxDB line protocol, measurement "axis_resource_monitor", tags model and serial plus each sample's labels,
 * seconds precision (malloc'd, to free); *lines: how many lines */
char* prom_influx(const PromSnap* p, int* lines);
#endif
