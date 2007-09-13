#ifndef __G_FILE_REF_H__
#define __G_FILE_REF_H__

#include <glib.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_FILE_REF_PORT_NONE -1
#define G_FILE_REF_PORT_ANY -2

typedef struct {
  char *protocol;
  char *username;
  char *host;
  int port;
  char *path;
} GFileRef;

typedef struct {
  char *protocol;
  char *username;
  char *host;
  int port;
  char *path_prefix;
  int max_path_depth;
  int min_path_depth;
} GFileRefTemplate;

void              g_file_ref_free               (GFileRef         *ref);
gboolean          g_file_ref_template_matches   (GFileRefTemplate *template,
						 GFileRef         *ref);
gboolean          g_file_ref_template_equal     (GFileRefTemplate *a,
						 GFileRefTemplate *b);
void              g_file_ref_template_free      (GFileRefTemplate *template);
void              g_file_ref_template_append    (GFileRefTemplate *template,
						 DBusMessageIter  *iter);
GFileRefTemplate *g_file_ref_template_from_dbus (DBusMessageIter  *iter);

G_END_DECLS

#endif /* __G_FILE_REF_H__ */
