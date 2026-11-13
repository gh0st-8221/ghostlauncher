#include "library.h"
#include "util.h"
#include <string.h>
#include <glib/gstdio.h>

Settings gl_settings;
GPtrArray *gl_games;

static char *read_file(const char *path)
{
	char *txt = NULL;
	if (!g_file_get_contents(path, &txt, NULL, NULL))
		return NULL;
	return txt;
}

static void write_file(const char *path, const char *txt)
{
	char *dir = g_path_get_dirname(path);
	gl_mkdir(dir);
	g_free(dir);
	char *tmp = g_strconcat(path, ".tmp", NULL);
	if (g_file_set_contents(tmp, txt, -1, NULL))
		g_rename(tmp, path);
	g_free(tmp);
}

void settings_load(void)
{
	char *dl = gl_home_path("Games");
	SETSTR(gl_settings.dl_dir, dl);
	g_free(dl);
	SETSTR(gl_settings.folder_cmd, "alacritty --working-directory %d");
	SETSTR(gl_settings.proxy, "");
	SETSTR(gl_settings.default_proton, "");
	char *p = gl_path(FALSE, "settings.json", NULL);
	char *txt = read_file(p);
	g_free(p);
	if (!txt)
		return;
	JV *v = jv_parse(txt, NULL);
	g_free(txt);
	if (!v)
		return;
	SETSTR(gl_settings.dl_dir, jv_gets(v, "dl_dir", gl_settings.dl_dir));
	SETSTR(gl_settings.folder_cmd, jv_gets(v, "folder_cmd", gl_settings.folder_cmd));
	SETSTR(gl_settings.proxy, jv_gets(v, "proxy", ""));
	SETSTR(gl_settings.default_proton, jv_gets(v, "default_proton", ""));
	jv_free(v);
}

void settings_save(void)
{
	JV *o = jv_new(J_OBJ);
	jv_set(o, "dl_dir", jv_new_str(gl_settings.dl_dir));
	jv_set(o, "folder_cmd", jv_new_str(gl_settings.folder_cmd));
	jv_set(o, "proxy", jv_new_str(gl_settings.proxy));
	jv_set(o, "default_proton", jv_new_str(gl_settings.default_proton));
	char *txt = jv_dump(o, TRUE);
	char *p = gl_path(FALSE, "settings.json", NULL);
	write_file(p, txt);
	g_free(p);
	g_free(txt);
	jv_free(o);
}

Game *game_new(const char *name)
{
	Game *g = g_new0(Game, 1);
	g->id = gl_new_id();
	g->name = g_strdup(name ? name : "");
	g->exe = g_strdup("");
	g->dir = g_strdup("");
	g->proton = g_strdup("");
	g->prefix = g_strdup("");
	g->args = g_strdup("");
	g->env = g_strdup("");
	g->wrapper = g_strdup("");
	g->cover = g_strdup("");
	return g;
}

void game_free(Game *g)
{
	if (!g)
		return;
	g_free(g->id);
	g_free(g->name);
	g_free(g->exe);
	g_free(g->dir);
	g_free(g->proton);
	g_free(g->prefix);
	g_free(g->args);
	g_free(g->env);
	g_free(g->wrapper);
	g_free(g->cover);
	g_free(g);
}

char *game_prefix(const Game *g)
{
	if (g->prefix && *g->prefix)
		return g_strdup(g->prefix);
	return gl_path(TRUE, "prefixes", g->id);
}

void lib_add(Game *g)
{
	if (!gl_games)
		gl_games = g_ptr_array_new_with_free_func((GDestroyNotify)game_free);
	g_ptr_array_add(gl_games, g);
}

void lib_remove(Game *g)
{
	g_ptr_array_remove(gl_games, g);
}

Game *lib_find_id(const char *id)
{
	for (guint i = 0; gl_games && i < gl_games->len; i++) {
		Game *g = g_ptr_array_index(gl_games, i);
		if (!strcmp(g->id, id))
			return g;
	}
	return NULL;
}

void lib_load(void)
{
	if (!gl_games)
		gl_games = g_ptr_array_new_with_free_func((GDestroyNotify)game_free);
	g_ptr_array_set_size(gl_games, 0);
	char *p = gl_path(FALSE, "library.json", NULL);
	char *txt = read_file(p);
	g_free(p);
	if (!txt)
		return;
	JV *v = jv_parse(txt, NULL);
	g_free(txt);
	for (guint i = 0; v && i < jv_len(v); i++) {
		JV *o = jv_at(v, i);
		Game *g = game_new(jv_gets(o, "name", ""));
		SETSTR(g->id, jv_gets(o, "id", g->id));
		SETSTR(g->exe, jv_gets(o, "exe", ""));
		SETSTR(g->dir, jv_gets(o, "dir", ""));
		SETSTR(g->proton, jv_gets(o, "proton", ""));
		SETSTR(g->prefix, jv_gets(o, "prefix", ""));
		SETSTR(g->args, jv_gets(o, "args", ""));
		SETSTR(g->env, jv_gets(o, "env", ""));
		SETSTR(g->wrapper, jv_gets(o, "wrapper", ""));
		SETSTR(g->cover, jv_gets(o, "cover", ""));
		lib_add(g);
	}
	jv_free(v);
}

void lib_save(void)
{
	JV *a = jv_new(J_ARR);
	for (guint i = 0; gl_games && i < gl_games->len; i++) {
		Game *g = g_ptr_array_index(gl_games, i);
		JV *o = jv_new(J_OBJ);
		jv_set(o, "id", jv_new_str(g->id));
		jv_set(o, "name", jv_new_str(g->name));
		jv_set(o, "exe", jv_new_str(g->exe));
		jv_set(o, "dir", jv_new_str(g->dir));
		jv_set(o, "proton", jv_new_str(g->proton));
		jv_set(o, "prefix", jv_new_str(g->prefix));
		jv_set(o, "args", jv_new_str(g->args));
		jv_set(o, "env", jv_new_str(g->env));
		jv_set(o, "wrapper", jv_new_str(g->wrapper));
		jv_set(o, "cover", jv_new_str(g->cover));
		jv_push(a, o);
	}
	char *txt = jv_dump(a, TRUE);
	char *p = gl_path(FALSE, "library.json", NULL);
	write_file(p, txt);
	g_free(p);
	g_free(txt);
	jv_free(a);
}
