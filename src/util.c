#include "util.h"
#include <string.h>
#include <glib/gstdio.h>

char *gl_config_dir(void)
{
	const char *h = g_getenv("GL_HOME");
	if (h)
		return g_build_filename(h, "config", NULL);
	return g_build_filename(g_get_user_config_dir(), "ghostlauncher", NULL);
}

char *gl_data_dir(void)
{
	const char *h = g_getenv("GL_HOME");
	if (h)
		return g_build_filename(h, "data", NULL);
	return g_build_filename(g_get_user_data_dir(), "ghostlauncher", NULL);
}

char *gl_path(gboolean data, const char *a, const char *b)
{
	char *base = data ? gl_data_dir() : gl_config_dir();
	char *p = g_build_filename(base, a, b, NULL);
	g_free(base);
	return p;
}

char *gl_home_path(const char *rel)
{
	const char *h = g_getenv("GL_HOME");
	return g_build_filename(h ? h : g_get_home_dir(), rel, NULL);
}

void gl_mkdir(const char *path)
{
	g_mkdir_with_parents(path, 0755);
}

char *gl_slug(const char *name)
{
	GString *s = g_string_new(NULL);
	for (const char *p = name; *p; p++) {
		if (g_ascii_isalnum(*p) || *p == '-' || *p == '_' || *p == '.')
			g_string_append_c(s, *p);
		else if (*p == ' ' || *p == '/' || *p == ':')
			g_string_append_c(s, '_');
	}
	if (s->len == 0)
		g_string_append(s, "game");
	if (s->len > 80)
		g_string_truncate(s, 80);
	return g_string_free(s, FALSE);
}

char *gl_new_id(void)
{
	return g_strdup_printf("%08x%08x", g_random_int(), g_random_int());
}

const char *bytes_str(GBytes *b)
{
	return b ? (const char *)g_bytes_get_data(b, NULL) : "";
}

char *bytes_dup_str(GBytes *b)
{
	gsize n = 0;
	const char *d = b ? g_bytes_get_data(b, &n) : NULL;
	return d ? g_strndup(d, n) : g_strdup("");
}

typedef struct {
	ProcCb cb;
	gpointer ud;
	GSubprocess *proc;
} ProcCtx;

static void proc_done(GObject *src, GAsyncResult *res, gpointer data)
{
	ProcCtx *c = data;
	GBytes *out = NULL, *err = NULL;
	GError *e = NULL;
	g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &out, &err, &e);
	int status = -1;
	if (!e && g_subprocess_get_if_exited(c->proc))
		status = g_subprocess_get_exit_status(c->proc);
	if (e)
		g_error_free(e);
	if (c->cb)
		c->cb(out, err, status, c->ud);
	if (out)
		g_bytes_unref(out);
	if (err)
		g_bytes_unref(err);
	g_object_unref(c->proc);
	g_free(c);
}

typedef struct {
	ProcCb cb;
	gpointer ud;
} FailCtx;

static gboolean proc_fail_idle(gpointer data)
{
	FailCtx *f = data;
	if (f->cb)
		f->cb(NULL, NULL, -1, f->ud);
	g_free(f);
	return G_SOURCE_REMOVE;
}

void proc_run(const char *const *argv, GBytes *stdin_data, ProcCb cb, gpointer ud)
{
	GSubprocessFlags fl = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
	if (stdin_data)
		fl |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
	GError *e = NULL;
	GSubprocess *p = g_subprocess_newv(argv, fl, &e);
	if (!p) {
		g_clear_error(&e);
		FailCtx *f = g_new0(FailCtx, 1);
		f->cb = cb;
		f->ud = ud;
		g_idle_add(proc_fail_idle, f);
		return;
	}
	ProcCtx *c = g_new0(ProcCtx, 1);
	c->cb = cb;
	c->ud = ud;
	c->proc = p;
	g_subprocess_communicate_async(p, stdin_data, NULL, proc_done, c);
}

char *fmt_size(gint64 n)
{
	const char *u[] = { "B", "KB", "MB", "GB", "TB" };
	double v = n;
	int i = 0;
	while (v >= 1024 && i < 4) {
		v /= 1024;
		i++;
	}
	return i == 0 ? g_strdup_printf("%d B", (int)v) : g_strdup_printf("%.1f %s", v, u[i]);
}

char *fmt_speed(gint64 n)
{
	char *s = fmt_size(n);
	char *r = g_strdup_printf("%s/s", s);
	g_free(s);
	return r;
}

char *fmt_eta(gint64 secs)
{
	if (secs < 0)
		return g_strdup("--");
	if (secs >= 86400)
		return g_strdup_printf("%dd %dh", (int)(secs / 86400), (int)(secs % 86400 / 3600));
	if (secs >= 3600)
		return g_strdup_printf("%dh %dm", (int)(secs / 3600), (int)(secs % 3600 / 60));
	if (secs >= 60)
		return g_strdup_printf("%dm %ds", (int)(secs / 60), (int)(secs % 60));
	return g_strdup_printf("%ds", (int)secs);
}

char *url_escape(const char *s)
{
	return g_uri_escape_string(s, NULL, FALSE);
}

char **cmd_expand(const char *tmpl, const char *dir, GError **err)
{
	char **argv = NULL;
	if (!g_shell_parse_argv(tmpl, NULL, &argv, err))
		return NULL;
	for (int i = 0; argv[i]; i++) {
		if (strstr(argv[i], "%d")) {
			char **parts = g_strsplit(argv[i], "%d", -1);
			char *joined = g_strjoinv(dir, parts);
			g_strfreev(parts);
			g_free(argv[i]);
			argv[i] = joined;
		}
	}
	return argv;
}
