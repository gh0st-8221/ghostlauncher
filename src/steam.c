#include "steam.h"
#include "json.h"
#include "library.h"
#include <string.h>
#include <glib/gstdio.h>

char *steam_parse_search(const char *json, const char *name)
{
	JV *root = jv_parse(json, NULL);
	JV *items = jv_get(root, "items");
	char *want = g_utf8_casefold(name, -1);
	const JV *best = NULL;
	for (guint i = 0; items && i < jv_len(items); i++) {
		JV *it = jv_at(items, i);
		if (strcmp(jv_gets(it, "type", "app"), "app"))
			continue;
		if (!best)
			best = it;
		char *n = g_utf8_casefold(jv_gets(it, "name", ""), -1);
		gboolean eq = !strcmp(n, want);
		g_free(n);
		if (eq) {
			best = it;
			break;
		}
	}
	char *id = best ? g_strdup_printf("%" G_GINT64_FORMAT, jv_getll(best, "id")) : NULL;
	g_free(want);
	jv_free(root);
	return id;
}

typedef struct {
	DoneCb cb;
	gpointer ud;
	char *dest;
	char *name;
	char *id;
	int stage;
} Ctx;

static void ctx_free(Ctx *c)
{
	g_free(c->dest);
	g_free(c->name);
	g_free(c->id);
	g_free(c);
}

static void curl_to(const char *url, const char *dest, ProcCb cb, gpointer ud)
{
	const char *argv[14] = { "curl", "-sS", "-L", "--max-time", "30", NULL };
	int n = 5;
	if (dest) {
		argv[n++] = "-f";
		argv[n++] = "-o";
		argv[n++] = dest;
	}
	if (gl_settings.proxy && *gl_settings.proxy) {
		argv[n++] = "-x";
		argv[n++] = gl_settings.proxy;
	}
	argv[n++] = url;
	argv[n] = NULL;
	proc_run(argv, NULL, cb, ud);
}

static void image_done(GBytes *out, GBytes *err, int status, gpointer ud)
{
	Ctx *c = ud;
	(void)out;
	(void)err;
	if (status == 0 && g_file_test(c->dest, G_FILE_TEST_EXISTS)) {
		c->cb(NULL, c->ud);
		ctx_free(c);
		return;
	}
	if (c->stage == 0) {
		c->stage = 1;
		char *u = g_strdup_printf("https://cdn.cloudflare.steamstatic.com/steam/apps/%s/header.jpg", c->id);
		curl_to(u, c->dest, image_done, c);
		g_free(u);
		return;
	}
	g_unlink(c->dest);
	c->cb("no cover found on Steam for this name", c->ud);
	ctx_free(c);
}

static void search_done(GBytes *out, GBytes *err, int status, gpointer ud)
{
	Ctx *c = ud;
	(void)err;
	char *txt = status == 0 ? bytes_dup_str(out) : NULL;
	char *id = txt ? steam_parse_search(txt, c->name) : NULL;
	g_free(txt);
	if (!id) {
		c->cb(status == 0 ? "game not found on Steam: fix the name and try again" : "network error", c->ud);
		ctx_free(c);
		return;
	}
	c->id = id;
	char *u = g_strdup_printf("https://cdn.cloudflare.steamstatic.com/steam/apps/%s/library_600x900.jpg", id);
	curl_to(u, c->dest, image_done, c);
	g_free(u);
}

void steam_cover(const char *name, const char *dest, DoneCb cb, gpointer ud)
{
	Ctx *c = g_new0(Ctx, 1);
	c->cb = cb;
	c->ud = ud;
	c->dest = g_strdup(dest);
	c->name = g_strdup(name);
	char *q = url_escape(name);
	char *u = g_strdup_printf("https://store.steampowered.com/api/storesearch/?term=%s&l=english&cc=us", q);
	g_free(q);
	curl_to(u, NULL, search_done, c);
	g_free(u);
}
