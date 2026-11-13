#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include "../src/json.h"
#include "../src/util.h"
#include "../src/library.h"
#include "../src/proton.h"
#include "../src/steam.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static GMainLoop *loop;
static int cb_status;
static char *cb_out;

static void proc_cb(GBytes *out, GBytes *err, int status, gpointer ud)
{
	(void)err; (void)ud;
	cb_status = status;
	g_free(cb_out);
	cb_out = bytes_dup_str(out);
	g_main_loop_quit(loop);
}

static gboolean timeout_quit(gpointer d)
{
	printf("TIMEOUT\n");
	g_main_loop_quit(d);
	return G_SOURCE_REMOVE;
}

static void wait_loop(void)
{
	guint id = g_timeout_add_seconds(15, timeout_quit, loop);
	g_main_loop_run(loop);
	g_source_remove(id);
}

static void test_json(void)
{
	GError *e = NULL;
	JV *v = jv_parse("{\"a\":[1,2.5,\"x\\u00e9\\ud83d\\ude00\\n\"],\"b\":{\"c\":true,\"d\":null},\"n\":\"123\"}", &e);
	CHECK(v && !e);
	CHECK(jv_len(jv_get(v, "a")) == 3);
	CHECK(jv_get(v, "a") && jv_at(jv_get(v, "a"), 1)->n == 2.5);
	CHECK(!strcmp(jv_at(jv_get(v, "a"), 2)->s, "x\xc3\xa9\xf0\x9f\x98\x80\n"));
	CHECK(jv_getb(jv_get(v, "b"), "c", FALSE));
	CHECK(jv_getll(v, "n") == 123);
	char *d = jv_dump(v, FALSE);
	JV *w = jv_parse(d, NULL);
	char *d2 = jv_dump(w, TRUE);
	JV *w2 = jv_parse(d2, NULL);
	char *d3 = jv_dump(w2, FALSE);
	CHECK(!strcmp(d, d3));
	g_free(d); g_free(d2); g_free(d3);
	jv_free(v); jv_free(w); jv_free(w2);
	CHECK(jv_parse("{\"a\":", &e) == NULL);
	g_clear_error(&e);
	CHECK(jv_parse("[1,2,]", NULL) == NULL);
	CHECK(jv_parse("{} x", NULL) == NULL);
	CHECK(jv_parse("\"\\q\"", NULL) == NULL);
}

static char *slurp(const char *p)
{
	char *t = NULL;
	g_file_get_contents(p, &t, NULL, NULL);
	return t;
}

static void test_steam(void)
{
	char *id = steam_parse_search("{\"total\":3,\"items\":[{\"type\":\"dlc\",\"name\":\"X\",\"id\":1},{\"type\":\"app\",\"name\":\"Other Game\",\"id\":10},{\"type\":\"app\",\"name\":\"Need For Speed\\u2122\",\"id\":20},{\"type\":\"app\",\"name\":\"nfsu2\",\"id\":30}]}", "NFSU2");
	CHECK(id && !strcmp(id, "30"));
	g_free(id);
	id = steam_parse_search("{\"items\":[{\"type\":\"app\",\"name\":\"First\",\"id\":7}]}", "zzz");
	CHECK(id && !strcmp(id, "7"));
	g_free(id);
	CHECK(steam_parse_search("{\"items\":[]}", "a") == NULL);
	CHECK(steam_parse_search("garbage", "a") == NULL);

	char **v = cmd_expand("alacritty --working-directory %d -e \"sh -c\"", "/g/My Game", NULL);
	CHECK(v && !strcmp(v[0], "alacritty") && !strcmp(v[2], "/g/My Game") && !strcmp(v[4], "sh -c") && !v[5]);
	g_strfreev(v);
	CHECK(cmd_expand("alacritty \"unterminated", "/x", NULL) == NULL);
}

static void test_proton(void)
{
	char *home = g_strdup(g_getenv("GL_HOME"));
	char *d = g_build_filename(home, ".steam/root/compatibilitytools.d/GE-Proton9-1", NULL);
	gl_mkdir(d);
	char *bin = g_build_filename(d, "proton", NULL);
	g_file_set_contents(bin, "#!/bin/sh\necho \"args=$*\" > \"$STEAM_COMPAT_DATA_PATH/../out.txt\"\necho \"pfx=$STEAM_COMPAT_DATA_PATH client=$STEAM_COMPAT_CLIENT_INSTALL_PATH foo=$FOO pwd=$(pwd)\" >> \"$STEAM_COMPAT_DATA_PATH/../out.txt\"\n", -1, NULL);
	g_chmod(bin, 0755);
	char *ours = g_build_filename(home, "data/proton/GE-Proton9-5", NULL);
	gl_mkdir(ours);
	char *b2 = g_build_filename(ours, "proton", NULL);
	g_file_set_contents(b2, "x", -1, NULL);

	GPtrArray *l = proton_list();
	CHECK(l->len == 2);
	CHECK(!strcmp(((ProtonVer *)g_ptr_array_index(l, 0))->name, "GE-Proton9-5"));
	CHECK(((ProtonVer *)g_ptr_array_index(l, 0))->ours);
	CHECK(proton_pick(l, "GE-Proton9-1") && !strcmp(proton_pick(l, "GE-Proton9-1")->name, "GE-Proton9-1"));
	CHECK(proton_pick(l, "nope") == g_ptr_array_index(l, 0));
	ProtonVer *theirs = g_ptr_array_index(l, 1);
	CHECK(!proton_remove(theirs));
	CHECK(g_file_test(theirs->path, G_FILE_TEST_EXISTS));
	CHECK(proton_remove(g_ptr_array_index(l, 0)));
	CHECK(!g_file_test(ours, G_FILE_TEST_EXISTS));
	g_ptr_array_free(l, TRUE);

	char *gdir = g_build_filename(home, "Games/nfsu2", NULL);
	gl_mkdir(gdir);
	char *exe = g_build_filename(gdir, "nfsu2.exe", NULL);
	g_file_set_contents(exe, "MZ", -1, NULL);
	Game *g = game_new("NFSU2");
	SETSTR(g->proton, "GE-Proton9-1");
	SETSTR(g->env, "FOO=bar PROTON_LOG=1");
	SETSTR(g->prefix, "");
	char *prefix = game_prefix(g);
	CHECK(g_str_has_suffix(prefix, g->id));
	char *log = NULL;
	GError *e = NULL;
	GSubprocess *p = proton_run(g, exe, "-windowed \"a b\"", &log, &e);
	CHECK(p && !e);
	if (p) {
		g_subprocess_wait(p, NULL, NULL);
		g_object_unref(p);
	}
	char *outp = g_build_filename(prefix, "../out.txt", NULL);
	char *o = slurp(outp);
	CHECK(o != NULL);
	if (o) {
		CHECK(strstr(o, "args=run "));
		CHECK(strstr(o, "nfsu2.exe -windowed a b"));
		CHECK(strstr(o, "foo=bar"));
		CHECK(strstr(o, "Games/nfsu2"));
		CHECK(strstr(o, prefix));
	}
	g_free(o); g_free(outp); g_free(log);

	SETSTR(g->wrapper, "env WRAP=1");
	p = proton_run(g, exe, "", &log, &e);
	CHECK(p);
	if (p) {
		g_subprocess_wait(p, NULL, NULL);
		g_object_unref(p);
	}
	g_free(log);
	SETSTR(g->proton, "missing");
	char *tmp = g_build_filename(home, ".steam", NULL);
	g_free(tmp);
	game_free(g);
	g_free(prefix); g_free(exe); g_free(gdir); g_free(b2); g_free(ours); g_free(bin); g_free(d); g_free(home);

	GPtrArray *rels = proton_parse_releases("[{\"tag_name\":\"GE-Proton11-12\",\"assets\":[{\"name\":\"GE-Proton11-12.sha512sum\",\"browser_download_url\":\"u0\",\"size\":1},{\"name\":\"GE-Proton11-12-aarch64.tar.gz\",\"browser_download_url\":\"https://g/a.tar.gz\",\"size\":400},{\"name\":\"GE-Proton11-12-x86_64.tar.gz\",\"browser_download_url\":\"https://g/x.tar.gz\",\"size\":500}]},{\"tag_name\":\"old\",\"assets\":[{\"name\":\"GE-Proton9-5.tar.gz\",\"browser_download_url\":\"https://g/o.tar.gz\",\"size\":7}]},{\"tag_name\":\"armonly\",\"assets\":[{\"name\":\"x-aarch64.tar.gz\",\"browser_download_url\":\"u\",\"size\":1}]},{\"tag_name\":\"x\",\"assets\":[]}]");
	CHECK(rels->len == 2);
	ProtonRel *r = g_ptr_array_index(rels, 0);
	CHECK(!strcmp(r->tag, "GE-Proton11-12") && !strcmp(r->file, "GE-Proton11-12-x86_64.tar.gz") && r->size == 500);
	CHECK(!strcmp(((ProtonRel *)g_ptr_array_index(rels, 1))->file, "GE-Proton9-5.tar.gz"));
	g_ptr_array_free(rels, TRUE);
}

static void test_extract_done(const char *err, gpointer ud)
{
	(void)ud;
	CHECK(err == NULL);
	g_main_loop_quit(loop);
}

static void test_library(void)
{
	settings_load();
	CHECK(g_str_has_suffix(gl_settings.dl_dir, "Games"));
	SETSTR(gl_settings.dl_dir, "/x/dl");
	SETSTR(gl_settings.default_proton, "GE-Proton9-5");
	settings_save();
	SETSTR(gl_settings.dl_dir, "");
	settings_load();
	CHECK(!strcmp(gl_settings.dl_dir, "/x/dl") && !strcmp(gl_settings.default_proton, "GE-Proton9-5"));

	lib_load();
	Game *g = game_new("Quote \"Game\" \xd0\x86");
	SETSTR(g->env, "A=1\nB=2");
	lib_add(g);
	lib_save();
	char *id = g_strdup(g->id);
	lib_load();
	CHECK(gl_games->len == 1);
	Game *h = lib_find_id(id);
	CHECK(h && !strcmp(h->name, "Quote \"Game\" \xd0\x86") && !strcmp(h->env, "A=1\nB=2"));
	lib_remove(h);
	lib_save();
	lib_load();
	CHECK(gl_games->len == 0);
	g_free(id);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	loop = g_main_loop_new(NULL, FALSE);
	test_json();
	test_library();
	test_steam();
	test_proton();

	const char *echo[] = { "echo", "hi", NULL };
	proc_run(echo, NULL, proc_cb, NULL);
	wait_loop();
	CHECK(cb_status == 0 && !strcmp(cb_out, "hi\n"));
	const char *cat[] = { "cat", NULL };
	GBytes *in = g_bytes_new("pw=ü", 5);
	proc_run(cat, in, proc_cb, NULL);
	wait_loop();
	CHECK(cb_status == 0 && !strcmp(cb_out, "pw=ü"));
	g_bytes_unref(in);
	const char *none[] = { "definitely-no-such-binary", NULL };
	proc_run(none, NULL, proc_cb, NULL);
	wait_loop();
	CHECK(cb_status == -1);

	char *tarsrc = g_build_filename(g_getenv("GL_HOME"), "tarsrc", NULL);
	gl_mkdir(tarsrc);
	char *tf = g_build_filename(tarsrc, "GE-X", NULL);
	gl_mkdir(tf);
	char *tp = g_build_filename(tf, "proton", NULL);
	g_file_set_contents(tp, "x", -1, NULL);
	char *tar = g_build_filename(g_getenv("GL_HOME"), "ge.tar.gz", NULL);
	char *cmd = g_strdup_printf("tar -czf %s -C %s GE-X", tar, tarsrc);
	CHECK(system(cmd) == 0);
	proton_extract(tar, test_extract_done, NULL);
	wait_loop();
	GPtrArray *l = proton_list();
	gboolean found = FALSE;
	for (guint i = 0; i < l->len; i++)
		if (!strcmp(((ProtonVer *)g_ptr_array_index(l, i))->name, "GE-X"))
			found = TRUE;
	CHECK(found);
	g_ptr_array_free(l, TRUE);

	printf(fails ? "%d FAILED\n" : "all ok\n", fails);
	return fails != 0;
}
