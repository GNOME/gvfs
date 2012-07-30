/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#include <config.h>

#include <string.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "gvfsftpfile.h"

/**
 * GVfsFtpFile:
 *
 * This structure maps between GVfs paths and the actual paths as used on an
 * FTP server. The mapping may not be a 1-to-1 mapping, so always use this
 * structure if you need to do operations on paths.
 */

struct _GVfsFtpFile {
  GVfsBackendFtp *      backend;        /* the backend */
  char *                gvfs_path;      /* path in gvfs terms */
  char *                ftp_path;       /* path in ftp terms */
};

/*** VFUNCS (that aren't yet made into vfuncs) ***/

/* FIXME: This most likely needs adaption to non-unix like directory structures.
 * There's at least the case of multiple roots (Netware) plus probably a shitload
 * of weird old file systems (starting with MS-DOS)
 * But we first need a way to detect that.
 */

static char *
g_vfs_ftp_file_compute_ftp_path (const char *gvfs_path)
{
  return g_strdup (gvfs_path);
}

static char *
g_vfs_ftp_file_compute_gvfs_path (const char *ftp_path)
{
  return g_strdup (ftp_path);
}

/*** API ***/

/**
 * g_vfs_ftp_file_new_from_gvfs:
 * @ftp: the ftp backend this file is to be used on
 * @gvfs_path: gvfs path to create the file from
 *
 * Constructs a new #GVfsFtpFile representing the given gvfs path.
 *
 * Returns: a new file
 **/
GVfsFtpFile *
g_vfs_ftp_file_new_from_gvfs (GVfsBackendFtp *ftp, const char *gvfs_path)
{
  GVfsFtpFile *file;

  g_return_val_if_fail (G_VFS_IS_BACKEND_FTP (ftp), NULL);
  g_return_val_if_fail (gvfs_path != NULL, NULL);

  file = g_slice_new (GVfsFtpFile);
  file->backend = g_object_ref (ftp);
  file->gvfs_path = g_strdup (gvfs_path);
  file->ftp_path = g_vfs_ftp_file_compute_ftp_path (gvfs_path);

  return file;
}

/**
 * g_vfs_ftp_file_new_from_ftp:
 * @ftp: the ftp backend this file is to be used on
 * @ftp_path: ftp path to create the file from
 *
 * Constructs a new #GVfsFtpFile representing the given ftp path.
 *
 * Returns: a new file
 **/
GVfsFtpFile *
g_vfs_ftp_file_new_from_ftp (GVfsBackendFtp *ftp, const char *ftp_path)
{
  GVfsFtpFile *file;

  g_return_val_if_fail (G_VFS_IS_BACKEND_FTP (ftp), NULL);
  g_return_val_if_fail (ftp_path != NULL, NULL);

  file = g_slice_new (GVfsFtpFile);
  file->backend = g_object_ref (ftp);
  file->ftp_path = g_strdup (ftp_path);
  file->gvfs_path = g_vfs_ftp_file_compute_gvfs_path (ftp_path);

  return file;
}

/**
 * g_vfs_ftp_file_new_parent:
 * @file: file to get the parent directory from
 *
 * Creates a new file to represent the parent directory of @file. If @file
 * already references the root directory, the new file will also reference
 * the root.
 *
 * Returns: a new file representing the parent directory of @file
 **/
GVfsFtpFile *
g_vfs_ftp_file_new_parent (const GVfsFtpFile *file)
{
  char *dirname;
  GVfsFtpFile *dir;

  g_return_val_if_fail (file != NULL, NULL);

  if (g_vfs_ftp_file_is_root (file))
    return g_vfs_ftp_file_copy (file);

  dirname = g_path_get_dirname (file->gvfs_path);
  dir = g_vfs_ftp_file_new_from_gvfs (file->backend, dirname);
  g_free (dirname);

  return dir;
}

/**
 * g_vfs_ftp_file_new_child:
 * @parent: the parent file
 * @display_name: the basename to use for the new file
 * @error: location to take an eventual error or %NULL
 *
 * Tries to create a new file for the given @display_name in the given @parent
 * directory. If the display name is invalid, @error is set and %NULL is
 * returned.
 *
 * Returns: a new file or %NULL on error
 **/
GVfsFtpFile *
g_vfs_ftp_file_new_child (const GVfsFtpFile *parent, const char *display_name, GError **error)
{
  char *new_path;
  GVfsFtpFile *child;

  g_return_val_if_fail (parent != NULL, NULL);
  g_return_val_if_fail (display_name != NULL, NULL);

  if (strpbrk (display_name, "/\r\n"))
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                           _("Filename contains invalid characters."));
      return NULL;
    }

  new_path = g_strconcat (parent->gvfs_path, parent->gvfs_path[1] == 0 ? "" : "/", display_name, NULL);
  child = g_vfs_ftp_file_new_from_gvfs (parent->backend, new_path);
  g_free (new_path);
  return child;
}

/**
 * g_vfs_ftp_file_copy:
 * @file: file to copy
 *
 * Creates a copy of the given @file.
 *
 * Returns: an identical copy of @file
 **/
GVfsFtpFile *
g_vfs_ftp_file_copy (const GVfsFtpFile *file)
{
  GVfsFtpFile *copy;

  g_return_val_if_fail (file != NULL, NULL);

  copy = g_slice_new (GVfsFtpFile);
  copy->backend = g_object_ref (file->backend);
  copy->ftp_path = g_strdup (file->ftp_path);
  copy->gvfs_path = g_strdup (file->gvfs_path);

  return copy;
}

/**
 * g_vfs_ftp_file_free:
 * @file: file to free
 *
 * Frees the given file structure and all associated resources.
 **/
void
g_vfs_ftp_file_free (GVfsFtpFile *file)
{
  g_return_if_fail (file != NULL);

  g_object_unref (file->backend);
  g_free (file->gvfs_path);
  g_free (file->ftp_path);
  g_slice_free (GVfsFtpFile, file);
}

/**
 * g_vfs_ftp_file_is_root:
 * @file: the file to check
 *
 * Checks if the given file references the root directory.
 *
 * Returns: %TRUE if @file references the root directory
 **/
gboolean
g_vfs_ftp_file_is_root (const GVfsFtpFile *file)
{
  g_return_val_if_fail (file != NULL, FALSE);

  return file->gvfs_path[0] == '/' &&
         file->gvfs_path[1] == 0;
}

/**
 * g_vfs_ftp_file_get_ftp_path:
 * @file: a file
 *
 * Gets the string to refer to @file on the ftp server. This string may not be
 * valid UTF-8.
 *
 * Returns: the path to refer to @file on the FTP server.
 **/
const char *
g_vfs_ftp_file_get_ftp_path (const GVfsFtpFile *file)
{
  g_return_val_if_fail (file != NULL, NULL);

  return file->ftp_path;
}

/**
 * g_vfs_ftp_file_get_gvfs_path:
 * @file: a file
 *
 * Gets the GVfs path used to refer to @file.
 *
 * Returns: the GVfs path used to refer to @file.
 **/
const char *
g_vfs_ftp_file_get_gvfs_path (const GVfsFtpFile *file)
{
  g_return_val_if_fail (file != NULL, NULL);

  return file->gvfs_path;
}

/**
 * g_vfs_ftp_file_equal:
 * @a: a #GVfsFtpFile
 * @b: a #GVfsFtpFile
 *
 * Compares @a and @b. If they reference the same file, %TRUE is returned.
 * This function uses #gconstpointer arguments to the #GEqualFunc type.
 *
 * Returns: %TRUE if @a and @b reference the same file.
 **/
gboolean
g_vfs_ftp_file_equal (gconstpointer a,
                      gconstpointer b)
{
  const GVfsFtpFile *af = a;
  const GVfsFtpFile *bf = b;

  g_return_val_if_fail (a != NULL, FALSE);
  g_return_val_if_fail (b != NULL, FALSE);

  /* FIXME: use ftp path? */
  return g_str_equal (af->gvfs_path, bf->gvfs_path);
}

/**
 * g_vfs_ftp_file_hash:
 * @a: a #GvfsFtpFile
 *
 * Computes a hash value for the given file to be used in a #GHashTable.
 * This function uses #gconstpointer arguments to the #GHashFunc type.
 *
 * Returns: a hash value for the given file.
 **/
guint
g_vfs_ftp_file_hash (gconstpointer a)
{
  const GVfsFtpFile *af = a;

  return g_str_hash (af->gvfs_path);
}

