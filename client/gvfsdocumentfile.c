/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gvfsdocumentfile.h"
#include "gvfsdocumentinputstream.h"
#include "gvfsdocumentoutputstream.h"
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gvfsdbus.h>
#include <gio/gunixfdlist.h>

static void gvfs_document_file_file_iface_init (GFileIface       *iface);

static void gvfs_document_file_read_async (GFile *file,
					   int io_priority,
					   GCancellable *cancellable,
					   GAsyncReadyCallback callback,
					   gpointer callback_data);

G_DEFINE_TYPE_WITH_CODE (GVfsDocumentFile, gvfs_document_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						gvfs_document_file_file_iface_init))

static void
gvfs_document_file_finalize (GObject *object)
{
  GVfsDocumentFile *doc;

  doc = GVFS_DOCUMENT_FILE (object);

  g_free (doc->path);
  
  if (G_OBJECT_CLASS (gvfs_document_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (gvfs_document_file_parent_class)->finalize) (object);
}

static void
gvfs_document_file_class_init (GVfsDocumentFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = gvfs_document_file_finalize;
}

static void
gvfs_document_file_init (GVfsDocumentFile *doc)
{
}

static char *
canonicalize_path (char *path)
{
  char *p, *q;

  /* Canonicalize multiple consecutive slashes */
  p = path;
  while (*p != 0)
    {
      q = p;
      while (*q && *q == '/')
	q++;

      if (q > p + 1)
	memmove (p+1, q, strlen (q)+1);

      /* Skip over the one separator */
      p++;

      /* Drop trailing (not first) slash */
      if (*p == 0 && p > path + 1)
	{
	  p--;
	  *p = 0;
	}
      
      /* Skip until next separator */
      while (*p != 0 && *p != '/')
	p++;
    }

  return path;
}

static char *
path_from_uri (const char *uri)
{
  char *to_free = NULL;
  char *path, *res, *p, *q;
  const char *path_part, *hash;
  int len = -1;

  path_part = uri + strlen ("document:");

  if (g_str_has_prefix (path_part, "///")) 
    path_part += 2;
  else if (g_str_has_prefix (path_part, "//"))
    return NULL; /* Has hostname, not valid */

  hash = strchr (path_part, '#');
  if (hash != NULL)
    {
      len = hash - path_part;
      path_part = to_free = g_strndup (path_part, len);
    }

  res = g_uri_unescape_string (path_part, "/");

  g_clear_pointer (&to_free, g_free);

  if (res == NULL)
    return NULL;

  if (*res != '/')
    {
      to_free = res;
      res = g_strconcat ("/", res, NULL);
      g_free (to_free);
    }

  return canonicalize_path (res);
}

/* Takes ownership of path */
static GFile *
gvfs_document_file_new_steals_path (char *path)
{
  GVfsDocumentFile *doc;
  
  doc = g_object_new (GVFS_TYPE_DOCUMENT_FILE, NULL);
  doc->path = path;
 
  return G_FILE (doc);
}

GFile *
gvfs_document_file_new (const char *uri)
{
  GVfsDocumentFile *doc;
  char *path;

  path = path_from_uri (uri);

  if (path == NULL)
    return NULL; /* Creates a dummy GFile */

  return gvfs_document_file_new_steals_path (path);
}

static gboolean
gvfs_document_file_is_native (GFile *file)
{
  return FALSE;
}

static gboolean
gvfs_document_file_has_uri_scheme (GFile *file,
				   const char *uri_scheme)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);

  return g_ascii_strcasecmp ("document", uri_scheme) == 0;
}

static char *
gvfs_document_file_get_uri_scheme (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  const char *scheme;

  return g_strdup ("document");
}

static char *
gvfs_document_file_get_basename (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  char *last_slash;    

  return g_path_get_basename (doc->path);
}

static char *
gvfs_document_file_get_path (GFile *file)
{
  return NULL;
}

static char *
gvfs_document_file_get_uri (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  char *res, *escaped_path;

  escaped_path = g_uri_escape_string (doc->path,
				      G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
  res = g_strconcat ("document://", escaped_path, NULL);
  g_free (escaped_path);

  return res;
}

static char *
gvfs_document_file_get_parse_name (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);

  return gvfs_document_file_get_uri (file);
}

static GFile *
gvfs_document_file_get_parent (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  GVfsDocumentFile *parent;
  char *dirname;

  if (strcmp (doc->path, "/"))
    return NULL;

  dirname = g_path_get_dirname (doc->path);

  return gvfs_document_file_new_steals_path (dirname);
}

static GFile *
gvfs_document_file_dup (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);

  return gvfs_document_file_new_steals_path (g_strdup (doc->path));
}

static guint
gvfs_document_file_hash (GFile *file)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);

  return g_str_hash (doc->path);
}

static gboolean
gvfs_document_file_equal (GFile *file1,
			  GFile *file2)
{
  GVfsDocumentFile *doc1 = GVFS_DOCUMENT_FILE (file1);
  GVfsDocumentFile *doc2 = GVFS_DOCUMENT_FILE (file2);

  return g_str_equal (doc1->path, doc2->path);
}

static const char *
match_prefix (const char *path, 
              const char *prefix)
{
  int prefix_len;

  prefix_len = strlen (prefix);
  if (strncmp (path, prefix, prefix_len) != 0)
    return NULL;
  
  /* Handle the case where prefix is the root, so that
   * the IS_DIR_SEPRARATOR check below works */
  if (prefix_len > 0 &&
      prefix[prefix_len-1] == '/')
    prefix_len--;
  
  return path + prefix_len;
}

static gboolean
gvfs_document_file_prefix_matches (GFile *parent,
				   GFile *descendant)
{
  GVfsDocumentFile *parent_doc = GVFS_DOCUMENT_FILE (parent);
  GVfsDocumentFile *descendant_doc = GVFS_DOCUMENT_FILE (descendant);
  const char *remainder;

  remainder = match_prefix (descendant_doc->path, parent_doc->path);
  if (remainder != NULL && *remainder == '/')
    return TRUE;
  return FALSE;
}

static char *
gvfs_document_file_get_relative_path (GFile *parent,
				      GFile *descendant)
{
  GVfsDocumentFile *parent_doc = GVFS_DOCUMENT_FILE (parent);
  GVfsDocumentFile *descendant_doc = GVFS_DOCUMENT_FILE (descendant);
  const char *remainder;

  remainder = match_prefix (descendant_doc->path, parent_doc->path);
  if (remainder != NULL && *remainder == '/')
    return g_strdup (remainder + 1);
  return NULL;
}

static GFile *
gvfs_document_file_resolve_relative_path (GFile *file,
					  const char *relative_path)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  char *filename;
  GFile *child;

  if (g_path_is_absolute (relative_path))
    return gvfs_document_file_new_steals_path (canonicalize_path (g_strdup (doc->path)));
  
  filename = g_build_filename (doc->path, relative_path, NULL);
  child = gvfs_document_file_new_steals_path (canonicalize_path (filename));
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
gvfs_document_file_enumerate_children (GFile      *file,
				       const char *attributes,
				       GFileQueryInfoFlags flags,
				       GCancellable *cancellable,
				       GError **error)
{
  return NULL;
}

static GFileInfo *
gvfs_document_file_query_info (GFile                *file,
			       const char           *attributes,
			       GFileQueryInfoFlags   flags,
			       GCancellable         *cancellable,
			       GError              **error)
{
  return NULL;
}

static void
gvfs_document_file_query_info_async (GFile                      *file,
				     const char                 *attributes,
				     GFileQueryInfoFlags         flags,
				     int                         io_priority,
				     GCancellable               *cancellable,
				     GAsyncReadyCallback         callback,
				     gpointer                    user_data)
{
}

static GFileInfo *
gvfs_document_file_query_info_finish (GFile                      *file,
				      GAsyncResult               *res,
				      GError                    **error)
{
  return NULL;
}

static void
gvfs_document_file_read_async (GFile *file,
			       int io_priority,
			       GCancellable *cancellable,
			       GAsyncReadyCallback callback,
			       gpointer callback_data)
{
}

static GFileInputStream *
gvfs_document_file_read_finish (GFile                  *file,
				GAsyncResult           *res,
				GError                **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);
  
  return NULL;
}

static gboolean
verify_file_path (GVfsDocumentFile *doc,
		  GError **error)
{
  if (strcmp (doc->path, "/") == 0)
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_IS_DIRECTORY,
			   _("Can't open directory"));
      return FALSE;
    }

  if (strchr (doc->path + 1, '/') != NULL)
    {
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_NOT_FOUND,
			   _("No such file"));
      return FALSE;
    }

  return TRUE;

}

static GVariant *
sync_document_call (GVfsDocumentFile *doc,
		    const char *method,
		    GVariant *parameters,
		    const GVariantType  *reply_type,
		    GUnixFDList  **out_fd_list,
		    GCancellable *cancellable,
		    GError **error)
{
  GVariant *res;
  GDBusConnection *bus;
  GUnixFDList *ret_fd_list;
  GVariant *reply, *fd_v;
  char *path;
  int handle, fd;

  if (!verify_file_path (doc, error))
    return NULL;
 
  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (bus == NULL)
    return NULL;

  path = g_build_filename ("/org/freedesktop/portal/document", doc->path, NULL);

  res = g_dbus_connection_call_with_unix_fd_list_sync (bus,
						       "org.freedesktop.portal.DocumentPortal",
						       path,
						       "org.freedesktop.portal.Document",
						       method,
						       parameters, reply_type,
						       G_DBUS_CALL_FLAGS_NONE,
						       -1,
						       NULL,
						       out_fd_list,
						       cancellable,
						       error);

  g_object_unref (bus);
  return res;
}

static GFileInputStream *
gvfs_document_file_read (GFile *file,
			 GCancellable *cancellable,
			 GError **error)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  GDBusConnection *bus;
  GUnixFDList *ret_fd_list;
  GVariant *reply, *fd_v;
  char *path;
  int handle, fd;

  reply = sync_document_call (doc, "Read",
			      g_variant_new ("()"),
			      G_VARIANT_TYPE("(h)"),
			      &ret_fd_list,
			      cancellable, error);
  if (reply == NULL)
    return NULL;

  if (ret_fd_list == NULL)
    {
      g_variant_unref (reply);
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_FAILED,
			   _("No file descriptor returned"));
      return NULL;
    }
      
  g_variant_get (reply, "(@h)", &fd_v);
  handle = g_variant_get_handle (fd_v);
  g_variant_unref (reply);

  fd = g_unix_fd_list_get (ret_fd_list, handle, error);
  g_object_unref (ret_fd_list);
  if (fd == -1)
    return NULL;

  return _gvfs_document_input_stream_new (fd);
}

static GFileOutputStream *
gvfs_document_file_create (GFile *file,
			   GFileCreateFlags flags,
			   GCancellable *cancellable,
			   GError **error)
{
  return NULL;
}

static GFileOutputStream *
gvfs_document_file_replace (GFile *file,
			    const char *etag,
			    gboolean make_backup,
			    GFileCreateFlags flags,
			    GCancellable *cancellable,
			    GError **error)
{
  GVfsDocumentFile *doc = GVFS_DOCUMENT_FILE (file);
  GDBusConnection *bus;
  GUnixFDList *ret_fd_list;
  GVariant *reply, *fd_v;
  char *path;
  int handle, fd;
  guint32 id;
  char *flags_array[] = {
    NULL
  };

  reply = sync_document_call (doc, "PrepareUpdate",
			      g_variant_new ("(s^as)",
					     etag ? etag : "",
					     flags_array),
			      G_VARIANT_TYPE("(uh)"),
			      &ret_fd_list,
			      cancellable, error);
  if (reply == NULL)
    return NULL;

  if (ret_fd_list == NULL)
    {
      g_variant_unref (reply);
      g_set_error_literal (error, G_IO_ERROR,
			   G_IO_ERROR_FAILED,
			   _("No file descriptor returned"));
      return NULL;
    }
      
  g_variant_get (reply, "(u@h)", &id, &fd_v);
  handle = g_variant_get_handle (fd_v);
  g_variant_unref (reply);

  fd = g_unix_fd_list_get (ret_fd_list, handle, error);
  g_object_unref (ret_fd_list);
  if (fd == -1)
    return NULL;

  return gvfs_document_output_stream_new (doc->path, id, fd);
}

static void
gvfs_document_file_create_async (GFile                      *file,
				 GFileCreateFlags            flags,
				 int                         io_priority,
				 GCancellable               *cancellable,
				 GAsyncReadyCallback         callback,
				 gpointer                    user_data)
{
  file_open_write_async (file,
                         0, "", FALSE, flags, io_priority,
                         cancellable,
                         callback, user_data);
}

static GFileOutputStream *
gvfs_document_file_create_finish (GFile                      *file,
				  GAsyncResult               *res,
				  GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileOutputStream *output_stream;

  output_stream = g_simple_async_result_get_op_res_gpointer (simple);
  if (output_stream)
    return g_object_ref (output_stream);

  return NULL;
}

static void
gvfs_document_file_enumerate_children_async (GFile                      *file,
                                        const char                 *attributes,
                                        GFileQueryInfoFlags         flags,
                                        int                         io_priority,
                                        GCancellable               *cancellable,
                                        GAsyncReadyCallback         callback,
                                        gpointer                    user_data)
{
}

static GFileEnumerator *
gvfs_document_file_enumerate_children_finish (GFile                      *file,
                                         GAsyncResult               *res,
                                         GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileEnumerator *enumerator;

  enumerator = g_simple_async_result_get_op_res_gpointer (simple);
  if (enumerator)
    return g_object_ref (enumerator);

  return NULL;
}

static void
gvfs_document_file_replace_async (GFile                      *file,
                             const char                 *etag,
                             gboolean                    make_backup,
                             GFileCreateFlags            flags,
                             int                         io_priority,
                             GCancellable               *cancellable,
                             GAsyncReadyCallback         callback,
                             gpointer                    user_data)
{
}

static GFileOutputStream *
gvfs_document_file_replace_finish (GFile                      *file,
                              GAsyncResult               *res,
                              GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileOutputStream *output_stream;

  output_stream = g_simple_async_result_get_op_res_gpointer (simple);
  if (output_stream)
    return g_object_ref (output_stream);

  return NULL;
}

static void
gvfs_document_file_file_iface_init (GFileIface *iface)
{
  iface->dup = gvfs_document_file_dup;
  iface->hash = gvfs_document_file_hash;
  iface->equal = gvfs_document_file_equal;
  iface->is_native = gvfs_document_file_is_native;
  iface->has_uri_scheme = gvfs_document_file_has_uri_scheme;
  iface->get_uri_scheme = gvfs_document_file_get_uri_scheme;
  iface->get_basename = gvfs_document_file_get_basename;
  iface->get_path = gvfs_document_file_get_path;
  iface->get_uri = gvfs_document_file_get_uri;
  iface->get_parse_name = gvfs_document_file_get_parse_name;
  iface->get_parent = gvfs_document_file_get_parent;
  iface->prefix_matches = gvfs_document_file_prefix_matches;
  iface->get_relative_path = gvfs_document_file_get_relative_path;
  iface->resolve_relative_path = gvfs_document_file_resolve_relative_path;
  iface->enumerate_children = gvfs_document_file_enumerate_children;
  iface->query_info = gvfs_document_file_query_info;
  iface->query_info_async = gvfs_document_file_query_info_async;
  iface->query_info_finish = gvfs_document_file_query_info_finish;
  iface->read_fn = gvfs_document_file_read;
  iface->create = gvfs_document_file_create;
  iface->replace = gvfs_document_file_replace;

  /* Async operations */

  /*
  iface->read_async = gvfs_document_file_read_async;
  iface->read_finish = gvfs_document_file_read_finish;
  iface->create_async = gvfs_document_file_create_async;
  iface->create_finish = gvfs_document_file_create_finish;
  iface->enumerate_children_async = gvfs_document_file_enumerate_children_async;
  iface->enumerate_children_finish = gvfs_document_file_enumerate_children_finish;
  iface->replace_async = gvfs_document_file_replace_async;
  iface->replace_finish = gvfs_document_file_replace_finish;
  */
}
