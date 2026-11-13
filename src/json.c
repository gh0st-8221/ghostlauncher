#include "json.h"
#include <string.h>
#include <stdlib.h>

#define JSON_ERR (g_quark_from_static_string("json"))

typedef struct {
	const char *p;
	const char *end;
	GError **err;
	gboolean failed;
	int depth;
} Parser;

static void fail(Parser *ps, const char *msg)
{
	if (ps->failed)
		return;
	ps->failed = TRUE;
	g_set_error(ps->err, JSON_ERR, 1, "json: %s", msg);
}

static void skip_ws(Parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

JV *jv_new(JType t)
{
	JV *v = g_new0(JV, 1);
	v->t = t;
	if (t == J_ARR)
		v->items = g_ptr_array_new();
	if (t == J_OBJ) {
		v->items = g_ptr_array_new();
		v->keys = g_ptr_array_new_with_free_func(g_free);
	}
	return v;
}

JV *jv_new_str(const char *s)
{
	JV *v = jv_new(J_STR);
	v->s = g_strdup(s ? s : "");
	return v;
}

JV *jv_new_num(double n)
{
	JV *v = jv_new(J_NUM);
	v->n = n;
	return v;
}

JV *jv_new_bool(gboolean b)
{
	JV *v = jv_new(J_BOOL);
	v->b = b;
	return v;
}

JV *jv_strv(const char *const *list)
{
	JV *a = jv_new(J_ARR);
	for (; list && *list; list++)
		jv_push(a, jv_new_str(*list));
	return a;
}

void jv_free(JV *v)
{
	if (!v)
		return;
	if (v->items) {
		for (guint i = 0; i < v->items->len; i++)
			jv_free(g_ptr_array_index(v->items, i));
		g_ptr_array_free(v->items, TRUE);
	}
	if (v->keys)
		g_ptr_array_free(v->keys, TRUE);
	g_free(v->s);
	g_free(v);
}

void jv_set(JV *o, const char *key, JV *v)
{
	for (guint i = 0; i < o->keys->len; i++) {
		if (!strcmp(g_ptr_array_index(o->keys, i), key)) {
			jv_free(g_ptr_array_index(o->items, i));
			o->items->pdata[i] = v;
			return;
		}
	}
	g_ptr_array_add(o->keys, g_strdup(key));
	g_ptr_array_add(o->items, v);
}

void jv_push(JV *a, JV *v)
{
	g_ptr_array_add(a->items, v);
}

JV *jv_get(const JV *o, const char *key)
{
	if (!o || o->t != J_OBJ)
		return NULL;
	for (guint i = 0; i < o->keys->len; i++)
		if (!strcmp(g_ptr_array_index(o->keys, i), key))
			return g_ptr_array_index(o->items, i);
	return NULL;
}

JV *jv_at(const JV *a, guint i)
{
	if (!a || a->t != J_ARR || i >= a->items->len)
		return NULL;
	return g_ptr_array_index(a->items, i);
}

guint jv_len(const JV *v)
{
	return (v && v->items) ? v->items->len : 0;
}

const char *jv_gets(const JV *o, const char *key, const char *def)
{
	JV *v = jv_get(o, key);
	return (v && v->t == J_STR) ? v->s : def;
}

double jv_getn(const JV *o, const char *key, double def)
{
	JV *v = jv_get(o, key);
	if (!v)
		return def;
	if (v->t == J_NUM)
		return v->n;
	if (v->t == J_STR)
		return g_ascii_strtod(v->s, NULL);
	return def;
}

gint64 jv_getll(const JV *o, const char *key)
{
	JV *v = jv_get(o, key);
	if (!v)
		return 0;
	if (v->t == J_NUM)
		return (gint64)v->n;
	if (v->t == J_STR)
		return g_ascii_strtoll(v->s, NULL, 10);
	return 0;
}

gboolean jv_getb(const JV *o, const char *key, gboolean def)
{
	JV *v = jv_get(o, key);
	if (!v)
		return def;
	if (v->t == J_BOOL)
		return v->b;
	if (v->t == J_STR)
		return !strcmp(v->s, "true");
	return def;
}

static JV *parse_value(Parser *ps);

static int hex4(Parser *ps)
{
	int v = 0;
	if (ps->end - ps->p < 4) {
		fail(ps, "bad \\u escape");
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		int c = g_ascii_xdigit_value(*ps->p++);
		if (c < 0) {
			fail(ps, "bad \\u escape");
			return 0;
		}
		v = v * 16 + c;
	}
	return v;
}

static char *parse_string_raw(Parser *ps)
{
	GString *out = g_string_new(NULL);
	ps->p++;
	while (ps->p < ps->end && *ps->p != '"') {
		char c = *ps->p++;
		if (c != '\\') {
			g_string_append_c(out, c);
			continue;
		}
		if (ps->p >= ps->end) {
			fail(ps, "truncated string");
			break;
		}
		c = *ps->p++;
		switch (c) {
		case 'n': g_string_append_c(out, '\n'); break;
		case 't': g_string_append_c(out, '\t'); break;
		case 'r': g_string_append_c(out, '\r'); break;
		case 'b': g_string_append_c(out, '\b'); break;
		case 'f': g_string_append_c(out, '\f'); break;
		case '/': g_string_append_c(out, '/'); break;
		case '\\': g_string_append_c(out, '\\'); break;
		case '"': g_string_append_c(out, '"'); break;
		case 'u': {
			gunichar u = hex4(ps);
			if (u >= 0xD800 && u < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
				ps->p += 2;
				gunichar lo = hex4(ps);
				u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
			}
			if (g_unichar_validate(u))
				g_string_append_unichar(out, u);
			break;
		}
		default:
			fail(ps, "bad escape");
		}
		if (ps->failed)
			break;
	}
	if (ps->p >= ps->end || *ps->p != '"')
		fail(ps, "unterminated string");
	else
		ps->p++;
	return g_string_free(out, FALSE);
}

static JV *parse_value(Parser *ps)
{
	skip_ws(ps);
	if (ps->p >= ps->end) {
		fail(ps, "unexpected end");
		return NULL;
	}
	if (++ps->depth > 64) {
		fail(ps, "too deep");
		return NULL;
	}
	JV *v = NULL;
	char c = *ps->p;
	if (c == '{') {
		v = jv_new(J_OBJ);
		ps->p++;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == '}') {
			ps->p++;
		} else {
			while (!ps->failed) {
				skip_ws(ps);
				if (ps->p >= ps->end || *ps->p != '"') {
					fail(ps, "expected key");
					break;
				}
				char *k = parse_string_raw(ps);
				skip_ws(ps);
				if (ps->p >= ps->end || *ps->p != ':') {
					g_free(k);
					fail(ps, "expected :");
					break;
				}
				ps->p++;
				JV *item = parse_value(ps);
				if (!item) {
					g_free(k);
					break;
				}
				jv_set(v, k, item);
				g_free(k);
				skip_ws(ps);
				if (ps->p < ps->end && *ps->p == ',') {
					ps->p++;
					continue;
				}
				if (ps->p < ps->end && *ps->p == '}') {
					ps->p++;
					break;
				}
				fail(ps, "expected , or }");
			}
		}
	} else if (c == '[') {
		v = jv_new(J_ARR);
		ps->p++;
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
		} else {
			while (!ps->failed) {
				JV *item = parse_value(ps);
				if (!item)
					break;
				jv_push(v, item);
				skip_ws(ps);
				if (ps->p < ps->end && *ps->p == ',') {
					ps->p++;
					continue;
				}
				if (ps->p < ps->end && *ps->p == ']') {
					ps->p++;
					break;
				}
				fail(ps, "expected , or ]");
			}
		}
	} else if (c == '"') {
		v = jv_new(J_STR);
		v->s = parse_string_raw(ps);
	} else if (!strncmp(ps->p, "true", 4) && ps->end - ps->p >= 4) {
		ps->p += 4;
		v = jv_new_bool(TRUE);
	} else if (!strncmp(ps->p, "false", 5) && ps->end - ps->p >= 5) {
		ps->p += 5;
		v = jv_new_bool(FALSE);
	} else if (!strncmp(ps->p, "null", 4) && ps->end - ps->p >= 4) {
		ps->p += 4;
		v = jv_new(J_NULL);
	} else if (c == '-' || g_ascii_isdigit(c)) {
		char *e = NULL;
		double d = g_ascii_strtod(ps->p, &e);
		if (e == ps->p || e > ps->end) {
			fail(ps, "bad number");
		} else {
			ps->p = e;
			v = jv_new_num(d);
		}
	} else {
		fail(ps, "unexpected character");
	}
	ps->depth--;
	if (ps->failed) {
		jv_free(v);
		return NULL;
	}
	return v;
}

JV *jv_parse(const char *text, GError **err)
{
	if (!text) {
		g_set_error(err, JSON_ERR, 1, "json: null input");
		return NULL;
	}
	Parser ps = { text, text + strlen(text), err, FALSE, 0 };
	JV *v = parse_value(&ps);
	if (v) {
		skip_ws(&ps);
		if (ps.p != ps.end) {
			jv_free(v);
			g_set_error(err, JSON_ERR, 1, "json: trailing data");
			return NULL;
		}
	}
	return v;
}

static void dump_str(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; *s; s++) {
		unsigned char c = *s;
		switch (c) {
		case '"': g_string_append(o, "\\\""); break;
		case '\\': g_string_append(o, "\\\\"); break;
		case '\n': g_string_append(o, "\\n"); break;
		case '\r': g_string_append(o, "\\r"); break;
		case '\t': g_string_append(o, "\\t"); break;
		default:
			if (c < 0x20)
				g_string_append_printf(o, "\\u%04x", c);
			else
				g_string_append_c(o, c);
		}
	}
	g_string_append_c(o, '"');
}

static void indent(GString *o, int n)
{
	g_string_append_c(o, '\n');
	for (int i = 0; i < n; i++)
		g_string_append(o, "  ");
}

static void dump(GString *o, const JV *v, gboolean pretty, int lvl)
{
	switch (v->t) {
	case J_NULL: g_string_append(o, "null"); break;
	case J_BOOL: g_string_append(o, v->b ? "true" : "false"); break;
	case J_NUM: {
		char buf[G_ASCII_DTOSTR_BUF_SIZE];
		if (v->n == (double)(gint64)v->n)
			g_string_append_printf(o, "%" G_GINT64_FORMAT, (gint64)v->n);
		else
			g_string_append(o, g_ascii_dtostr(buf, sizeof buf, v->n));
		break;
	}
	case J_STR: dump_str(o, v->s); break;
	case J_ARR:
		g_string_append_c(o, '[');
		for (guint i = 0; i < v->items->len; i++) {
			if (i)
				g_string_append_c(o, ',');
			if (pretty)
				indent(o, lvl + 1);
			dump(o, g_ptr_array_index(v->items, i), pretty, lvl + 1);
		}
		if (pretty && v->items->len)
			indent(o, lvl);
		g_string_append_c(o, ']');
		break;
	case J_OBJ:
		g_string_append_c(o, '{');
		for (guint i = 0; i < v->items->len; i++) {
			if (i)
				g_string_append_c(o, ',');
			if (pretty)
				indent(o, lvl + 1);
			dump_str(o, g_ptr_array_index(v->keys, i));
			g_string_append_c(o, ':');
			if (pretty)
				g_string_append_c(o, ' ');
			dump(o, g_ptr_array_index(v->items, i), pretty, lvl + 1);
		}
		if (pretty && v->items->len)
			indent(o, lvl);
		g_string_append_c(o, '}');
		break;
	}
}

char *jv_dump(const JV *v, gboolean pretty)
{
	GString *o = g_string_new(NULL);
	dump(o, v, pretty, 0);
	return g_string_free(o, FALSE);
}
