#ifndef PROTON_H
#define PROTON_H
#include "library.h"
#include "util.h"

typedef struct {
	char *name;
	char *path;
	gboolean ours;
} ProtonVer;

typedef struct {
	char *tag;
	char *url;
	char *file;
	gint64 size;
} ProtonRel;

typedef void (*ProtonRelCb)(GPtrArray *rels, const char *err, gpointer ud);

void proton_ver_free(ProtonVer *v);
void proton_rel_free(ProtonRel *r);
char *proton_install_dir(void);
GPtrArray *proton_list(void);
ProtonVer *proton_pick(GPtrArray *list, const char *name);
GPtrArray *proton_parse_releases(const char *json);
void proton_fetch_releases(ProtonRelCb cb, gpointer ud);
void proton_download(const char *url, const char *dest, DoneCb cb, gpointer ud);
void proton_extract(const char *tarball, DoneCb cb, gpointer ud);
gboolean proton_remove(const ProtonVer *v);
char *steam_root(void);
GSubprocess *proton_run(const Game *g, const char *exe, const char *extra_args, char **logpath, GError **err);

#endif
