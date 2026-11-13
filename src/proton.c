#include "proton.h"
#include <string.h>
#include <glib/gstdio.h>

void proton_ver_free(ProtonVer *v)
{
	if (!v)
		return;
	g_free(v->name);
	g_free(v->path);
	g_free(v);
}

void proton_rel_free(ProtonRel *r)
{
	if (!r)
		return;
	g_free(r->tag);
	g_free(r->url);
	g_free(r->file);
	g_free(r);
}

char *proton_install_dir(void)
{
	return gl_path(TRUE, "proton", NULL);
}

static void scan_dir(GPtrArray *out, const char *dir, gboolean ours)
{
	GDir *d = g_dir_open(dir, 0, NULL);
	if (!d)
		return;
	const char *n;
	while ((n = g_dir_read_name(d))) {
		char *p = g_build_filename(dir, n, NULL);
		char *bin = g_build_filename(p, "proton", NULL);
		gboolean dup = FALSE;
		for (guint i = 0; i < out->len; i++)
			if (!strcmp(((ProtonVer *)g_ptr_array_index(out, i))->name, n))
				dup = TRUE;
		if (!dup && g_file_test(bin, G_FILE_TEST_IS_REGULAR)) {
			ProtonVer *v = g_new0(ProtonVer, 1);
			v->name = g_strdup(n);
			v->path = p;
			v->ours = ours;
			g_ptr_array_add(out, v);
			p = NULL;
		}
		g_free(p);
		g_free(bin);
	}
	g_dir_close(d);
}

static gint cmp_ver(gconstpointer a, gconstpointer b)
{
	const ProtonVer *x = *(ProtonVer *const *)a, *y = *(ProtonVer *const *)b;
	return -g_strcmp0(x->name, y->name);
}

GPtrArray *proton_list(void)
{
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)proton_ver_free);
	char *ours = proton_install_dir();
	scan_dir(out, ours, TRUE);
	g_free(ours);
	const char *rel[] = { ".steam/root/compatibilitytools.d", ".steam/steam/compatibilitytools.d",
			      ".local/share/Steam/compatibilitytools.d", ".local/share/Steam/steamapps/common", NULL };
	for (int i = 0; rel[i]; i++) {
		char *d = gl_home_path(rel[i]);
		scan_dir(out, d, FALSE);
		g_free(d);
	}
	scan_dir(out, "/usr/share/steam/compatibilitytools.d", FALSE);
	g_ptr_array_sort(out, cmp_ver);
	return out;
}

ProtonVer *proton_pick(GPtrArray *list, const char *name)
{
	for (guint i = 0; name && *name && i < list->len; i++) {
		ProtonVer *v = g_ptr_array_index(list, i);
		if (!strcmp(v->name, name))
			return v;
	}
	return list->len ? g_ptr_array_index(list, 0) : NULL;
}

static int asset_score(const char *name)
{
	if (!g_str_has_suffix(name, ".tar.gz"))
		return -1;
	gboolean arm = strstr(name, "aarch64") || strstr(name, "arm64");
	gboolean x86 = strstr(name, "x86_64") || strstr(name, "amd64");
#if defined(__aarch64__)
	return arm ? 2 : (x86 ? -1 : 0);
#else
	if (arm)
		return -1;
	return x86 ? 2 : 1;
#endif
}

GPtrArray *proton_parse_releases(const char *json)
{
	GPtrArray *res = g_ptr_array_new_with_free_func((GDestroyNotify)proton_rel_free);
	JV *root = jv_parse(json, NULL);
	for (guint i = 0; root && root->t == J_ARR && i < jv_len(root); i++) {
		JV *r = jv_at(root, i);
		JV *assets = jv_get(r, "assets");
		const JV *best = NULL;
		int best_score = -1;
		for (guint j = 0; assets && j < jv_len(assets); j++) {
			JV *a = jv_at(assets, j);
			const char *u = jv_gets(a, "browser_download_url", "");
			int sc = *u ? asset_score(jv_gets(a, "name", "")) : -1;
			if (sc > best_score) {
				best_score = sc;
				best = a;
			}
		}
		if (best) {
			ProtonRel *x = g_new0(ProtonRel, 1);
			x->tag = g_strdup(jv_gets(r, "tag_name", jv_gets(best, "name", "")));
			x->url = g_strdup(jv_gets(best, "browser_download_url", ""));
			x->file = g_strdup(jv_gets(best, "name", ""));
			x->size = jv_getll(best, "size");
			g_ptr_array_add(res, x);
		}
	}
	jv_free(root);
	return res;
}

typedef struct {
	ProtonRelCb cb;
	gpointer ud;
} RelCtx;

static void rel_done(GBytes *out, GBytes *err, int status, gpointer ud)
{
	RelCtx *c = ud;
	if (status != 0) {
		char *e = bytes_dup_str(err);
		char *m = g_strdup_printf("cannot fetch the release list: %s", *e ? e : "curl failed");
		c->cb(NULL, m, c->ud);
		g_free(m);
		g_free(e);
	} else {
		char *txt = bytes_dup_str(out);
		GPtrArray *r = proton_parse_releases(txt);
		g_free(txt);
		if (r->len == 0)
			c->cb(NULL, "no releases found (github rate limit?)", c->ud);
		else
			c->cb(r, NULL, c->ud);
		g_ptr_array_free(r, TRUE);
	}
	g_free(c);
}

void proton_fetch_releases(ProtonRelCb cb, gpointer ud)
{
	RelCtx *c = g_new0(RelCtx, 1);
	c->cb = cb;
	c->ud = ud;
	const char *argv[10] = { "curl", "-sS", "-L", "--max-time", "30", NULL };
	const char *px = gl_settings.proxy && *gl_settings.proxy ? gl_settings.proxy : NULL;
	char *url = g_strdup("https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=12");
	int n = 5;
	if (px) {
		argv[n++] = "-x";
		argv[n++] = px;
	}
	argv[n++] = url;
	proc_run(argv, NULL, rel_done, c);
	g_free(url);
}

typedef struct {
	DoneCb cb;
	gpointer ud;
} ExCtx;

static void download_done(GBytes *out, GBytes *err, int status, gpointer ud)
{
	ExCtx *c = ud;
	(void)out;
	if (status == 0) {
		c->cb(NULL, c->ud);
	} else {
		char *e = bytes_dup_str(err);
		char *m = g_strdup_printf("download failed: %s", *e ? e : "curl failed");
		c->cb(m, c->ud);
		g_free(m);
		g_free(e);
	}
	g_free(c);
}

void proton_download(const char *url, const char *dest, DoneCb cb, gpointer ud)
{
	const char *argv[12] = { "curl", "-sS", "-L", "-f", "-o", dest, NULL };
	int n = 6;
	if (gl_settings.proxy && *gl_settings.proxy) {
		argv[n++] = "-x";
		argv[n++] = gl_settings.proxy;
	}
	argv[n++] = url;
	argv[n] = NULL;
	ExCtx *c = g_new0(ExCtx, 1);
	c->cb = cb;
	c->ud = ud;
	proc_run(argv, NULL, download_done, c);
}

static void extract_done(GBytes *out, GBytes *err, int status, gpointer ud)
{
	ExCtx *c = ud;
	(void)out;
	if (status == 0) {
		c->cb(NULL, c->ud);
	} else {
		char *e = bytes_dup_str(err);
		char *m = g_strdup_printf("tar failed: %s", e);
		c->cb(m, c->ud);
		g_free(m);
		g_free(e);
	}
	g_free(c);
}

void proton_extract(const char *tarball, DoneCb cb, gpointer ud)
{
	char *dir = proton_install_dir();
	gl_mkdir(dir);
	const char *argv[] = { "tar", "-xzf", tarball, "-C", dir, NULL };
	ExCtx *c = g_new0(ExCtx, 1);
	c->cb = cb;
	c->ud = ud;
	proc_run(argv, NULL, extract_done, c);
	g_free(dir);
}

gboolean proton_remove(const ProtonVer *v)
{
	if (!v->ours || !*v->path || !*v->name)
		return FALSE;
	char *base = proton_install_dir();
	gboolean ok = g_str_has_prefix(v->path, base) && strlen(v->path) > strlen(base) + 1;
	g_free(base);
	if (!ok)
		return FALSE;
	const char *argv[] = { "rm", "-rf", "--", v->path, NULL };
	return g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
}

char *steam_root(void)
{
	const char *cand[] = { ".local/share/Steam", ".steam/steam", ".steam/root", NULL };
	for (int i = 0; cand[i]; i++) {
		char *p = gl_home_path(cand[i]);
		if (g_file_test(p, G_FILE_TEST_IS_DIR))
			return p;
		g_free(p);
	}
	char *p = gl_path(TRUE, "steam", NULL);
	gl_mkdir(p);
	return p;
}

static void add_words(GPtrArray *argv, const char *s)
{
	if (!s || !*s)
		return;
	char **words = NULL;
	int n = 0;
	if (g_shell_parse_argv(s, &n, &words, NULL)) {
		for (int i = 0; i < n; i++)
			g_ptr_array_add(argv, g_strdup(words[i]));
		g_strfreev(words);
	}
}

GSubprocess *proton_run(const Game *g, const char *exe, const char *extra_args, char **logpath, GError **err)
{
	GPtrArray *vers = proton_list();
	ProtonVer *v = proton_pick(vers, *g->proton ? g->proton : gl_settings.default_proton);
	if (!v) {
		g_set_error(err, g_quark_from_static_string("proton"), 1, "no Proton installed: open the Proton tab and install one");
		g_ptr_array_free(vers, TRUE);
		return NULL;
	}
	char *bin = g_build_filename(v->path, "proton", NULL);
	char *prefix = game_prefix(g);
	gl_mkdir(prefix);
	char *steam = steam_root();
	char *logs = gl_path(TRUE, "logs", NULL);
	gl_mkdir(logs);
	char *logname = g_strconcat(g->id, ".log", NULL);
	char *log = g_build_filename(logs, logname, NULL);
	g_free(logname);
	g_free(logs);

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	add_words(argv, g->wrapper);
	g_ptr_array_add(argv, bin);
	g_ptr_array_add(argv, g_strdup("run"));
	g_ptr_array_add(argv, g_strdup(exe));
	add_words(argv, extra_args);
	g_ptr_array_add(argv, NULL);

	GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
	g_subprocess_launcher_setenv(l, "STEAM_COMPAT_DATA_PATH", prefix, TRUE);
	g_subprocess_launcher_setenv(l, "STEAM_COMPAT_CLIENT_INSTALL_PATH", steam, TRUE);
	if (g->env && *g->env) {
		char **kv = g_strsplit_set(g->env, " \t\n", -1);
		for (int i = 0; kv[i]; i++) {
			char *eq = strchr(kv[i], '=');
			if (eq && eq != kv[i]) {
				*eq = 0;
				g_subprocess_launcher_setenv(l, kv[i], eq + 1, TRUE);
			}
		}
		g_strfreev(kv);
	}
	char *cwd = g_path_get_dirname(exe);
	if (g_file_test(cwd, G_FILE_TEST_IS_DIR))
		g_subprocess_launcher_set_cwd(l, cwd);
	g_subprocess_launcher_set_stdout_file_path(l, log);
	GSubprocess *p = g_subprocess_launcher_spawnv(l, (const char *const *)argv->pdata, err);
	g_object_unref(l);
	g_ptr_array_free(argv, TRUE);
	g_free(cwd);
	g_free(prefix);
	g_free(steam);
	g_ptr_array_free(vers, TRUE);
	if (logpath)
		*logpath = log;
	else
		g_free(log);
	return p;
}
