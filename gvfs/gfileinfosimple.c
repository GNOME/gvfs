#include <config.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <sys/types.h>
#include <attr/xattr.h>

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

static void
get_one_xattr (const char *path,
	       GFileInfo *info,
	       const char *attr,
	       gboolean follow_symlinks)
{
  char value[65];
  char *value_p;
  ssize_t res;
  char *full_attr;

  if (follow_symlinks)  
    res = getxattr (path, attr, value, sizeof (value)-1);
  else
    res = lgetxattr (path, attr,value, sizeof (value)-1);

  value_p = NULL;
  if (res > 0) {
    /* Null terminate */
    value[res] = 0;
    value_p = value;
  } else if (res == -1 && errno == ERANGE)
    {
      if (follow_symlinks)  
	res = getxattr (path, attr, NULL, 0);
      else
	res = lgetxattr (path, attr, NULL, 0);

      if (res < 0)
	return;

      value_p = g_malloc (res+1);

      if (follow_symlinks)  
	res = getxattr (path, attr, value_p, res);
      else
	res = lgetxattr (path, attr, value_p, res);

      if (res < 0)
	{
	  g_free (value_p);
	  return;
	}

      /* Null terminate */
      value_p[res] = 0;
    }

  full_attr = g_strconcat ("xattr:", attr, NULL);
  g_file_info_set_attribute (info, full_attr, value_p);
  g_free (full_attr);
  if (value_p != value)
    g_free (value_p);
}

static void
get_xattrs (const char *path,
	    GFileInfo *info,
	    GFileAttributeMatcher *matcher,
	    gboolean follow_symlinks)
{
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr;

  all = g_file_attribute_matcher_enumerate (matcher, "xattr");

  if (all)
    {
      if (follow_symlinks)
	list_res_size = listxattr (path, NULL, 0);
      else
	list_res_size = llistxattr (path, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      if (follow_symlinks)
	list_res_size = listxattr (path, list, list_size);
      else
	list_res_size = llistxattr (path, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  get_one_xattr (path, info, attr, follow_symlinks);
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	get_one_xattr (path, info, attr, follow_symlinks);
    }
}

gboolean
g_file_info_simple_get (const char *basename,
			const char *path,
			GFileInfo *info,
			GFileInfoRequestFlags requested,
			GFileAttributeMatcher *attribute_matcher,
			gboolean follow_symlinks,
			GError **error)
{
  struct stat statbuf;
  int res;

  if (requested & G_FILE_INFO_NAME)
    g_file_info_set_name (info, basename);

  if (requested & G_FILE_INFO_IS_HIDDEN)
    g_file_info_set_is_hidden (info,
			  basename != NULL &&
			  basename[0] == '.');

  
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
  
  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      char *link = read_link (path);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      /* TODO */
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_ICON)
    {
      /* TODO */
    }

  get_selinux_context (path, info, attribute_matcher, follow_symlinks);
  get_xattrs (path, info, attribute_matcher, follow_symlinks);
  
  return TRUE;
}
