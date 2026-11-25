/*
 * kit: the camera side of the ACAP UI kit. Copy kit.h and the kit_*.c files
 * unchanged into a new ACAP, next to ACAP.c, and add them to OBJS1 in the Makefile
 * (kit 1.3.0 adds kit_configio.c, and ACAP.h takes POST bodies up to 256 KB: copy it too).
 *
 * kit_init() registers these endpoints (declare each in manifest.json httpConfig, admin):
 *   resources  GET   CPU, memory, storage, SD card, Ethernet, temperature, uptime; all apps' share
 *                    (appsCpu, appsMemKb, the same method as Resource Monitor) and this app's own numbers
 *   processes  GET   top 10 processes by CPU (?by=mem for memory), the rest added up under Other
 *   apps       GET   the apps installed on the camera and their status (for source lights)
 *   log        GET   the last 100 log lines, newest first
 *              POST  {"action":"clear"} (JSON only) empties it and logs who did it
 *   configio   GET/POST  settings export and import (see kit_configio_section below)
 *
 * The page side is html/kit/acap-kit.js and acap-kit.css. Call kit_init() after ACAP_Init()
 * and before the main loop runs. Everything here may be called from any thread.
 */
#ifndef KIT_H
#define KIT_H

#include <glib.h>
#include "cJSON.h"

#define KIT_VERSION "1.3.0"   /* the same in acap-kit.js and acap-kit.css; the page shows it on hover over the version */

void kit_init(const char* appPackage, const char* appName);

/* Resource limits: every app keeps its own use in check. Call setpriority(PRIO_PROCESS, 0, 10)
 * first in main yourself (before ACAP_Init starts a thread), then kit_limits() after kit_init().
 * Every minute the app's private memory is read; above memLimitKb 3 times in a row, the reason is
 * written to the kit log and syslog and the app exits, and the camera starts it again
 * (manifest runMode respawn). Set the limit to about ten times normal use. A test build passes
 * -DKIT_MEM_LIMIT_KB=512 to see the restart work. The kit's own lists are all capped, the
 * resources sample stretches when it runs slow, and the process scan runs at most every 4 s. */
void kit_limits(long memLimitKb);

/* Log: one line, newest first, last 100 kept (on storage, written at most every 10 s).
 * item:   the item (rule, app...) the line belongs to; the page's Item column
 * what:   what came in or happened
 * detail: what was sent or done ("" or NULL for none)
 * who:    camera user, "Camera", or NULL for the app itself */
void kit_log(const char* item, const char* what, const char* detail, const char* who);
/* The same, with the app's own fields on the line (for example {"state":"On","name":"Person Detected"});
 * the page's Kit.log columns read them by key. fields is copied, not taken; t, item and who are ignored.
 * item NULL means the app itself. */
void kit_log_fields(const char* item, cJSON* fields, const char* who);
void kit_log_flush(void);   /* write the log to storage now (before exiting) */

/* Installed apps, read from the camera every 5 s.
 * kit_app_status: "Running", "Stopped", "Idle"... or NULL when the app is not installed (g_free it). */
gchar* kit_app_status(const char* id);
gchar* kit_app_name(const char* id);   /* the app's nice name, or NULL; g_free it */

/* Called on the main loop when an installed app's status changes (not on the first read). */
typedef void (*KitAppChanged)(const char* id, const char* name, const char* was, const char* now);
void kit_on_app_change(KitAppChanged cb);

/* Config export / import (kit_configio.c, kit 1.3.0): the configio endpoint copies the app's settings to
 * other cameras, by hand (the page's Kit.configio panel) or from a fleet tool; the HTTP contract is at the
 * top of kit_configio.c. The app splits its settings into sections and adds each
 * once at start, after kit_init(); the kit does the file, merge, checks-first, undo copy and log line.
 *
 *   static const char* const sendoutSecrets[] = { "mqtt.password", NULL };
 *   static const KitConfigSection sendout = { "sendout", "Send out", 0, sendoutSecrets, NULL,
 *                                             Sendout_Get, Sendout_Check, Sendout_Apply };
 *   kit_configio_section(&sendout);
 *
 * id       the key in the file ("rules"): letters, digits, _ and -
 * name     shown on the page and in messages ("Rules")
 * list     1: a list of objects matched by their "name" (merge adds or changes, never removes; a file
 *          with two items of the same name is refused by Merge, taken as it is by Replace; names match
 *          without spaces at the ends; the camera's own list may hold two of a name: Merge matches the first)
 *          0: an object (merge changes only the keys in the file, deep)
 * secrets  object sections: NULL-terminated dot paths inside the section that never leave the camera
 *          ("mqtt.password"); export lists the set ones in "omitted"; an import keeps the camera's value
 *          unless the request gives "secrets": {"<id>.<path>": "..."}. NULL for none. A list section with
 *          secrets is refused (kit_configio_section returns 0 and logs why): it would export them.
 * strip    list sections: NULL-terminated item fields that are runtime only ("id", "status"); left out
 *          of the export, and an imported item named like one on the camera keeps that one's. NULL for none.
 * get      a new copy of the current value, secrets included (the kit removes them); the kit deletes it.
 * check    the app's own validation of a proposed value (the same as its save endpoint uses). It may change
 *          the value (fill in ids, drop what this camera can't use) and add plain-words notes with
 *          kit_configio_note(). Returns NULL when it may be saved, else the reason (g_strdup'ed; the kit frees it).
 * apply    saves the value through the app's own save path and starts using it; 1 = saved. The value
 *          stays the kit's: duplicate what you keep. An apply that fails should leave the old value.
 * The callbacks run on the web thread (one request at a time); lock what the main loop also reads.
 * The strings and arrays in a section must stay for the life of the app (static). Up to 8 sections. */
typedef struct {
	const char* id;
	const char* name;
	int list;
	const char* const* secrets;
	const char* const* strip;
	cJSON* (*get)(void);
	gchar* (*check)(cJSON* value, cJSON* notes);
	int (*apply)(cJSON* value);
} KitConfigSection;
int kit_configio_section(const KitConfigSection* section);   /* 1 = added */
void kit_configio_note(cJSON* notes, const char* fmt, ...) G_GNUC_PRINTF(2, 3);

/* Internal: one init per part */
void kit_resources_init(void);
void kit_log_init(const char* appName);
void kit_apps_init(const char* appPackage);
void kit_configio_init(const char* appPackage, const char* appName);

#endif
