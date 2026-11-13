#include <gtk/gtk.h>
#include <string.h>
#include <glib/gstdio.h>
#include "library.h"
#include "util.h"
#include "proton.h"
#include "steam.h"

typedef void (*PickCb)(const char *path, gpointer ud);

typedef struct {
	char *tag;
	char *dest;
	gint64 size;
	int pct;
	gboolean unpack;
} Job;

static struct {
	GtkApplication *app;
	GtkWindow *win;
	GtkWidget *stack;
	GtkWidget *status;
	GtkWidget *lib_flow;
	GtkWidget *lib_empty;
	GHashTable *running;
	GtkWidget *pr_inst;
	GtkWidget *pr_avail;
	GPtrArray *rels;
	GHashTable *jobs;
	guint job_timer;
	GtkWidget *st_dir;
	GtkWidget *st_proxy;
	GtkWidget *st_cmd;
	GtkWidget *st_proton;
	GPtrArray *st_vers;
} A;

static void rebuild_library(void);
static void rebuild_proton(gboolean choices);

static void set_status(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	gtk_label_set_text(GTK_LABEL(A.status), s);
	g_free(s);
}

static void alert(const char *msg)
{
	GtkAlertDialog *d = gtk_alert_dialog_new("%s", msg);
	gtk_alert_dialog_show(d, A.win);
	g_object_unref(d);
}

static void pad(GtkWidget *w, int px)
{
	gtk_widget_set_margin_top(w, px);
	gtk_widget_set_margin_bottom(w, px);
	gtk_widget_set_margin_start(w, px);
	gtk_widget_set_margin_end(w, px);
}

static GtkWidget *label(const char *text, gboolean grow)
{
	GtkWidget *l = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	if (grow)
		gtk_widget_set_hexpand(l, TRUE);
	return l;
}

static GtkWidget *button(const char *text, GCallback cb, gpointer data)
{
	GtkWidget *b = gtk_button_new_with_label(text);
	g_signal_connect(b, "clicked", cb, data);
	return b;
}

static GtkWidget *hbox(int spacing)
{
	return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
}

static GtkWidget *vbox(int spacing)
{
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
}

static GtkWidget *mk_row(GtkWidget *content)
{
	GtkWidget *r = gtk_list_box_row_new();
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), content);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
	return r;
}

static GtkWidget *mk_list(void)
{
	GtkWidget *l = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(l), GTK_SELECTION_NONE);
	return l;
}

static GtkWidget *scroller(GtkWidget *child)
{
	GtkWidget *s = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
	gtk_widget_set_vexpand(s, TRUE);
	return s;
}

static void clear_list(GtkWidget *l)
{
	GtkWidget *c;
	while ((c = gtk_widget_get_first_child(l)))
		gtk_list_box_remove(GTK_LIST_BOX(l), c);
}

static void tag(GtkWidget *w, const char *key, const char *val)
{
	g_object_set_data_full(G_OBJECT(w), key, g_strdup(val), g_free);
}

static const char *txt(GtkWidget *e)
{
	return gtk_editable_get_text(GTK_EDITABLE(e));
}

static GtkWidget *entry_with(const char *text)
{
	GtkWidget *e = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(e), text ? text : "");
	gtk_widget_set_hexpand(e, TRUE);
	return e;
}

static void grid_add(GtkWidget *grid, int row, const char *name, GtkWidget *w, GtkWidget *extra)
{
	GtkWidget *l = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(l), 1);
	gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
	if (extra)
		gtk_grid_attach(GTK_GRID(grid), extra, 2, row, 1, 1);
}

typedef struct {
	PickCb cb;
	gpointer ud;
	gboolean dir;
} Pick;

static void pick_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Pick *p = data;
	GFile *f = p->dir ? gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL)
			  : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
	if (f) {
		char *path = g_file_get_path(f);
		p->cb(path, p->ud);
		g_free(path);
		g_object_unref(f);
	} else {
		p->cb(NULL, p->ud);
	}
	g_free(p);
}

static void pick(GtkWindow *parent, const char *title, const char *folder, gboolean dir, PickCb cb, gpointer ud)
{
	GtkFileDialog *d = gtk_file_dialog_new();
	gtk_file_dialog_set_title(d, title);
	if (folder && *folder && g_file_test(folder, G_FILE_TEST_IS_DIR)) {
		GFile *f = g_file_new_for_path(folder);
		gtk_file_dialog_set_initial_folder(d, f);
		g_object_unref(f);
	}
	Pick *p = g_new0(Pick, 1);
	p->cb = cb;
	p->ud = ud;
	p->dir = dir;
	if (dir)
		gtk_file_dialog_select_folder(d, parent, NULL, pick_done, p);
	else
		gtk_file_dialog_open(d, parent, NULL, pick_done, p);
	g_object_unref(d);
}

static char *game_folder(const Game *g)
{
	if (*g->exe)
		return g_path_get_dirname(g->exe);
	return g_strdup(g->dir);
}

static void open_folder(const Game *g)
{
	char *dir = game_folder(g);
	if (!*dir || !g_file_test(dir, G_FILE_TEST_IS_DIR)) {
		alert("The game folder does not exist.");
		g_free(dir);
		return;
	}
	GError *e = NULL;
	char **argv = cmd_expand(gl_settings.folder_cmd, dir, &e);
	if (!argv || !g_spawn_async(dir, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
		char *m = g_strdup_printf("Cannot run \"%s\": %s", gl_settings.folder_cmd, e ? e->message : "bad command");
		alert(m);
		g_free(m);
		g_clear_error(&e);
	}
	g_strfreev(argv);
	g_free(dir);
}

static char *game_state(const Game *g)
{
	if (g_hash_table_contains(A.running, g->id))
		return g_strdup("running");
	if (!*g->exe)
		return g_strdup("no exe set");
	return g_path_get_basename(g->exe);
}

static void game_exited(GObject *src, GAsyncResult *res, gpointer data)
{
	char *id = data;
	GSubprocess *p = G_SUBPROCESS(src);
	g_subprocess_wait_finish(p, res, NULL);
	gboolean failed = g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) != 0;
	int code = failed ? g_subprocess_get_exit_status(p) : 0;
	g_hash_table_remove(A.running, id);
	Game *g = lib_find_id(id);
	set_status("%s exited", g ? g->name : "game");
	rebuild_library();
	if (failed) {
		char *fn = g_strconcat(id, ".log", NULL);
		char *log = gl_path(TRUE, "logs", fn);
		g_free(fn);
		char *body = NULL;
		gsize n = 0;
		char *tail = NULL;
		if (g_file_get_contents(log, &body, &n, NULL)) {
			const char *from = n > 1500 ? body + n - 1500 : body;
			if (n > 1500) {
				const char *nl = strchr(from, '\n');
				if (nl)
					from = nl + 1;
			}
			tail = g_strdup(from);
		}
		char *msg = g_strdup_printf("%s exited with code %d.\n\nLog %s:\n\n%s", g ? g->name : "game", code, log,
					    tail && *tail ? tail : "(empty)");
		alert(msg);
		g_free(msg);
		g_free(tail);
		g_free(body);
		g_free(log);
	}
	g_free(id);
}

static void launch(Game *g, const char *exe, const char *args)
{
	if (g_hash_table_contains(A.running, g->id)) {
		set_status("%s is already running", g->name);
		return;
	}
	char *log = NULL;
	GError *e = NULL;
	GSubprocess *p = proton_run(g, exe, args, &log, &e);
	if (!p) {
		alert(e ? e->message : "cannot start");
		g_clear_error(&e);
		g_free(log);
		return;
	}
	g_hash_table_insert(A.running, g_strdup(g->id), p);
	set_status("started %s, log: %s", g->name, log);
	g_free(log);
	g_subprocess_wait_async(p, NULL, game_exited, g_strdup(g->id));
	rebuild_library();
}

static Game *game_of(GtkWidget *w)
{
	const char *id = g_object_get_data(G_OBJECT(w), "gid");
	return id ? lib_find_id(id) : NULL;
}

static void on_play(GtkButton *b, gpointer d)
{
	(void)d;
	Game *g = game_of(GTK_WIDGET(b));
	if (!g)
		return;
	GSubprocess *p = g_hash_table_lookup(A.running, g->id);
	if (p) {
		g_subprocess_send_signal(p, 15);
		return;
	}
	if (!*g->exe) {
		alert("Set the game exe first (Edit).");
		return;
	}
	launch(g, g->exe, g->args);
}

static void run_picked(const char *path, gpointer ud)
{
	Game *g = path ? lib_find_id(ud) : NULL;
	if (g)
		launch(g, path, "");
	g_free(ud);
}

static void on_run_other(GtkButton *b, gpointer d)
{
	(void)d;
	Game *g = game_of(GTK_WIDGET(b));
	if (!g)
		return;
	char *id = g_strdup(g->id);
	char *start = game_folder(g);
	pick(A.win, "Run an exe in this game's prefix (setup, winecfg tools)", start, FALSE, run_picked, id);
	g_free(start);
}

static void on_folder(GtkButton *b, gpointer d)
{
	(void)d;
	Game *g = game_of(GTK_WIDGET(b));
	if (g)
		open_folder(g);
}

static void on_remove_game(GtkButton *b, gpointer d)
{
	(void)d;
	Game *g = game_of(GTK_WIDGET(b));
	if (!g)
		return;
	GSubprocess *p = g_hash_table_lookup(A.running, g->id);
	if (p)
		g_subprocess_send_signal(p, 15);
	lib_remove(g);
	lib_save();
	rebuild_library();
}

typedef struct {
	char *gid;
	GtkWidget *entry;
	gboolean manual;
} CoverCtx;

static void cover_done(const char *err, gpointer ud)
{
	CoverCtx *c = ud;
	Game *g = lib_find_id(c->gid);
	if (!err && g) {
		char *dest = gl_path(TRUE, "covers", NULL);
		char *name = g_strconcat(g->id, ".jpg", NULL);
		char *path = g_build_filename(dest, name, NULL);
		SETSTR(g->cover, path);
		if (c->entry)
			gtk_editable_set_text(GTK_EDITABLE(c->entry), path);
		g_free(path);
		g_free(name);
		g_free(dest);
		lib_save();
		rebuild_library();
		set_status("cover found for %s", g->name);
	} else if (err && c->manual) {
		alert(err);
	} else if (err) {
		set_status("%s", err);
	}
	if (c->entry)
		g_object_remove_weak_pointer(G_OBJECT(c->entry), (gpointer *)&c->entry);
	g_free(c->gid);
	g_free(c);
}

static void fetch_cover(const Game *g, const char *name, GtkWidget *entry, gboolean manual)
{
	char *dir = gl_path(TRUE, "covers", NULL);
	gl_mkdir(dir);
	char *fn = g_strconcat(g->id, ".jpg", NULL);
	char *dest = g_build_filename(dir, fn, NULL);
	CoverCtx *c = g_new0(CoverCtx, 1);
	c->gid = g_strdup(g->id);
	c->manual = manual;
	if (entry) {
		c->entry = entry;
		g_object_add_weak_pointer(G_OBJECT(entry), (gpointer *)&c->entry);
	}
	set_status("looking for the cover of %s...", name);
	steam_cover(name, dest, cover_done, c);
	g_free(dest);
	g_free(fn);
	g_free(dir);
}

typedef struct {
	char *gid;
	GtkWidget *win;
	GtkWidget *name;
	GtkWidget *exe;
	GtkWidget *cover;
	GtkWidget *prefix;
	GtkWidget *args;
	GtkWidget *env;
	GtkWidget *wrap;
	GtkWidget *proton;
	GPtrArray *vers;
} Edit;

static void edit_free(GtkWidget *w, gpointer d)
{
	(void)w;
	Edit *e = d;
	g_free(e->gid);
	g_ptr_array_free(e->vers, TRUE);
	g_free(e);
}

static void edit_save(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	Game *g = lib_find_id(e->gid);
	if (g) {
		SETSTR(g->name, txt(e->name));
		SETSTR(g->exe, txt(e->exe));
		SETSTR(g->cover, txt(e->cover));
		SETSTR(g->prefix, txt(e->prefix));
		SETSTR(g->args, txt(e->args));
		SETSTR(g->env, txt(e->env));
		SETSTR(g->wrapper, txt(e->wrap));
		guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(e->proton));
		SETSTR(g->proton, sel > 0 && sel - 1 < e->vers->len ? ((ProtonVer *)g_ptr_array_index(e->vers, sel - 1))->name : "");
		lib_save();
	}
	gtk_window_destroy(GTK_WINDOW(e->win));
	rebuild_library();
}

static void edit_cancel(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	gtk_window_destroy(GTK_WINDOW(e->win));
}

static void exe_picked(const char *path, gpointer d)
{
	Edit *e = d;
	if (path)
		gtk_editable_set_text(GTK_EDITABLE(e->exe), path);
}

static void cover_picked(const char *path, gpointer d)
{
	Edit *e = d;
	if (path)
		gtk_editable_set_text(GTK_EDITABLE(e->cover), path);
}

static void prefix_picked(const char *path, gpointer d)
{
	Edit *e = d;
	if (path)
		gtk_editable_set_text(GTK_EDITABLE(e->prefix), path);
}

static void edit_browse_exe(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	char *start = g_path_get_dirname(txt(e->exe));
	pick(GTK_WINDOW(e->win), "Game exe", start, FALSE, exe_picked, e);
	g_free(start);
}

static void edit_browse_cover(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	pick(GTK_WINDOW(e->win), "Cover image", gl_settings.dl_dir, FALSE, cover_picked, e);
}

static void edit_find_cover(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	Game *g = lib_find_id(e->gid);
	if (g && *txt(e->name))
		fetch_cover(g, txt(e->name), e->cover, TRUE);
}

static void edit_browse_prefix(GtkButton *b, gpointer d)
{
	(void)b;
	Edit *e = d;
	pick(GTK_WINDOW(e->win), "Wine prefix folder", gl_settings.dl_dir, TRUE, prefix_picked, e);
}

static GtkWidget *row_buttons(GtkWidget *a, GtkWidget *b)
{
	GtkWidget *h = hbox(6);
	gtk_box_append(GTK_BOX(h), a);
	gtk_box_append(GTK_BOX(h), b);
	return h;
}

static void show_edit(Game *g)
{
	Edit *e = g_new0(Edit, 1);
	e->gid = g_strdup(g->id);
	e->vers = proton_list();
	e->win = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(e->win), "Edit game");
	gtk_window_set_modal(GTK_WINDOW(e->win), TRUE);
	gtk_window_set_transient_for(GTK_WINDOW(e->win), A.win);
	gtk_window_set_default_size(GTK_WINDOW(e->win), 720, -1);
	g_signal_connect(e->win, "destroy", G_CALLBACK(edit_free), e);

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	e->name = entry_with(g->name);
	e->exe = entry_with(g->exe);
	e->cover = entry_with(g->cover);
	e->prefix = entry_with(g->prefix);
	e->args = entry_with(g->args);
	e->env = entry_with(g->env);
	e->wrap = entry_with(g->wrapper);
	GPtrArray *names = g_ptr_array_new();
	g_ptr_array_add(names, (gpointer) "(default)");
	guint sel = 0;
	for (guint i = 0; i < e->vers->len; i++) {
		ProtonVer *v = g_ptr_array_index(e->vers, i);
		g_ptr_array_add(names, v->name);
		if (!strcmp(v->name, g->proton))
			sel = i + 1;
	}
	g_ptr_array_add(names, NULL);
	e->proton = gtk_drop_down_new_from_strings((const char *const *)names->pdata);
	g_ptr_array_free(names, TRUE);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(e->proton), sel);
	gtk_widget_set_hexpand(e->proton, TRUE);

	grid_add(grid, 0, "Name", e->name, NULL);
	grid_add(grid, 1, "Exe", e->exe, button("Browse", G_CALLBACK(edit_browse_exe), e));
	grid_add(grid, 2, "Cover", e->cover,
		 row_buttons(button("Browse", G_CALLBACK(edit_browse_cover), e), button("Find on Steam", G_CALLBACK(edit_find_cover), e)));
	grid_add(grid, 3, "Proton", e->proton, NULL);
	grid_add(grid, 4, "Prefix", e->prefix, button("Browse", G_CALLBACK(edit_browse_prefix), e));
	grid_add(grid, 5, "Arguments", e->args, NULL);
	grid_add(grid, 6, "Environment (A=1 B=2)", e->env, NULL);
	grid_add(grid, 7, "Wrapper (gamemoderun, mangohud)", e->wrap, NULL);

	GtkWidget *btns = hbox(8);
	gtk_widget_set_halign(btns, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(btns), button("Cancel", G_CALLBACK(edit_cancel), e));
	GtkWidget *save = button("Save", G_CALLBACK(edit_save), e);
	gtk_widget_add_css_class(save, "suggested-action");
	gtk_box_append(GTK_BOX(btns), save);

	GtkWidget *box = vbox(12);
	pad(box, 16);
	gtk_box_append(GTK_BOX(box), grid);
	gtk_box_append(GTK_BOX(box), btns);
	gtk_window_set_child(GTK_WINDOW(e->win), box);
	gtk_window_present(GTK_WINDOW(e->win));
}

static void on_edit(GtkButton *b, gpointer d)
{
	(void)d;
	Game *g = game_of(GTK_WIDGET(b));
	if (g)
		show_edit(g);
}

static GtkWidget *tagged_button(const char *text, GCallback cb, const Game *g)
{
	GtkWidget *b = button(text, cb, NULL);
	tag(b, "gid", g->id);
	return b;
}

static GtkWidget *make_cover(const Game *g)
{
	GtkWidget *ov = gtk_overlay_new();
	gtk_widget_add_css_class(ov, "cover");
	GtkWidget *pic = gtk_picture_new();
	gtk_widget_set_size_request(pic, 200, 290);
	gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
	gtk_overlay_set_child(GTK_OVERLAY(ov), pic);
	if (*g->cover && g_file_test(g->cover, G_FILE_TEST_EXISTS)) {
		gtk_picture_set_filename(GTK_PICTURE(pic), g->cover);
	} else {
		char letter[8] = "?";
		if (*g->name) {
			char *up = g_utf8_strup(g->name, g_utf8_next_char(g->name) - g->name);
			g_strlcpy(letter, up, sizeof letter);
			g_free(up);
		}
		GtkWidget *l = gtk_label_new(letter);
		gtk_widget_add_css_class(l, "big-letter");
		gtk_overlay_add_overlay(GTK_OVERLAY(ov), l);
	}
	return ov;
}

static void rebuild_library(void)
{
	GtkWidget *c;
	while ((c = gtk_widget_get_first_child(A.lib_flow)))
		gtk_flow_box_remove(GTK_FLOW_BOX(A.lib_flow), c);
	gtk_widget_set_visible(A.lib_empty, gl_games->len == 0);
	for (guint i = 0; i < gl_games->len; i++) {
		Game *g = g_ptr_array_index(gl_games, i);
		GtkWidget *tile = vbox(8);
		gtk_widget_add_css_class(tile, "tile");
		gtk_box_append(GTK_BOX(tile), make_cover(g));
		GtkWidget *name = gtk_label_new(g->name);
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(name), 22);
		gtk_widget_add_css_class(name, "heading");
		gtk_box_append(GTK_BOX(tile), name);
		char *st = game_state(g);
		GtkWidget *state = gtk_label_new(st);
		gtk_label_set_ellipsize(GTK_LABEL(state), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(state), 22);
		gtk_widget_add_css_class(state, "dim-label");
		g_free(st);
		gtk_box_append(GTK_BOX(tile), state);

		gboolean running = g_hash_table_contains(A.running, g->id);
		GtkWidget *play = tagged_button(running ? "Stop" : "Play", G_CALLBACK(on_play), g);
		gtk_widget_add_css_class(play, running ? "destructive-action" : "suggested-action");
		gtk_widget_set_sensitive(play, running || *g->exe);
		gtk_box_append(GTK_BOX(tile), play);

		GtkWidget *row1 = hbox(6);
		GtkWidget *folder = tagged_button("Folder", G_CALLBACK(on_folder), g);
		GtkWidget *edit = tagged_button("Edit", G_CALLBACK(on_edit), g);
		gtk_widget_set_hexpand(folder, TRUE);
		gtk_widget_set_hexpand(edit, TRUE);
		gtk_box_append(GTK_BOX(row1), folder);
		gtk_box_append(GTK_BOX(row1), edit);
		gtk_box_append(GTK_BOX(tile), row1);
		GtkWidget *row2 = hbox(6);
		GtkWidget *other = tagged_button("Run exe...", G_CALLBACK(on_run_other), g);
		GtkWidget *rm = tagged_button("Remove", G_CALLBACK(on_remove_game), g);
		gtk_widget_set_hexpand(other, TRUE);
		gtk_widget_set_hexpand(rm, TRUE);
		gtk_box_append(GTK_BOX(row2), other);
		gtk_box_append(GTK_BOX(row2), rm);
		gtk_box_append(GTK_BOX(tile), row2);
		gtk_flow_box_append(GTK_FLOW_BOX(A.lib_flow), tile);
	}
}

static void exe_added(const char *path, gpointer d)
{
	(void)d;
	if (!path)
		return;
	char *base = g_path_get_basename(path);
	char *dot = strrchr(base, '.');
	if (dot && dot != base)
		*dot = 0;
	Game *g = game_new(base);
	SETSTR(g->exe, path);
	char *dir = g_path_get_dirname(path);
	SETSTR(g->dir, dir);
	g_free(dir);
	g_free(base);
	lib_add(g);
	lib_save();
	rebuild_library();
	fetch_cover(g, g->name, NULL, FALSE);
	show_edit(g);
}

static void on_add_exe(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	pick(A.win, "Pick the game exe", gl_settings.dl_dir, FALSE, exe_added, NULL);
}

static GtkWidget *build_library(void)
{
	GtkWidget *page = vbox(8);
	pad(page, 12);
	GtkWidget *bar = hbox(8);
	GtkWidget *title = label("Library", TRUE);
	gtk_widget_add_css_class(title, "title");
	gtk_box_append(GTK_BOX(bar), title);
	GtkWidget *add = button("Add exe", G_CALLBACK(on_add_exe), NULL);
	gtk_widget_add_css_class(add, "suggested-action");
	gtk_box_append(GTK_BOX(bar), add);
	gtk_box_append(GTK_BOX(page), bar);
	A.lib_empty = label("No games yet. Press Add exe and pick a game's .exe, it will run through Proton.", TRUE);
	gtk_widget_add_css_class(A.lib_empty, "dim-label");
	gtk_box_append(GTK_BOX(page), A.lib_empty);
	A.lib_flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(A.lib_flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(A.lib_flow), TRUE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(A.lib_flow), 1);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(A.lib_flow), 20);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(A.lib_flow), 8);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(A.lib_flow), 8);
	gtk_widget_set_valign(A.lib_flow, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), scroller(A.lib_flow));
	return page;
}

static void job_free(gpointer p)
{
	Job *j = p;
	g_free(j->tag);
	g_free(j->dest);
	g_free(j);
}

static gboolean job_tick(gpointer d)
{
	(void)d;
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, A.jobs);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Job *j = v;
		GStatBuf st;
		if (!j->unpack && g_stat(j->dest, &st) == 0 && j->size > 0)
			j->pct = (int)MIN(99, (gint64)st.st_size * 100 / j->size);
	}
	rebuild_proton(FALSE);
	if (g_hash_table_size(A.jobs) == 0) {
		A.job_timer = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void on_make_default(GtkButton *b, gpointer d)
{
	(void)d;
	SETSTR(gl_settings.default_proton, g_object_get_data(G_OBJECT(b), "pname"));
	settings_save();
	rebuild_proton(TRUE);
}

static void on_proton_remove(GtkButton *b, gpointer d)
{
	(void)d;
	const char *name = g_object_get_data(G_OBJECT(b), "pname");
	GPtrArray *vers = proton_list();
	for (guint i = 0; i < vers->len; i++) {
		ProtonVer *v = g_ptr_array_index(vers, i);
		if (!strcmp(v->name, name))
			set_status(proton_remove(v) ? "removed %s" : "cannot remove %s", name);
	}
	g_ptr_array_free(vers, TRUE);
	rebuild_proton(TRUE);
}

static void unpacked(const char *err, gpointer ud)
{
	char *name = ud;
	Job *j = g_hash_table_lookup(A.jobs, name);
	if (j)
		g_unlink(j->dest);
	g_hash_table_remove(A.jobs, name);
	if (err)
		alert(err);
	else
		set_status("%s installed", name);
	rebuild_proton(TRUE);
	g_free(name);
}

static void downloaded(const char *err, gpointer ud)
{
	char *name = ud;
	Job *j = g_hash_table_lookup(A.jobs, name);
	if (err || !j) {
		if (err)
			alert(err);
		if (j)
			g_unlink(j->dest);
		g_hash_table_remove(A.jobs, name);
		rebuild_proton(FALSE);
		g_free(name);
		return;
	}
	j->unpack = TRUE;
	set_status("unpacking %s...", name);
	rebuild_proton(FALSE);
	proton_extract(j->dest, unpacked, name);
}

static void on_proton_install(GtkButton *b, gpointer d)
{
	(void)d;
	const char *url = g_object_get_data(G_OBJECT(b), "url");
	const char *name = g_object_get_data(G_OBJECT(b), "pname");
	const char *file = g_object_get_data(G_OBJECT(b), "file");
	gint64 size = g_ascii_strtoll(g_object_get_data(G_OBJECT(b), "size"), NULL, 10);
	if (g_hash_table_contains(A.jobs, name))
		return;
	char *dir = gl_path(TRUE, "proton-dl", NULL);
	gl_mkdir(dir);
	Job *j = g_new0(Job, 1);
	j->tag = g_strdup(name);
	j->dest = g_build_filename(dir, file, NULL);
	j->size = size;
	g_free(dir);
	g_hash_table_insert(A.jobs, g_strdup(name), j);
	proton_download(url, j->dest, downloaded, g_strdup(name));
	set_status("downloading %s...", name);
	if (!A.job_timer)
		A.job_timer = g_timeout_add(700, job_tick, NULL);
	rebuild_proton(FALSE);
}

static void rels_done(GPtrArray *rels, const char *err, gpointer ud)
{
	(void)ud;
	if (!rels) {
		alert(err ? err : "cannot fetch releases");
		return;
	}
	g_ptr_array_set_size(A.rels, 0);
	for (guint i = 0; i < rels->len; i++) {
		ProtonRel *s = g_ptr_array_index(rels, i);
		ProtonRel *c = g_new0(ProtonRel, 1);
		c->tag = g_strdup(s->tag);
		c->url = g_strdup(s->url);
		c->file = g_strdup(s->file);
		c->size = s->size;
		g_ptr_array_add(A.rels, c);
	}
	set_status("%u Proton-GE releases", rels->len);
	rebuild_proton(FALSE);
}

static void on_proton_refresh(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	set_status("fetching Proton-GE releases...");
	proton_fetch_releases(rels_done, NULL);
}

static void refresh_proton_choices(void)
{
	GPtrArray *vers = proton_list();
	GPtrArray *names = g_ptr_array_new();
	g_ptr_array_add(names, (gpointer) "(first found)");
	guint sel = 0;
	for (guint i = 0; i < vers->len; i++) {
		ProtonVer *v = g_ptr_array_index(vers, i);
		g_ptr_array_add(names, v->name);
		if (!strcmp(v->name, gl_settings.default_proton))
			sel = i + 1;
	}
	g_ptr_array_add(names, NULL);
	gtk_drop_down_set_model(GTK_DROP_DOWN(A.st_proton), G_LIST_MODEL(gtk_string_list_new((const char *const *)names->pdata)));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(A.st_proton), sel);
	g_ptr_array_free(names, TRUE);
	if (A.st_vers)
		g_ptr_array_free(A.st_vers, TRUE);
	A.st_vers = vers;
}

static void rebuild_proton(gboolean choices)
{
	clear_list(A.pr_inst);
	clear_list(A.pr_avail);
	GPtrArray *vers = proton_list();
	for (guint i = 0; i < vers->len; i++) {
		ProtonVer *v = g_ptr_array_index(vers, i);
		GtkWidget *row = hbox(8);
		pad(row, 8);
		GtkWidget *col = vbox(2);
		gtk_widget_set_hexpand(col, TRUE);
		gboolean def = !strcmp(v->name, gl_settings.default_proton);
		char *title = g_strdup_printf("%s%s", v->name, def ? "  (default)" : "");
		GtkWidget *tl = label(title, TRUE);
		gtk_widget_add_css_class(tl, "heading");
		gtk_box_append(GTK_BOX(col), tl);
		g_free(title);
		GtkWidget *p = label(v->path, TRUE);
		gtk_widget_add_css_class(p, "dim-label");
		gtk_box_append(GTK_BOX(col), p);
		gtk_box_append(GTK_BOX(row), col);
		GtkWidget *mk = button("Make default", G_CALLBACK(on_make_default), NULL);
		tag(mk, "pname", v->name);
		gtk_widget_set_sensitive(mk, !def);
		gtk_widget_set_valign(mk, GTK_ALIGN_CENTER);
		gtk_box_append(GTK_BOX(row), mk);
		if (v->ours) {
			GtkWidget *rm = button("Remove", G_CALLBACK(on_proton_remove), NULL);
			tag(rm, "pname", v->name);
			gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
			gtk_box_append(GTK_BOX(row), rm);
		}
		gtk_list_box_append(GTK_LIST_BOX(A.pr_inst), mk_row(row));
	}
	for (guint i = 0; i < A.rels->len; i++) {
		ProtonRel *r = g_ptr_array_index(A.rels, i);
		gboolean have = FALSE;
		char *dirname = g_strndup(r->file, strlen(r->file) - 7);
		for (guint j = 0; j < vers->len; j++) {
			const char *vn = ((ProtonVer *)g_ptr_array_index(vers, j))->name;
			if (!strcmp(vn, dirname) || !strcmp(vn, r->tag))
				have = TRUE;
		}
		g_free(dirname);
		Job *job = g_hash_table_lookup(A.jobs, r->tag);
		GtkWidget *row = hbox(8);
		pad(row, 8);
		char *sz = fmt_size(r->size);
		char *t = g_strdup_printf("%s  \xc2\xb7  %s", r->tag, sz);
		gtk_box_append(GTK_BOX(row), label(t, TRUE));
		g_free(t);
		g_free(sz);
		char *bl;
		if (job)
			bl = job->unpack ? g_strdup("Unpacking...") : g_strdup_printf("%d%%", job->pct);
		else
			bl = g_strdup(have ? "Installed" : "Install");
		GtkWidget *b = button(bl, G_CALLBACK(on_proton_install), NULL);
		g_free(bl);
		char *size = g_strdup_printf("%" G_GINT64_FORMAT, r->size);
		tag(b, "url", r->url);
		tag(b, "pname", r->tag);
		tag(b, "file", r->file);
		tag(b, "size", size);
		g_free(size);
		gtk_widget_set_sensitive(b, !have && !job);
		gtk_box_append(GTK_BOX(row), b);
		gtk_list_box_append(GTK_LIST_BOX(A.pr_avail), mk_row(row));
	}
	g_ptr_array_free(vers, TRUE);
	if (choices)
		refresh_proton_choices();
}

static GtkWidget *build_proton(void)
{
	GtkWidget *page = vbox(8);
	pad(page, 12);
	GtkWidget *t1 = label("Installed Proton", TRUE);
	gtk_widget_add_css_class(t1, "title");
	gtk_box_append(GTK_BOX(page), t1);
	A.pr_inst = mk_list();
	gtk_box_append(GTK_BOX(page), scroller(A.pr_inst));
	GtkWidget *bar = hbox(8);
	GtkWidget *t2 = label("Proton-GE releases", TRUE);
	gtk_widget_add_css_class(t2, "title");
	gtk_box_append(GTK_BOX(bar), t2);
	gtk_box_append(GTK_BOX(bar), button("Refresh", G_CALLBACK(on_proton_refresh), NULL));
	gtk_box_append(GTK_BOX(page), bar);
	A.pr_avail = mk_list();
	gtk_box_append(GTK_BOX(page), scroller(A.pr_avail));
	return page;
}

static void dir_picked(const char *path, gpointer d)
{
	(void)d;
	if (path)
		gtk_editable_set_text(GTK_EDITABLE(A.st_dir), path);
}

static void on_dir_browse(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	pick(A.win, "Games folder", txt(A.st_dir), TRUE, dir_picked, NULL);
}

static void on_settings_save(GtkButton *b, gpointer d)
{
	(void)b;
	(void)d;
	SETSTR(gl_settings.dl_dir, txt(A.st_dir));
	SETSTR(gl_settings.proxy, txt(A.st_proxy));
	SETSTR(gl_settings.folder_cmd, *txt(A.st_cmd) ? txt(A.st_cmd) : "alacritty --working-directory %d");
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(A.st_proton));
	SETSTR(gl_settings.default_proton, sel > 0 && A.st_vers && sel - 1 < A.st_vers->len ? ((ProtonVer *)g_ptr_array_index(A.st_vers, sel - 1))->name : "");
	settings_save();
	rebuild_proton(TRUE);
	set_status("settings saved");
}

static GtkWidget *build_settings(void)
{
	GtkWidget *page = vbox(12);
	pad(page, 12);
	GtkWidget *t = label("Settings", TRUE);
	gtk_widget_add_css_class(t, "title");
	gtk_box_append(GTK_BOX(page), t);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	A.st_dir = entry_with(gl_settings.dl_dir);
	A.st_proxy = entry_with(gl_settings.proxy);
	gtk_entry_set_placeholder_text(GTK_ENTRY(A.st_proxy), "socks5h://127.0.0.1:1080  (empty = direct)");
	A.st_cmd = entry_with(gl_settings.folder_cmd);
	A.st_proton = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_hexpand(A.st_proton, TRUE);
	grid_add(grid, 0, "Games folder (start of file pickers)", A.st_dir, button("Browse", G_CALLBACK(on_dir_browse), NULL));
	grid_add(grid, 1, "Open folder command (%d = folder)", A.st_cmd, NULL);
	grid_add(grid, 2, "Proxy for Steam covers and Proton", A.st_proxy, NULL);
	grid_add(grid, 3, "Default Proton", A.st_proton, NULL);
	gtk_box_append(GTK_BOX(page), grid);
	GtkWidget *save = button("Save", G_CALLBACK(on_settings_save), NULL);
	gtk_widget_add_css_class(save, "suggested-action");
	gtk_widget_set_halign(save, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), save);
	return page;
}

static const char *THEME =
	"@define-color base #1e1e2e;\n"
	"@define-color mantle #181825;\n"
	"@define-color crust #11111b;\n"
	"@define-color surface0 #313244;\n"
	"@define-color surface1 #45475a;\n"
	"@define-color text #cdd6f4;\n"
	"@define-color subtext #a6adc8;\n"
	"@define-color blue #89b4fa;\n"
	"@define-color mauve #cba6f7;\n"
	"@define-color red #f38ba8;\n"
	"window, .background { background-color: @base; color: @text; }\n"
	"label { color: @text; }\n"
	".dim-label { color: @subtext; }\n"
	".heading { font-weight: 700; }\n"
	".title { font-size: 20px; font-weight: 700; }\n"
	".big-letter { font-size: 80px; font-weight: 800; color: @surface1; }\n"
	".cover { background-color: @surface0; border-radius: 10px; }\n"
	".tile { background-color: @mantle; border-radius: 14px; padding: 10px; }\n"
	"flowboxchild { background: transparent; padding: 4px; }\n"
	"stacksidebar, .sidebar { background-color: @mantle; border: none; }\n"
	"stacksidebar list, .sidebar list { background: transparent; }\n"
	"stacksidebar row { padding: 8px 10px; margin: 2px 8px; border-radius: 8px; color: @subtext; background: transparent; }\n"
	"stacksidebar row:hover { background-color: @surface0; }\n"
	"stacksidebar row:selected { background-color: @surface0; color: @text; }\n"
	"list { background: transparent; color: @text; }\n"
	"list > row { background-color: @mantle; border-radius: 10px; margin: 3px 6px; }\n"
	"list > row:hover { background-color: @surface0; }\n"
	"stacksidebar list > row { margin: 2px 8px; }\n"
	"entry, passwordentry { background-color: @mantle; color: @text; border: 1px solid @surface1; border-radius: 8px; padding: 6px 10px; box-shadow: none; }\n"
	"entry:focus-within { border-color: @mauve; }\n"
	"button { background-image: none; background-color: @surface0; color: @text; border: none; border-radius: 8px; box-shadow: none; padding: 6px 14px; }\n"
	"button:hover { background-color: @surface1; }\n"
	"button:disabled { opacity: 0.45; }\n"
	"button.suggested-action { background-color: @blue; color: @crust; font-weight: 700; }\n"
	"button.destructive-action { background-color: @red; color: @crust; font-weight: 700; }\n"
	"scrollbar slider { background-color: @surface1; border-radius: 6px; min-width: 6px; min-height: 6px; }\n"
	"scrollbar { background: transparent; }\n"
	"dropdown button, dropdown > button { background-color: @mantle; border: 1px solid @surface1; }\n"
	"popover contents, popover > contents { background-color: @mantle; color: @text; border-radius: 10px; }\n"
	"tooltip { background-color: @crust; color: @text; }\n"
	"selection { background-color: @mauve; color: @crust; }\n";

static void apply_theme(void)
{
	GtkSettings *s = gtk_settings_get_default();
	if (s && g_object_class_find_property(G_OBJECT_GET_CLASS(s), "gtk-interface-color-scheme"))
		g_object_set(s, "gtk-interface-color-scheme", 2, NULL);
	else if (s)
		g_object_set(s, "gtk-application-prefer-dark-theme", TRUE, NULL);
	GtkCssProvider *p = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
	gtk_css_provider_load_from_string(p, THEME);
#else
	gtk_css_provider_load_from_data(p, THEME, -1);
#endif
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static void activate(GtkApplication *app, gpointer d)
{
	(void)d;
	if (A.win) {
		gtk_window_present(A.win);
		return;
	}
	apply_theme();
	settings_load();
	lib_load();
	A.running = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	A.jobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, job_free);
	A.rels = g_ptr_array_new_with_free_func((GDestroyNotify)proton_rel_free);

	A.win = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_title(A.win, "Ghost Launcher");
	gtk_window_set_default_size(A.win, 1100, 720);
	A.stack = gtk_stack_new();
	gtk_widget_set_hexpand(A.stack, TRUE);
	gtk_stack_add_titled(GTK_STACK(A.stack), build_library(), "library", "Library");
	gtk_stack_add_titled(GTK_STACK(A.stack), build_proton(), "proton", "Proton");
	gtk_stack_add_titled(GTK_STACK(A.stack), build_settings(), "settings", "Settings");
	GtkWidget *side = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(A.stack));
	gtk_widget_set_size_request(side, 170, -1);
	GtkWidget *main_row = hbox(0);
	gtk_widget_set_vexpand(main_row, TRUE);
	gtk_box_append(GTK_BOX(main_row), side);
	gtk_box_append(GTK_BOX(main_row), A.stack);
	A.status = label("", TRUE);
	pad(A.status, 6);
	GtkWidget *root = vbox(0);
	gtk_box_append(GTK_BOX(root), main_row);
	gtk_box_append(GTK_BOX(root), A.status);
	gtk_window_set_child(A.win, root);

	rebuild_library();
	rebuild_proton(TRUE);
	gtk_window_present(A.win);

	GPtrArray *vers = proton_list();
	if (vers->len == 0) {
		set_status("No Proton found, fetching Proton-GE releases: install one in the Proton tab");
		proton_fetch_releases(rels_done, NULL);
	}
	g_ptr_array_free(vers, TRUE);
}

int main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new("org.ghost.launcher", G_APPLICATION_DEFAULT_FLAGS);
	A.app = app;
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int rc = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return rc;
}
