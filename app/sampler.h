/*
 * sampler: reads what the camera is using, once a second, and keeps 24 h of history.
 *
 * sampler_tick() runs on the main loop. Everything else may be called from the
 * web thread: all shared state is behind one mutex.
 */
#ifndef SAMPLER_H
#define SAMPLER_H

#include <glib.h>
#include "cJSON.h"

void   sampler_init(void);
void   sampler_tick(void);                          /* every second */

cJSON* sampler_live_json(long since, long max);     /* up to `max` 1 s readings newer than `since` (sequence number) */
cJSON* sampler_history_json(int seconds, int points); /* peaks per point, with the top 10 then; over 24 h from the card */
cJSON* sampler_processes_json(void);                 /* the latest scan, every process name */
cJSON* sampler_app_usage_json(void);                 /* per installed app: cpu, memKb, processes */
cJSON* sampler_power_history_json(int seconds);     /* the camera's own power log for the last `seconds` (1 h to 30 d), kept 60 s */
cJSON* sampler_camera_json(void);                    /* what the camera itself reports: streams, disk health and recording space, time sync, security */

/* 1.3.0: Settings. Groups of readings: a group neither shown nor sent is not read at all. */
enum { G_CPU = 1, G_MEM = 2, G_STORAGE = 4, G_NET = 8, G_TEMP = 16, G_POWER = 32, G_DLPU = 64, G_PROCS = 128, G_CONNS = 256, G_EVENTS = 512 /* 1.6.5: no longer read (camera events removed); the bit stays so saved settings keep their meaning */, G_ALL = 1023 };
enum { PS_AUTO, PS_CAMERA, PS_APP, PS_OFF };          /* where power figures come from */
typedef struct { int historyDays; int powerSource; unsigned collect; int tempF; } SamplerSettings;   /* tempF: show temperatures in °F (1.6.1) */
void   sampler_apply(const SamplerSettings* s);       /* any thread; history storage follows on the next reading */
cJSON* sampler_store_json(void);                     /* where history is kept: the card's state, each tier's size and points */
int    sampler_card_ok(void);                         /* 1 when there is an SD card folder this app can write */
int    sampler_power_log_known(void);                 /* -1 not known yet, 0 no, 1 the camera keeps a power log */
void   sampler_set_need(unsigned groups);          /* 1.4.0: groups the alerts need read (any thread) */
void   sampler_stop(void);                            /* write what is waiting to the card (main loop, on stop) */
/* 1.3.1: Settings › Clear */
void   sampler_clear_history(void);                   /* every history point, in memory, on the built-in flash and on the SD card */
/* 1.3.2: the Prometheus page text for the groups in `send` (to free); *count: samples */
char*  sampler_metrics(unsigned send, const char* model, const char* os, const char* appVer, int* count);
/* 1.3.3: the latest reading for MQTT and InfluxDB (PromSnap from prom.h; the caller fills model, os, appVer, serial) */
void   sampler_snapshot(void* promSnap, unsigned send);

#endif
