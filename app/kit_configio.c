/*
 * kit_configio: copy an app's settings to other cameras, by hand or from a fleet tool.
 * The HTTP contract below is fixed: other tools (fleet installers) rely on it.
 * The app gives its sections with kit_configio_section() (see kit.h); this file does the rest:
 *
 *   GET  configio?action=sections                         the sections, their counts, the undo state
 *   GET  configio?action=export[&sections=a,b]            the file (format 2), secrets left out
 *   POST configio?action=import&mode=replace|merge[&sections=a,b][&dry=1]   body = the file (JSON, max 256 KB)
 *   POST configio?action=undo                             puts back localdata/before-import.json
 *
 * Every answer is JSON. A refusal is HTTP 400 {"ok":false,"error":"...","sections":[...]} and writes nothing.
 * An import checks every chosen section first (the app's own check), saves the current values of those
 * sections (with secrets) to localdata/before-import.json (0600), then applies them one by one; if one
 * fails, the ones already written are put back. If a put-back fails too, the undo copy is kept (it is
 * then the only good copy). The undo copy of an earlier import is kept aside while an import runs and
 * comes back if the import fails. List sections may not have secrets. Replace takes a list exactly as
 * in the file, two items of one name included; Merge matches by name, so it refuses a file whose list
 * has two items of one name (spaces at the ends don't count). One log line per import and undo, never values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <glib.h>
#include "ACAP.h"
#include "cJSON.h"
#include "kit.h"

#define CFG_FORMAT       2
#define CFG_MAX_BODY     (256 * 1024)
#define CFG_MAX_SECTIONS 8
#define CFG_UNDO_FILE    "localdata/before-import.json"
#define CFG_PREV_FILE    "localdata/before-import.prev.json"   /* the earlier undo copy, while an import runs */

static GMutex lock;
static KitConfigSection secs[CFG_MAX_SECTIONS];
static int secCount = 0;
static gchar* ownId = NULL;      /* package name: the file's "app" */
static gchar* ownName = NULL;    /* nice name, for messages */

/*----------------------------------------------------- registering */

static gboolean good_id(const char* s) {
	if (!s || !*s || strlen(s) > 32) return FALSE;
	for (; *s; s++) if (!g_ascii_isalnum(*s) && *s != '_' && *s != '-') return FALSE;
	return TRUE;
}

int kit_configio_section(const KitConfigSection* s) {
	if (!s || !good_id(s->id) || !s->name || !s->get || !s->check || !s->apply) {
		syslog(LOG_WARNING, "configio: a section is missing its id, name or callbacks; not added");
		return 0;
	}
	if (s->list && s->secrets && s->secrets[0]) {
		/* Secrets are kept out of object sections only; a list section with secrets would export them */
		syslog(LOG_WARNING, "configio: section %s is a list and lists secrets; secrets work in object sections only. Not added", s->id);
		return 0;
	}
	g_mutex_lock(&lock);
	int ok = secCount < CFG_MAX_SECTIONS;
	for (int i = 0; ok && i < secCount; i++) if (strcmp(secs[i].id, s->id) == 0) ok = 0;
	if (ok) secs[secCount++] = *s;
	g_mutex_unlock(&lock);
	if (!ok) syslog(LOG_WARNING, "configio: section %s is already added, or there are more than %d", s->id, CFG_MAX_SECTIONS);
	return ok;
}

void kit_configio_note(cJSON* notes, const char* fmt, ...) {
	if (!cJSON_IsArray(notes) || !fmt) return;
	va_list ap;
	va_start(ap, fmt);
	gchar* text = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	cJSON_AddItemToArray(notes, cJSON_CreateString(text));
	g_free(text);
}

/* Sections are added at start and never removed, so a copy of the table is safe to use unlocked */
static int snapshot_sections(KitConfigSection* out) {
	g_mutex_lock(&lock);
	int n = secCount;
	memcpy(out, secs, sizeof(KitConfigSection) * (size_t)n);
	g_mutex_unlock(&lock);
	return n;
}

static int find_section(const KitConfigSection* list, int n, const char* id) {
	for (int i = 0; id && i < n; i++) if (strcmp(list[i].id, id) == 0) return i;
	return -1;
}

/*----------------------------------------------------- small JSON helpers */

/* A dot path inside an object ("mqtt.password"). parent=1 returns the object that holds the last key. */
static cJSON* path_walk(cJSON* obj, const char* path, int parent, const char** last) {
	gchar** parts = g_strsplit(path, ".", 0);
	int n = (int)g_strv_length(parts);
	cJSON* cur = obj;
	for (int i = 0; cur && i < n - (parent ? 1 : 0); i++)
		cur = cJSON_IsObject(cur) ? cJSON_GetObjectItemCaseSensitive(cur, parts[i]) : NULL;
	if (last) {
		const char* dot = strrchr(path, '.');
		*last = dot ? dot + 1 : path;
	}
	g_strfreev(parts);
	return cJSON_IsObject(cur) || !parent ? cur : NULL;
}

static cJSON* path_get(cJSON* obj, const char* path) { return path_walk(obj, path, 0, NULL); }

static void path_delete(cJSON* obj, const char* path) {
	const char* key;
	cJSON* p = path_walk(obj, path, 1, &key);
	if (p) cJSON_DeleteItemFromObjectCaseSensitive(p, key);
}

/* Sets path to value (taken). Only when the object that holds it exists; else value is deleted, 0. */
static int path_set(cJSON* obj, const char* path, cJSON* value) {
	const char* key;
	cJSON* p = path_walk(obj, path, 1, &key);
	if (!p) { cJSON_Delete(value); return 0; }
	if (cJSON_GetObjectItemCaseSensitive(p, key)) cJSON_ReplaceItemInObjectCaseSensitive(p, key, value);
	else cJSON_AddItemToObject(p, key, value);
	return 1;
}

static gboolean is_set(cJSON* v) {
	return v && !cJSON_IsNull(v) && !(cJSON_IsString(v) && (!v->valuestring || !*v->valuestring));
}

static const char* item_name(cJSON* it) {
	const char* n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "name"));
	return n ? n : "";
}

/* Names match without their leading and trailing spaces ("Hot" and "Hot " are one item): apps trim on save */
static size_t trimmed(const char** s) {
	const char* a = *s;
	while (g_ascii_isspace(*a)) a++;
	size_t n = strlen(a);
	while (n && g_ascii_isspace(a[n - 1])) n--;
	*s = a;
	return n;
}
static gboolean names_equal(const char* a, const char* b) {
	size_t na = trimmed(&a), nb = trimmed(&b);
	return na == nb && strncmp(a, b, na) == 0;
}
static gboolean blank(const char* s) { return trimmed(&s) == 0; }

/* The first item with that name (a camera's list may hold two of the same name; the others are left alone) */
static int index_by_name(cJSON* list, const char* name) {
	int i = 0;
	cJSON* it;
	cJSON_ArrayForEach(it, list) {
		if (names_equal(item_name(it), name)) return i;
		i++;
	}
	return -1;
}

static cJSON* item_by_name(cJSON* list, const char* name) {
	int i = index_by_name(list, name);
	return i < 0 ? NULL : cJSON_GetArrayItem(list, i);
}

/* A copy without what never leaves the camera: secrets (object sections) and runtime-only item fields (lists) */
static cJSON* clean_copy(const KitConfigSection* s, cJSON* value) {
	cJSON* c = cJSON_Duplicate(value, 1);
	if (!c) return NULL;
	if (s->list) {
		cJSON* it;
		cJSON_ArrayForEach(it, c)
			for (int k = 0; s->strip && s->strip[k]; k++) cJSON_DeleteItemFromObjectCaseSensitive(it, s->strip[k]);
	} else {
		for (int k = 0; s->secrets && s->secrets[k]; k++) path_delete(c, s->secrets[k]);
	}
	return c;
}

static void deep_merge(cJSON* dst, cJSON* src) {
	cJSON* f;
	cJSON_ArrayForEach(f, src) {
		if (!f->string) continue;
		cJSON* d = cJSON_GetObjectItemCaseSensitive(dst, f->string);
		if (cJSON_IsObject(d) && cJSON_IsObject(f)) deep_merge(d, f);
		else if (d) cJSON_ReplaceItemInObjectCaseSensitive(dst, f->string, cJSON_Duplicate(f, 1));
		else cJSON_AddItemToObject(dst, f->string, cJSON_Duplicate(f, 1));
	}
}

/* What the section becomes: the file's value applied to the current one (merge or replace).
 * Lists: items match by name; a matched item keeps the camera's runtime-only fields (its id).
 * Objects: secrets the file doesn't carry keep the camera's value. */
static cJSON* build(const KitConfigSection* s, cJSON* cur, cJSON* fromFile, int merge) {
	cJSON* in = clean_copy(s, fromFile);
	if (!in) return NULL;
	if (s->list) {
		cJSON* out = merge ? cJSON_Duplicate(cur, 1) : cJSON_CreateArray();
		/* The camera's items not yet paired: the n-th file item of a name takes the n-th camera item's
		 * runtime fields (its id), so two items of one name keep two ids */
		cJSON* pool = cJSON_Duplicate(cur, 1);
		cJSON* it;
		cJSON_ArrayForEach(it, in) {
			cJSON* copy = cJSON_Duplicate(it, 1);
			const char* name = item_name(it);
			cJSON* old = !blank(name) ? item_by_name(pool, name) : NULL;
			for (int k = 0; old && s->strip && s->strip[k]; k++) {
				cJSON* f = cJSON_GetObjectItemCaseSensitive(old, s->strip[k]);
				if (f) cJSON_AddItemToObject(copy, s->strip[k], cJSON_Duplicate(f, 1));
			}
			if (old) cJSON_Delete(cJSON_DetachItemViaPointer(pool, old));
			int at = merge && !blank(name) ? index_by_name(out, name) : -1;
			if (at >= 0) cJSON_ReplaceItemInArray(out, at, copy);
			else cJSON_AddItemToArray(out, copy);
		}
		cJSON_Delete(pool);
		cJSON_Delete(in);
		return out;
	}
	cJSON* out = in;
	if (merge) { out = cJSON_Duplicate(cur, 1); deep_merge(out, in); cJSON_Delete(in); }
	for (int k = 0; s->secrets && s->secrets[k]; k++) {
		cJSON* was = path_get(cur, s->secrets[k]);
		if (was && !path_get(out, s->secrets[k])) path_set(out, s->secrets[k], cJSON_Duplicate(was, 1));
	}
	return out;
}

/* Counts what changes from cur to next, secrets and runtime-only fields not counted.
 * List items pair up by name, each camera item used once (an equal one first), so two items of
 * one name count as two. */
static cJSON* count_entry(const KitConfigSection* s, cJSON* cur, cJSON* next, cJSON* notes) {
	int added = 0, changed = 0, removed = 0, unchanged = 0;
	cJSON* a = clean_copy(s, cur);
	cJSON* b = clean_copy(s, next);
	cJSON* x;
	if (s->list) {
		cJSON_ArrayForEach(x, b) {
			cJSON* o = NULL, *y;
			cJSON_ArrayForEach(y, a) if (names_equal(item_name(y), item_name(x)) && cJSON_Compare(y, x, 1)) { o = y; break; }
			if (o) unchanged++;
			else if ((o = item_by_name(a, item_name(x)))) changed++;
			else added++;
			if (o) cJSON_Delete(cJSON_DetachItemViaPointer(a, o));
		}
		removed = cJSON_GetArraySize(a);
	} else {
		cJSON_ArrayForEach(x, b) {
			cJSON* o = x->string ? cJSON_GetObjectItemCaseSensitive(a, x->string) : NULL;
			if (!o) added++;
			else if (cJSON_Compare(o, x, 1)) unchanged++;
			else changed++;
		}
		cJSON_ArrayForEach(x, a) if (!x->string || !cJSON_GetObjectItemCaseSensitive(b, x->string)) removed++;
	}
	cJSON_Delete(a);
	cJSON_Delete(b);
	cJSON* e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "id", s->id);
	cJSON_AddStringToObject(e, "name", s->name);
	cJSON_AddNumberToObject(e, "added", added);
	cJSON_AddNumberToObject(e, "changed", changed);
	cJSON_AddNumberToObject(e, "removed", removed);
	cJSON_AddNumberToObject(e, "unchanged", unchanged);
	cJSON_AddItemToObject(e, "notes", notes ? notes : cJSON_CreateArray());
	return e;
}

static gchar* counts_text(cJSON* entries) {
	GString* t = g_string_new("");
	cJSON* e;
	cJSON_ArrayForEach(e, entries) {
		g_string_append_printf(t, "%s%s: %d added, %d changed, %d removed", t->len ? "; " : "",
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "name")),
			(int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "added")), (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "changed")),
			(int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(e, "removed")));
	}
	return g_string_free(t, FALSE);
}

/*----------------------------------------------------- files and answers */

static void iso_now(char* buf, size_t n) {
	time_t t = time(NULL);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static const char* app_version(void) {
	cJSON* m = ACAP_Get_Config("manifest");
	const char* v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "acapPackageConf"), "setup"), "version"));
	return v ? v : "";
}

/* The undo copy holds secrets: written 0600 through a temporary file, so it is whole or absent */
static int write_private(const char* rel, cJSON* obj) {
	char* text = cJSON_PrintUnformatted(obj);
	if (!text) return 0;
	gchar* path = g_strconcat(ACAP_FILE_AppPath(), rel, NULL);
	gchar* tmp = g_strconcat(path, ".tmp", NULL);
	int ok = 0;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		size_t len = strlen(text), done = 0;
		ok = fchmod(fd, 0600) == 0;
		while (ok && done < len) {
			ssize_t w = write(fd, text + done, len - done);
			if (w <= 0) ok = 0; else done += (size_t)w;
		}
		if (ok) ok = fsync(fd) == 0;
		if (close(fd) != 0) ok = 0;
		if (ok) ok = rename(tmp, path) == 0;
		if (!ok) unlink(tmp);
	}
	free(text);
	g_free(tmp);
	g_free(path);
	return ok;
}

static void respond(ACAP_HTTP_Response r, int code, cJSON* out) {
	char* s = cJSON_PrintUnformatted(out);
	if (!s) { ACAP_HTTP_Respond_Error(r, 500, "Out of memory"); return; }
	ACAP_HTTP_Respond_String(r, "Status: %d %s\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-cache\r\n\r\n",
		code, code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 405 ? "Method Not Allowed" : "Internal Server Error");
	ACAP_HTTP_Respond_Data(r, strlen(s), s);
	free(s);
}

/* A refusal: nothing was written. sections (taken) may be NULL. */
static void refuse(ACAP_HTTP_Response r, int code, const char* error, cJSON* sections) {
	cJSON* out = cJSON_CreateObject();
	cJSON_AddBoolToObject(out, "ok", 0);
	cJSON_AddStringToObject(out, "error", error);
	cJSON_AddItemToObject(out, "sections", sections ? sections : cJSON_CreateArray());
	respond(r, code, out);
	cJSON_Delete(out);
}

/* "a,b" -> list of ids (NULL when absent). Bad ids are kept as given so the caller can name them. */
static gchar** param_list(ACAP_HTTP_Request req, const char* name) {
	char* v = ACAP_HTTP_Request_Param(req, name);
	if (!v) return NULL;
	gchar** parts = g_strsplit(v, ",", 17);
	free(v);
	for (int i = 0; parts[i]; i++) g_strstrip(parts[i]);
	return parts;
}

static gboolean in_list(gchar** list, const char* id) {
	for (int i = 0; list && list[i]; i++) if (strcmp(list[i], id) == 0) return TRUE;
	return FALSE;
}

/* Short and printable, for echoing a value from a file in a message */
static gchar* shown(const char* s) {
	gchar* c = g_strndup(s ? s : "", 40);
	for (char* p = c; *p; p++) if ((unsigned char)*p < 32 || *p == '"') *p = '?';
	return c;
}

static cJSON* undo_info(void) {
	cJSON* u = cJSON_CreateObject();
	cJSON* snap = ACAP_FILE_Exists(CFG_UNDO_FILE) ? ACAP_FILE_Read(CFG_UNDO_FILE) : NULL;
	cJSON* s = cJSON_GetObjectItemCaseSensitive(snap, "sections");
	cJSON_AddBoolToObject(u, "available", cJSON_IsObject(s));
	if (cJSON_IsObject(s)) {
		const char* t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snap, "time"));
		const char* m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snap, "mode"));
		cJSON_AddStringToObject(u, "time", t ? t : "");
		cJSON_AddStringToObject(u, "mode", m ? m : "");
		cJSON* ids = cJSON_AddArrayToObject(u, "sections");
		cJSON* x;
		cJSON_ArrayForEach(x, s) if (x->string) cJSON_AddItemToArray(ids, cJSON_CreateString(x->string));
	}
	cJSON_Delete(snap);
	return u;
}

/*----------------------------------------------------- actions */

static void act_sections(ACAP_HTTP_Response r) {
	KitConfigSection list[CFG_MAX_SECTIONS];
	int n = snapshot_sections(list);
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "app", ownId);
	cJSON_AddStringToObject(out, "appVersion", app_version());
	cJSON_AddNumberToObject(out, "format", CFG_FORMAT);
	cJSON* arr = cJSON_AddArrayToObject(out, "sections");
	for (int i = 0; i < n; i++) {
		cJSON* v = list[i].get();
		cJSON* e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "id", list[i].id);
		cJSON_AddStringToObject(e, "name", list[i].name);
		cJSON_AddBoolToObject(e, "list", list[i].list);
		cJSON_AddNumberToObject(e, "count", cJSON_GetArraySize(v));
		cJSON* sec = cJSON_AddArrayToObject(e, "secrets");
		for (int k = 0; !list[i].list && list[i].secrets && list[i].secrets[k]; k++) cJSON_AddItemToArray(sec, cJSON_CreateString(list[i].secrets[k]));
		cJSON_AddItemToArray(arr, e);
		cJSON_Delete(v);
	}
	cJSON_AddItemToObject(out, "undo", undo_info());
	respond(r, 200, out);
	cJSON_Delete(out);
}

static void act_export(ACAP_HTTP_Response r, ACAP_HTTP_Request req) {
	KitConfigSection list[CFG_MAX_SECTIONS];
	int n = snapshot_sections(list);
	gchar** want = param_list(req, "sections");
	for (int i = 0; want && want[i]; i++) {
		if (*want[i] && find_section(list, n, want[i]) < 0) {
			gchar* id = shown(want[i]);
			gchar* msg = g_strdup_printf("%s has no section called '%s'. Leave out sections to export them all.", ownName, id);
			refuse(r, 400, msg, NULL);
			g_free(msg); g_free(id); g_strfreev(want);
			return;
		}
	}
	char now[32];
	iso_now(now, sizeof now);
	const char* model = ACAP_DEVICE_Prop("model");
	cJSON* out = cJSON_CreateObject();
	cJSON_AddStringToObject(out, "kind", "acap-config");
	cJSON_AddNumberToObject(out, "format", CFG_FORMAT);
	cJSON_AddStringToObject(out, "app", ownId);
	cJSON_AddStringToObject(out, "appVersion", app_version());
	cJSON_AddStringToObject(out, "exported", now);
	cJSON_AddStringToObject(out, "fromModel", model ? model : "");
	cJSON* body = cJSON_AddObjectToObject(out, "sections");
	cJSON* omitted = cJSON_AddArrayToObject(out, "omitted");
	for (int i = 0; i < n; i++) {
		if (want && want[0] && *want[0] && !in_list(want, list[i].id)) continue;
		cJSON* v = list[i].get();
		if (!v) continue;
		for (int k = 0; !list[i].list && list[i].secrets && list[i].secrets[k]; k++) {
			if (!is_set(path_get(v, list[i].secrets[k]))) continue;
			gchar* p = g_strdup_printf("%s.%s", list[i].id, list[i].secrets[k]);
			cJSON_AddItemToArray(omitted, cJSON_CreateString(p));
			g_free(p);
		}
		cJSON_AddItemToObject(body, list[i].id, clean_copy(&list[i], v));
		cJSON_Delete(v);
	}
	g_strfreev(want);
	respond(r, 200, out);
	cJSON_Delete(out);
}

/* Applies the values in order; on a failure puts back the ones already written (from `before`).
 * Returns the index that failed, or -1. *putBackFailed: the name of a section that could not be
 * put back either (the undo copy is then the only good copy), or NULL. */
static int apply_all(KitConfigSection* list, int* idx, cJSON** values, cJSON** before, int n, const char** putBackFailed) {
	*putBackFailed = NULL;
	for (int i = 0; i < n; i++) {
		if (list[idx[i]].apply(values[i])) continue;
		for (int j = i; j >= 0; j--) {
			if (list[idx[j]].apply(before[j])) continue;
			syslog(LOG_ERR, "configio: could not put back %s", list[idx[j]].id);
			*putBackFailed = list[idx[j]].name;
		}
		return i;
	}
	return -1;
}

/* The undo copy of the import before this one is kept aside while an import runs, so a failed
 * import leaves the earlier one undoable. Both files hold secrets: 0600. */
static gchar* app_path(const char* rel) { return g_strconcat(ACAP_FILE_AppPath(), rel, NULL); }
static void move_file(const char* fromRel, const char* toRel) {
	gchar* from = app_path(fromRel);
	gchar* to = app_path(toRel);
	if (access(from, F_OK) == 0) {
		if (rename(from, to) == 0) chmod(to, 0600);
		else syslog(LOG_ERR, "configio: could not move %s", fromRel);
	}
	g_free(from);
	g_free(to);
}

/* A file's list whose items share a name (trimmed) is refused in Merge, which matches items by name.
 * Replace and undo take two of a name as they are (counts pair them by occurrence). */
static gchar* duplicate_name(cJSON* items) {
	GHashTable* seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gchar* dup = NULL;
	cJSON* it;
	cJSON_ArrayForEach(it, items) {
		const char* n = item_name(it);
		size_t len = trimmed(&n);
		if (!len) continue;
		gchar* key = g_strndup(n, len);
		if (g_hash_table_contains(seen, key)) { dup = shown(key); g_free(key); break; }
		g_hash_table_add(seen, key);
	}
	g_hash_table_destroy(seen);
	return dup;
}

/* The first key of an object that appears twice when case is ignored (shown form), or NULL */
static gchar* key_twice(cJSON* obj) {
	if (!cJSON_IsObject(obj)) return NULL;
	GHashTable* seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gchar* dup = NULL;
	cJSON* k;
	cJSON_ArrayForEach(k, obj) {
		if (!k->string) continue;
		gchar* low = g_ascii_strdown(k->string, -1);
		if (g_hash_table_contains(seen, low)) { dup = shown(k->string); g_free(low); break; }
		g_hash_table_add(seen, low);
	}
	g_hash_table_destroy(seen);
	return dup;
}

static void act_import(ACAP_HTTP_Response r, ACAP_HTTP_Request req) {
	KitConfigSection list[CFG_MAX_SECTIONS];
	int n = snapshot_sections(list);
	if (ACAP_HTTP_Get_Content_Length(req) > CFG_MAX_BODY) { refuse(r, 400, "The file is too big: the limit is 256 KB.", NULL); return; }
	const char* body = ACAP_HTTP_Get_Body(req);
	cJSON* file = body ? cJSON_ParseWithLength(body, ACAP_HTTP_Get_Body_Length(req)) : NULL;
	if (!cJSON_IsObject(file)) {
		refuse(r, 400, body ? "That is not a settings file: it is not JSON. Choose a file exported from this app." : "Send the settings file as the request body.", NULL);
		cJSON_Delete(file);
		return;
	}

	char* modeP = ACAP_HTTP_Request_Param(req, "mode");
	char* dryP = ACAP_HTTP_Request_Param(req, "dry");
	int merge = modeP && strcmp(modeP, "merge") == 0;
	int badMode = modeP && !merge && strcmp(modeP, "replace") != 0;
	int dry = dryP && (strcmp(dryP, "1") == 0 || strcmp(dryP, "true") == 0);
	free(modeP);
	free(dryP);
	gchar** want = param_list(req, "sections");
	cJSON* notes = cJSON_CreateArray();
	cJSON* entries = cJSON_CreateArray();
	cJSON* fileSecs = NULL;          /* the file's sections, as an object */
	cJSON* values[CFG_MAX_SECTIONS] = { 0 }, *before[CFG_MAX_SECTIONS] = { 0 };
	int idx[CFG_MAX_SECTIONS], chosen = 0;
	gchar* error = NULL;

	/* Which app and format */
	const char* fileApp = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, "app"));
	cJSON* kind = cJSON_GetObjectItemCaseSensitive(file, "kind");
	cJSON* fmtItem = cJSON_GetObjectItemCaseSensitive(file, "format");
	double format = cJSON_IsNumber(fmtItem) ? cJSON_GetNumberValue(fmtItem) : (fmtItem || kind ? 0 : 1);
	/* Keys are read case-sensitively; a key twice, or twice in different case ("secrets" and "Secrets"),
	 * could carry values a tool reading the file another way would miss: refused */
	gchar* twice = key_twice(file);
	if (!twice) twice = key_twice(cJSON_GetObjectItemCaseSensitive(file, "sections"));
	if (!twice) twice = key_twice(cJSON_GetObjectItemCaseSensitive(file, "secrets"));
	if (twice) {
		error = g_strdup_printf("That file has the key '%s' twice (perhaps in different case). Keep one and try again.", twice);
		g_free(twice);
	}
	else if (badMode) error = g_strdup("mode must be replace or merge.");
	else if (!fileApp) error = g_strdup("That is not a settings file: it does not say which app it is for.");
	else if (strcmp(fileApp, ownId) != 0) {
		gchar* a = shown(fileApp);
		error = g_strdup_printf("This file is for another app (%s), not %s. Export the file from %s on the other camera.", a, ownName, ownName);
		g_free(a);
	}
	else if (format > CFG_FORMAT) error = g_strdup_printf("This file is from a newer version of %s: update the app first.", ownName);
	else if (format != 1 && format != 2) error = g_strdup("That is not a settings file: its format is not one this app reads.");
	else if (format == 2 && (!cJSON_IsString(kind) || strcmp(kind->valuestring, "acap-config") != 0))
		error = g_strdup("That is not a settings file: it has no \"kind\": \"acap-config\".");
	else if (format == 2 && !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(file, "sections")))
		error = g_strdup("That settings file has no sections.");
	if (error) goto refused;

	/* Format 1 (the older per-page exports): each list the app knows is one section */
	if (format == 1) {
		fileSecs = cJSON_CreateObject();
		for (int i = 0; i < n; i++) {
			cJSON* l = cJSON_GetObjectItemCaseSensitive(file, list[i].id);
			if (list[i].list && cJSON_IsArray(l)) cJSON_AddItemToObject(fileSecs, list[i].id, cJSON_Duplicate(l, 1));
		}
	} else {
		fileSecs = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(file, "sections"), 1);
	}

	/* Which sections: the file's, that the app knows, that were asked for */
	cJSON* fs;
	cJSON_ArrayForEach(fs, fileSecs) {
		if (!fs->string) continue;
		if (find_section(list, n, fs->string) < 0) {
			gchar* id = shown(fs->string);
			kit_configio_note(notes, "The file's section '%s' is not used by %s.", id, ownName);
			g_free(id);
		}
	}
	for (int i = 0; want && want[i]; i++) {
		if (!*want[i] || cJSON_GetObjectItemCaseSensitive(fileSecs, want[i])) continue;
		gchar* id = shown(want[i]);
		kit_configio_note(notes, "The file has no section '%s'.", id);
		g_free(id);
	}
	for (int i = 0; i < n && chosen < CFG_MAX_SECTIONS; i++) {
		if (!cJSON_GetObjectItemCaseSensitive(fileSecs, list[i].id)) continue;
		if (want && want[0] && *want[0] && !in_list(want, list[i].id)) continue;
		idx[chosen++] = i;
	}
	if (!chosen) { error = g_strdup_printf("Nothing to import: the file has none of the sections asked for, or none %s uses.", ownName); goto refused; }

	/* Secrets given with the request: "<section>.<path>": "value", used on a real apply only */
	cJSON* given = cJSON_GetObjectItemCaseSensitive(file, "secrets");
	if (given && !cJSON_IsObject(given)) { error = g_strdup("secrets must be an object of \"section.path\": \"value\"."); goto refused; }
	cJSON* g;
	cJSON_ArrayForEach(g, given) {
		int known = 0;
		for (int c = 0; g->string && c < chosen && !known; c++) {
			const KitConfigSection* s = &list[idx[c]];
			size_t L = strlen(s->id);
			for (int k = 0; !s->list && s->secrets && s->secrets[k] && !known; k++)
				known = strncmp(g->string, s->id, L) == 0 && g->string[L] == '.' && strcmp(g->string + L + 1, s->secrets[k]) == 0;
		}
		if (!cJSON_IsString(g) || strlen(g->valuestring) > 4096) { error = g_strdup("Each secret must be a text of at most 4096 characters."); goto refused; }
		if (!known) {
			gchar* id = shown(g->string);
			kit_configio_note(notes, "'%s' is not a secret of the chosen sections; it was not used.", id);
			g_free(id);
		}
	}

	/* Check every chosen section before anything is written */
	for (int c = 0; c < chosen; c++) {
		const KitConfigSection* s = &list[idx[c]];
		cJSON* v = cJSON_GetObjectItemCaseSensitive(fileSecs, s->id);
		int okType = s->list ? cJSON_IsArray(v) : cJSON_IsObject(v);
		cJSON* it;
		if (okType && s->list) cJSON_ArrayForEach(it, v) if (!cJSON_IsObject(it)) okType = 0;
		if (!okType) { error = g_strdup_printf("%s: the file's section is not %s.", s->name, s->list ? "a list of items" : "a group of settings"); goto refused; }
		/* Merge matches by name and can't tell two items of one name apart; Replace takes them as they are */
		gchar* dup = s->list && merge ? duplicate_name(v) : NULL;
		if (dup) { error = g_strdup_printf("%s: the file has two items called '%s', and Merge matches items by name. Use Replace, or rename one in the file.", s->name, dup); g_free(dup); goto refused; }
		before[c] = s->get();
		if (!before[c]) { error = g_strdup_printf("%s: could not read the camera's current settings. Nothing was changed.", s->name); goto refused; }
		values[c] = build(s, before[c], v, merge);
		cJSON* secNotes = cJSON_CreateArray();
		size_t L = strlen(s->id);
		cJSON_ArrayForEach(g, given) {
			if (!g->string || strncmp(g->string, s->id, L) != 0 || g->string[L] != '.') continue;
			const char* path = g->string + L + 1;
			int known = 0;
			for (int k = 0; !s->list && s->secrets && s->secrets[k]; k++) if (strcmp(s->secrets[k], path) == 0) known = 1;
			if (!known) continue;
			gchar* p = shown(path);
			if (dry) kit_configio_note(secNotes, "A new value for %s is used on Apply.", p);
			else if (!path_set(values[c], path, cJSON_CreateString(g->valuestring)))
				kit_configio_note(secNotes, "The value given for %s was not used: the file has nothing it belongs to.", p);
			g_free(p);
		}
		gchar* err = s->check(values[c], secNotes);
		cJSON_AddItemToArray(entries, count_entry(s, before[c], values[c], secNotes));
		if (err) { error = g_strdup_printf("%s: %s", s->name, err); g_free(err); goto refused; }
	}

	if (!dry) {
		char now[32];
		iso_now(now, sizeof now);
		cJSON* snap = cJSON_CreateObject();
		cJSON_AddStringToObject(snap, "time", now);
		cJSON_AddStringToObject(snap, "mode", merge ? "merge" : "replace");
		cJSON* ss = cJSON_AddObjectToObject(snap, "sections");
		for (int c = 0; c < chosen; c++) cJSON_AddItemToObject(ss, list[idx[c]].id, cJSON_Duplicate(before[c], 1));
		move_file(CFG_UNDO_FILE, CFG_PREV_FILE);   /* the earlier import stays undoable if this one fails */
		int saved = write_private(CFG_UNDO_FILE, snap);
		cJSON_Delete(snap);
		if (!saved) {
			move_file(CFG_PREV_FILE, CFG_UNDO_FILE);
			error = g_strdup("Could not save the undo copy on the camera (is its storage full?). Nothing was changed.");
			goto refused;
		}
		const char* putBackFailed = NULL;
		int failed = apply_all(list, idx, values, before, chosen, &putBackFailed);
		gchar* counts = counts_text(entries);
		const char* user = ACAP_HTTP_Get_User(req);
		if (failed >= 0) {
			if (putBackFailed) {
				/* The undo copy is now the only good copy of the old settings: keep it */
				kit_log("Settings", "Import failed and could not be put back", list[idx[failed]].name, user);
				error = g_strdup_printf("%s: the camera could not save it, and putting back %s failed too. "
					"Undo last import is still available: use it to put back the settings from before this import.", list[idx[failed]].name, putBackFailed);
			} else {
				ACAP_FILE_Delete(CFG_UNDO_FILE);
				move_file(CFG_PREV_FILE, CFG_UNDO_FILE);
				kit_log("Settings", "Import failed and was put back", list[idx[failed]].name, user);
				error = g_strdup_printf("%s: the camera could not save it. The sections already written were put back; nothing changed.", list[idx[failed]].name);
			}
			g_free(counts);
			for (int c = 0; c < chosen; c++) { cJSON_Delete(values[c]); cJSON_Delete(before[c]); }
			refuse(r, 500, error, entries);
			g_free(error);
			cJSON_Delete(notes); cJSON_Delete(fileSecs); cJSON_Delete(file); g_strfreev(want);
			return;
		}
		if (ACAP_FILE_Exists(CFG_PREV_FILE)) ACAP_FILE_Delete(CFG_PREV_FILE);
		kit_log("Settings", merge ? "Imported settings (Merge)" : "Imported settings (Replace)", counts, user);
		g_free(counts);
	}

	cJSON* out = cJSON_CreateObject();
	cJSON_AddBoolToObject(out, "ok", 1);
	cJSON_AddBoolToObject(out, "dry", dry);
	cJSON_AddStringToObject(out, "mode", merge ? "merge" : "replace");
	cJSON_AddItemToObject(out, "sections", entries);
	cJSON_AddItemToObject(out, "notes", notes);
	respond(r, 200, out);
	cJSON_Delete(out);
	for (int c = 0; c < chosen; c++) { cJSON_Delete(values[c]); cJSON_Delete(before[c]); }
	cJSON_Delete(fileSecs); cJSON_Delete(file); g_strfreev(want);
	return;

refused:
	refuse(r, 400, error, entries);
	g_free(error);
	for (int c = 0; c < chosen; c++) { cJSON_Delete(values[c]); cJSON_Delete(before[c]); }
	cJSON_Delete(notes); cJSON_Delete(fileSecs); cJSON_Delete(file); g_strfreev(want);
}

static void act_undo(ACAP_HTTP_Response r, ACAP_HTTP_Request req) {
	KitConfigSection list[CFG_MAX_SECTIONS];
	int n = snapshot_sections(list);
	cJSON* snap = ACAP_FILE_Exists(CFG_UNDO_FILE) ? ACAP_FILE_Read(CFG_UNDO_FILE) : NULL;
	cJSON* ss = cJSON_GetObjectItemCaseSensitive(snap, "sections");
	if (!cJSON_IsObject(ss)) { refuse(r, 400, "There is no import to undo.", NULL); cJSON_Delete(snap); return; }
	const char* mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snap, "mode"));
	cJSON* notes = cJSON_CreateArray();
	cJSON* entries = cJSON_CreateArray();
	cJSON* values[CFG_MAX_SECTIONS] = { 0 }, *before[CFG_MAX_SECTIONS] = { 0 };
	int idx[CFG_MAX_SECTIONS], chosen = 0;
	gchar* error = NULL;
	cJSON* x;
	cJSON_ArrayForEach(x, ss) {
		int i = x->string ? find_section(list, n, x->string) : -1;
		if (i < 0 || chosen >= CFG_MAX_SECTIONS) { gchar* id = shown(x->string); kit_configio_note(notes, "Section '%s' is no longer in %s; it was skipped.", id, ownName); g_free(id); continue; }
		const KitConfigSection* s = &list[i];
		before[chosen] = s->get();
		values[chosen] = cJSON_Duplicate(x, 1);
		idx[chosen] = i;
		chosen++;
		if (!before[chosen - 1]) { error = g_strdup_printf("%s: could not read the camera's current settings. Nothing was changed.", s->name); break; }
		cJSON* secNotes = cJSON_CreateArray();
		gchar* err = s->check(values[chosen - 1], secNotes);
		cJSON_AddItemToArray(entries, count_entry(s, before[chosen - 1], values[chosen - 1], secNotes));
		if (err) { error = g_strdup_printf("%s: %s", s->name, err); g_free(err); break; }
	}
	const char* putBackFailed = NULL;
	int failed = error ? -1 : apply_all(list, idx, values, before, chosen, &putBackFailed);
	if (!error && failed >= 0)
		error = putBackFailed ? g_strdup_printf("%s: the camera could not save it, and putting back %s failed too. The undo copy is kept: try Undo last import again.", list[idx[failed]].name, putBackFailed)
			: g_strdup_printf("%s: the camera could not save it. Nothing changed; the undo copy is kept.", list[idx[failed]].name);
	if (error) {
		refuse(r, failed >= 0 ? 500 : 400, error, entries);
	} else {
		/* Done; an earlier import kept aside by a failed one becomes the one to undo again */
		ACAP_FILE_Delete(CFG_UNDO_FILE);
		move_file(CFG_PREV_FILE, CFG_UNDO_FILE);
		gchar* counts = counts_text(entries);
		kit_log("Settings", "Undid the last import", counts, ACAP_HTTP_Get_User(req));
		g_free(counts);
		cJSON* out = cJSON_CreateObject();
		cJSON_AddBoolToObject(out, "ok", 1);
		cJSON_AddBoolToObject(out, "dry", 0);
		cJSON_AddStringToObject(out, "mode", mode ? mode : "replace");
		cJSON_AddItemToObject(out, "sections", entries);
		cJSON_AddItemToObject(out, "notes", notes);
		notes = NULL;
		respond(r, 200, out);
		cJSON_Delete(out);
	}
	g_free(error);
	for (int c = 0; c < chosen; c++) { cJSON_Delete(values[c]); cJSON_Delete(before[c]); }
	cJSON_Delete(notes);
	cJSON_Delete(snap);
}

static void HTTP_configio(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	char* action = ACAP_HTTP_Request_Param(request, "action");
	int isPost = method && strcmp(method, "POST") == 0;
	int isGet = method && strcmp(method, "GET") == 0;
	if (action && (strcmp(action, "import") == 0 || strcmp(action, "undo") == 0)) {
		/* JSON only, so no form or link from another site can send it */
		const char* type = ACAP_HTTP_Get_Content_Type(request);
		if (!isPost) refuse(response, 405, "Use POST with the settings file as JSON.", NULL);
		else if (!type || !strstr(type, "application/json")) refuse(response, 400, "Send JSON (Content-Type: application/json).", NULL);
		else if (strcmp(action, "import") == 0) act_import(response, request);
		else act_undo(response, request);
	} else if (action && (strcmp(action, "sections") == 0 || strcmp(action, "export") == 0)) {
		if (!isGet) refuse(response, 405, "Use GET.", NULL);
		else if (strcmp(action, "sections") == 0) act_sections(response);
		else act_export(response, request);
	} else {
		refuse(response, 400, "action must be sections, export, import or undo.", NULL);
	}
	free(action);
}

void kit_configio_init(const char* appPackage, const char* appName) {
	g_mutex_init(&lock);
	ownId = g_strdup(appPackage);
	ownName = g_strdup(appName);
	ACAP_HTTP_Node("configio", HTTP_configio);
}
