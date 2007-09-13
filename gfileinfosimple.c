#include <config.h>

#include <unistd.h>
#include <errno.h>
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <glib/gi18n-lib.h>

#include "gfileinfosimple.h"

static gchar *
read_link (const gchar *full_name)
{
	gchar *buffer;
	guint size;

	size = 256;
	buffer = g_malloc (size);
          
	while (1) {
		int read_size;

                read_size = readlink (full_name, buffer, size);
		if (read_size < 0) {
			g_free (buffer);
			return NULL;
		}
                if (read_size < size) {
			buffer[read_size] = 0;
			return buffer;
		}
                size *= 2;
		buffer = g_realloc (buffer, size);
	}
}


/* Get the SELinux security context */
static void
get_selinux_context (const char *path,
		     GFileInfo *info,
		     GFileAttributeMatcher *attribute_matcher,
		     gboolean follow_symlinks)
{
#ifdef HAVE_SELINUX
  char *context;

  if (!g_file_attribute_matcher_matches (attribute_matcher,
					 "selinux", "selinux:context"))
    return;
  
  if (is_selinux_enabled ()) {
    if (follow_symlinks) {
      if (lgetfilecon_raw (path, &context) < 0)
	return;
    } else {
      if (getfilecon_raw (path, &context) < 0)
	return;
    }

    if (context)
      {
	g_file_info_set_attribute (info, "selinux:context", context);
	freecon(context);
      }
  }
#endif
}

gboolean
g_file_info_simple_get (const char *path,
			GFileInfo *info,
			GFileInfoRequestFlags requested,
			GFileAttributeMatcher *attribute_matcher,
			gboolean follow_symlinks,
			GError **error)
{
  struct stat statbuf;
  int res;

  if (follow_symlinks)
    res = stat (path, &statbuf);
  else
    res = lstat (path, &statbuf);
  
  if (res == -1)
    {
      g_set_error (error,
		   G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error stating file '%s': %s"),
		   path, g_strerror (errno));
      return FALSE;
    }
  
  g_file_info_set_from_stat (info, requested, &statbuf);
  
  if (requested && G_FILE_INFO_SYMLINK_TARGET)
    {
      char *link = read_link (path);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (requested && G_FILE_INFO_ACCESS_RIGHTS)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_DISPLAY_NAME)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_EDIT_NAME)
    {
      /* TODO */
    }

  if (requested && G_FILE_INFO_MIME_TYPE)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_ICON)
    {
      /* TODO */
    }

  get_selinux_context (path, info, attribute_matcher, follow_symlinks);
  
  return TRUE;
}
