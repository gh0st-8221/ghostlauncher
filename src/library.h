#ifndef LIBRARY_H
#define LIBRARY_H
#include <glib.h>
#include "json.h"

typedef struct {
	char *id;
	char *name;
	char *exe;
	char *dir;
	char *proton;
	char *prefix;
	char *args;
	char *env;
	char *wrapper;
	char *cover;
} Game;

typedef struct {
	char *dl_dir;
	char *folder_cmd;
	char *proxy;
	char *default_proton;
} Settings;

extern Settings gl_settings;
extern GPtrArray *gl_games;

void settings_load(void);
void settings_save(void);
void lib_load(void);
void lib_save(void);
Game *game_new(const char *name);
void game_free(Game *g);
void lib_add(Game *g);
void lib_remove(Game *g);
Game *lib_find_id(const char *id);
char *game_prefix(const Game *g);

#endif
