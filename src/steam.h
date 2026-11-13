#ifndef STEAM_H
#define STEAM_H
#include "util.h"

char *steam_parse_search(const char *json, const char *name);
void steam_cover(const char *name, const char *dest, DoneCb cb, gpointer ud);

#endif
