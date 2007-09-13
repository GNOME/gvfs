#include <config.h>

#include <unistd.h>
#include <errno.h>
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

gboolean
g_file_info_simple_get (const char *path,
			GFileInfo *info,
			GFileInfoRequestFlags requested,
			const char *attributes,
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

  return TRUE;
}
