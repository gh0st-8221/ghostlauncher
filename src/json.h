#ifndef JSON_H
#define JSON_H
#include <glib.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JV {
	JType t;
	double n;
	gboolean b;
	char *s;
	GPtrArray *items;
	GPtrArray *keys;
} JV;

JV *jv_parse(const char *text, GError **err);
void jv_free(JV *v);
JV *jv_new(JType t);
JV *jv_new_str(const char *s);
JV *jv_new_num(double n);
JV *jv_new_bool(gboolean b);
void jv_set(JV *o, const char *key, JV *v);
void jv_push(JV *a, JV *v);
JV *jv_get(const JV *o, const char *key);
JV *jv_at(const JV *a, guint i);
guint jv_len(const JV *v);
const char *jv_gets(const JV *o, const char *key, const char *def);
double jv_getn(const JV *o, const char *key, double def);
gint64 jv_getll(const JV *o, const char *key);
gboolean jv_getb(const JV *o, const char *key, gboolean def);
char *jv_dump(const JV *v, gboolean pretty);
JV *jv_strv(const char *const *list);

#endif
