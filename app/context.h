/*
 * context.h - fills in an alert's Context text (the same engine as Data Bender's).
 *
 * Text is copied as is, except {...}, which holds a small formula:
 *   {count}  {time}  {camera}             fields (see CtxVar)
 *   {count * 2}  {count % 100 + 1}        + - * / %  and ( )
 *   {if(count > 10, "busy", "normal")}    if(test, then, else)
 *   {upper(object)} {lower(rule)}         text case
 *   {field:port}                          any field of the source event
 *   {round(count / 3, 1)} {pad(count, 4)} rounding, leading zeros
 *   comparisons: > < >= <= == !=   text in "double quotes"
 * A formula that can't be read becomes "{?}". An unknown field is empty.
 * {{ and }} give a literal { and }.
 */
#ifndef RM_CONTEXT_H
#define RM_CONTEXT_H

#include <glib.h>

typedef struct {
	const char* name;   /* e.g. "count" */
	const char* text;   /* value as text (used when isNum is 0) */
	double      num;    /* value as a number (used when isNum is 1) */
	int         isNum;
} CtxVar;

/* Returns the filled-in text; free with g_free. */
gchar* ctx_render(const char* tmpl, const CtxVar* vars, int nvars);

#endif
