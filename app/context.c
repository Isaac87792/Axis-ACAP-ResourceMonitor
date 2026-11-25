/*
 * context.c - a small formula engine for the Context text. See context.h.
 * Recursive descent over one {...} at a time; values are numbers or text.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "context.h"

#define MAX_DEPTH 32

typedef struct { int isNum; double num; GString* str; } Val;

typedef struct {
	const char* p;
	const CtxVar* vars;
	int nvars;
	int err;
	int depth;
} Parser;

static Val v_num(double d) { Val v = { 1, d, NULL }; return v; }
static Val v_str(const char* s) { Val v = { 0, 0, g_string_new(s ? s : "") }; return v; }
static void v_free(Val* v) { if (v->str) g_string_free(v->str, TRUE); v->str = NULL; }

/* Number as text: whole numbers without decimals */
static void num_text(GString* out, double d) {
	if (isnan(d) || isinf(d)) { g_string_append(out, "?"); return; }
	if (fabs(d - (double)(long long)d) < 1e-9 && fabs(d) < 1e15) g_string_append_printf(out, "%lld", (long long)d);
	else {
		char buf[64];
		snprintf(buf, sizeof(buf), "%.6f", d);
		char* e = buf + strlen(buf) - 1;          /* trim trailing zeros */
		while (e > buf && *e == '0') *e-- = '\0';
		if (*e == '.') *e = '\0';
		g_string_append(out, buf);
	}
}

static gchar* v_text(const Val* v) {
	if (!v->isNum) return g_strdup(v->str ? v->str->str : "");
	GString* s = g_string_new("");
	num_text(s, v->num);
	return g_string_free(s, FALSE);
}

static double v_number(const Val* v) {
	if (v->isNum) return v->num;
	if (!v->str || !*v->str->str) return 0;
	char* end;
	double d = strtod(v->str->str, &end);
	return (*end == '\0') ? d : 0;
}

static int v_true(const Val* v) {
	if (v->isNum) return v->num != 0;
	return v->str && *v->str->str && strcmp(v->str->str, "0") != 0;
}

static void skip(Parser* ps) { while (*ps->p == ' ' || *ps->p == '\t') ps->p++; }

static Val parse_expr(Parser* ps);

static Val lookup(Parser* ps, const char* name) {
	for (int i = 0; i < ps->nvars; i++)
		if (g_ascii_strcasecmp(ps->vars[i].name, name) == 0)
			return ps->vars[i].isNum ? v_num(ps->vars[i].num) : v_str(ps->vars[i].text);
	return v_str("");
}

static Val call(Parser* ps, const char* fn, Val* args, int n) {
	if (!g_ascii_strcasecmp(fn, "if") && n == 3) {
		Val r = v_true(&args[0]) ? args[1] : args[2];
		Val out = r.isNum ? v_num(r.num) : v_str(r.str ? r.str->str : "");
		return out;
	}
	if ((!g_ascii_strcasecmp(fn, "upper") || !g_ascii_strcasecmp(fn, "lower")) && n == 1) {
		gchar* t = v_text(&args[0]);
		gchar* c = !g_ascii_strcasecmp(fn, "upper") ? g_utf8_strup(t, -1) : g_utf8_strdown(t, -1);
		Val out = v_str(c);
		g_free(t); g_free(c);
		return out;
	}
	if (!g_ascii_strcasecmp(fn, "round") && (n == 1 || n == 2)) {
		double f = pow(10, n == 2 ? floor(v_number(&args[1])) : 0);
		return v_num(round(v_number(&args[0]) * f) / f);
	}
	if (!g_ascii_strcasecmp(fn, "pad") && n == 2) {
		gchar* t = v_text(&args[0]);
		int w = (int)v_number(&args[1]);
		if (w > 32) w = 32;
		GString* s = g_string_new("");
		for (int i = (int)strlen(t); i < w; i++) g_string_append_c(s, '0');
		g_string_append(s, t);
		Val out = v_str(s->str);
		g_string_free(s, TRUE); g_free(t);
		return out;
	}
	ps->err = 1;
	return v_str("");
}

static Val parse_primary(Parser* ps) {
	skip(ps);
	if (++ps->depth > MAX_DEPTH) { ps->err = 1; return v_str(""); }
	Val v;
	if (*ps->p == '(') {
		ps->p++;
		v = parse_expr(ps);
		skip(ps);
		if (*ps->p == ')') ps->p++; else ps->err = 1;
	} else if (*ps->p == '"') {
		ps->p++;
		GString* s = g_string_new("");
		while (*ps->p && *ps->p != '"') g_string_append_c(s, *ps->p++);
		if (*ps->p == '"') ps->p++; else ps->err = 1;
		v = v_str(s->str);
		g_string_free(s, TRUE);
	} else if (isdigit((unsigned char)*ps->p) || (*ps->p == '.' && isdigit((unsigned char)ps->p[1]))) {
		char* end;
		double d = strtod(ps->p, &end);
		ps->p = end;
		v = v_num(d);
	} else if (isalpha((unsigned char)*ps->p) || *ps->p == '_') {
		const char* s = ps->p;
		while (isalnum((unsigned char)*ps->p) || *ps->p == '_') ps->p++;
		gchar* name = g_strndup(s, ps->p - s);
		/* {field:port}: any field of the source event */
		if (*ps->p == ':' && !g_ascii_strcasecmp(name, "field")) {
			const char* f = ++ps->p;
			while (isalnum((unsigned char)*ps->p) || *ps->p == '_' || *ps->p == '.' || *ps->p == '-') ps->p++;
			if (ps->p == f) ps->err = 1;
			gchar* full = g_strdup_printf("field:%.*s", (int)(ps->p - f), f);
			g_free(name);
			name = full;
		}
		skip(ps);
		if (*ps->p == '(') {
			ps->p++;
			Val args[4];
			int n = 0;
			skip(ps);
			if (*ps->p != ')') {
				while (1) {
					Val a = parse_expr(ps);
					if (n < 4) args[n++] = a; else { v_free(&a); ps->err = 1; }
					skip(ps);
					if (*ps->p == ',') { ps->p++; continue; }
					break;
				}
			}
			if (*ps->p == ')') ps->p++; else ps->err = 1;
			v = ps->err ? v_str("") : call(ps, name, args, n);
			for (int i = 0; i < n; i++) v_free(&args[i]);
		} else {
			v = lookup(ps, name);
		}
		g_free(name);
	} else {
		ps->err = 1;
		v = v_str("");
	}
	ps->depth--;
	return v;
}

static Val parse_unary(Parser* ps) {
	skip(ps);
	if (*ps->p == '-') {
		ps->p++;
		Val v = parse_unary(ps);
		double d = -v_number(&v);
		v_free(&v);
		return v_num(d);
	}
	return parse_primary(ps);
}

static Val parse_term(Parser* ps) {
	Val a = parse_unary(ps);
	while (!ps->err) {
		skip(ps);
		char op = *ps->p;
		if (op != '*' && op != '/' && op != '%') break;
		ps->p++;
		Val b = parse_unary(ps);
		double x = v_number(&a), y = v_number(&b), r;
		if (op == '*') r = x * y;
		else if (y == 0) r = NAN;
		else if (op == '/') r = x / y;
		else r = fmod(x, y);
		v_free(&a); v_free(&b);
		a = v_num(r);
	}
	return a;
}

static Val parse_sum(Parser* ps) {
	Val a = parse_term(ps);
	while (!ps->err) {
		skip(ps);
		char op = *ps->p;
		if (op != '+' && op != '-') break;
		ps->p++;
		Val b = parse_term(ps);
		if (op == '+' && (!a.isNum || !b.isNum)) {   /* text + anything joins the text */
			gchar* x = v_text(&a);
			gchar* y = v_text(&b);
			gchar* j = g_strconcat(x, y, NULL);
			v_free(&a); v_free(&b);
			a = v_str(j);
			g_free(x); g_free(y); g_free(j);
		} else {
			double r = op == '+' ? v_number(&a) + v_number(&b) : v_number(&a) - v_number(&b);
			v_free(&a); v_free(&b);
			a = v_num(r);
		}
	}
	return a;
}

static Val parse_expr(Parser* ps) {
	Val a = parse_sum(ps);
	if (ps->err) return a;
	skip(ps);
	const char* p = ps->p;
	int op = 0;   /* 1 > 2 < 3 >= 4 <= 5 == 6 != */
	if (p[0] == '>' && p[1] == '=') op = 3;
	else if (p[0] == '<' && p[1] == '=') op = 4;
	else if (p[0] == '=' && p[1] == '=') op = 5;
	else if (p[0] == '!' && p[1] == '=') op = 6;
	else if (p[0] == '>') op = 1;
	else if (p[0] == '<') op = 2;
	if (!op) return a;
	ps->p += (op >= 3) ? 2 : 1;
	Val b = parse_sum(ps);
	int r;
	if ((op == 5 || op == 6) && (!a.isNum || !b.isNum)) {
		gchar* x = v_text(&a);
		gchar* y = v_text(&b);
		r = strcmp(x, y) == 0;
		if (op == 6) r = !r;
		g_free(x); g_free(y);
	} else {
		double x = v_number(&a), y = v_number(&b);
		r = op == 1 ? x > y : op == 2 ? x < y : op == 3 ? x >= y : op == 4 ? x <= y : op == 5 ? x == y : x != y;
	}
	v_free(&a); v_free(&b);
	return v_num(r);
}

gchar* ctx_render(const char* tmpl, const CtxVar* vars, int nvars) {
	GString* out = g_string_new("");
	const char* p = tmpl ? tmpl : "";
	while (*p) {
		if (p[0] == '{' && p[1] == '{') { g_string_append_c(out, '{'); p += 2; continue; }
		if (p[0] == '}' && p[1] == '}') { g_string_append_c(out, '}'); p += 2; continue; }
		if (*p != '{') { g_string_append_c(out, *p++); continue; }

		/* Find the matching }, skipping text in quotes */
		const char* s = p + 1;
		const char* e = s;
		int quoted = 0;
		while (*e && (quoted || *e != '}')) { if (*e == '"') quoted = !quoted; e++; }
		if (!*e) { g_string_append(out, p); break; }   /* no closing }: copy the rest as is */

		gchar* expr = g_strndup(s, e - s);
		Parser ps = { expr, vars, nvars, 0, 0 };
		Val v = parse_expr(&ps);
		skip(&ps);
		if (ps.err || *ps.p) g_string_append(out, "{?}");
		else { gchar* t = v_text(&v); g_string_append(out, t); g_free(t); }
		v_free(&v);
		g_free(expr);
		p = e + 1;
	}
	return g_string_free(out, FALSE);
}
