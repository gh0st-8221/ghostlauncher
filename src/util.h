#ifndef UTIL_H
#define UTIL_H
#include <gio/gio.h>

typedef void (*ProcCb)(GBytes *out, GBytes *err, int status, gpointer ud);
typedef void (*DoneCb)(const char *err, gpointer ud);
typedef void (*StrCb)(const char *value, const char *err, gpointer ud);

char *gl_config_dir(void);
char *gl_data_dir(void);
char *gl_path(gboolean data, const char *a, const char *b);
void gl_mkdir(const char *path);
char *gl_slug(const char *name);
char *gl_new_id(void);
char *gl_home_path(const char *rel);

void proc_run(const char *const *argv, GBytes *stdin_data, ProcCb cb, gpointer ud);
const char *bytes_str(GBytes *b);
char *bytes_dup_str(GBytes *b);

char *fmt_size(gint64 n);
char *fmt_speed(gint64 n);
char *fmt_eta(gint64 secs);
char *url_escape(const char *s);
char **cmd_expand(const char *tmpl, const char *dir, GError **err);

#define SETSTR(dst, val) do { char *_n = g_strdup(val); g_free(dst); (dst) = _n; } while (0)

#endif
