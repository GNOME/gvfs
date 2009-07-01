/* GIO - GLib Input, Output and Streaming Library
 *   Local testing backend wrapping error injection 
 * 
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */


/***  USAGE:
 * 
 *  - behaviour is controlled via environment variable (i.e. set from the shell, before launching /usr/libexec/gvfsd)
 *    GVFS_ERRORNEOUS: number, how often operation should fail (a random() is used, this number is not a sequence) 
 *    GVFS_ERRORNEOUS_OPS: bitmask of operations to fail - see GVfsJobType enum
 * 
 ***/



#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendlocaltest.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsmonitor.h"


/* TODO:
 *  Bugs: 
 *   - unmounting ?
 *   - copy dialog still visible after try_copy() finished successfully  --> temporarily disabled do_copy ()
 * 
 * 
 *  Conceptual:
 *   - closing directory/file monitor - after a Nautilus window is closed, no method is called to destroy the monitor object
 *   - find a better way how to pass parameters to a running instance of gvfsd-localtest
 * 
 */





/************************************************
 * 	  Error injection
 * 
 */


static gboolean     
inject_error (GVfsBackend *backend,
			  GVfsJob *job,
			  GVfsJobType job_type)
{
  GVfsBackendLocalTest *op_backend = G_VFS_BACKEND_LOCALTEST (backend);

  if ((op_backend->errorneous > 0) && ((random() % op_backend->errorneous) == 0) && 
	  ((op_backend->inject_op_types < 1) || ((op_backend->inject_op_types & job_type) == job_type)))
  {
	  g_print ("(II) inject_error: BANG! injecting error... \n");
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_FAILED,
			"Injected error");
      return FALSE;
  } else g_vfs_job_succeeded (job);
  return TRUE;
}





/************************************************
 * 	  Utility functions
 * 
 */

/*  returned object should be freed in user's function  */
static GFile*
get_g_file_from_local (const char *filename, GVfsJob *job)
{
	GVfs *local_vfs;
	GFile *file = NULL;
	  
	local_vfs = g_vfs_get_local ();
	if (! local_vfs) {
		g_vfs_job_failed (job,	G_IO_ERROR, G_IO_ERROR_FAILED,
				"Cannot get local vfs"); 
	    g_print (" (EE) get_g_file_from_local (filename = '%s'): local_vfs == NULL \n", filename);
	    return NULL;
	}

	file = g_vfs_get_file_for_path (local_vfs, filename);
	if (! file) {
		g_vfs_job_failed (job,	G_IO_ERROR, G_IO_ERROR_FAILED,
				"Cannot get file from local vfs"); 
	    g_print (" (EE) get_g_file_from_local (filename = '%s'): file == NULL \n", filename);
		return NULL;
	}
	return file;
}

/*  returned object should be freed in user's function  */
static GFileInfo*
get_g_file_info_from_local (const char *filename, GFile *file, 
		const char *attributes, GFileQueryInfoFlags flags, 
		GVfsJob *job)
{
	GError *error;
	GFileInfo *info = NULL;

	g_return_val_if_fail (file != NULL, NULL);
	if (file) {
		error = NULL;
		info = g_file_query_info (file, attributes, flags, G_VFS_JOB (job)->cancellable, &error);
		
		if ((error) || (! info) ) {
		    g_print (" (EE) get_g_file_info_from_local (filename = '%s'): g_file_query_info failed: %s \n", filename, error->message);
		    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
		    return NULL;
		}
	}
	return info;
}







/************************************************
 * 	  Initialization
 * 
 */

G_DEFINE_TYPE (GVfsBackendLocalTest, g_vfs_backend_localtest, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_localtest_init (GVfsBackendLocalTest *backend)
{
	const char *c;

	/*  Nothing in there */
	g_print ("(II) g_vfs_backend_localtest_init \n");

	/*  env var conversion */
	backend->errorneous = -1;
	backend->inject_op_types = -1;
	
	c = g_getenv("GVFS_ERRORNEOUS");
	if (c) {
		backend->errorneous = g_ascii_strtoll(c, NULL, 0);
		g_print ("(II) g_vfs_backend_localtest_init: setting 'errorneous' to '%d' \n", backend->errorneous);
	}

	c = g_getenv("GVFS_ERRORNEOUS_OPS");
	if (c) {
		backend->inject_op_types = g_ascii_strtoll(c, NULL, 0);
		g_print ("(II) g_vfs_backend_localtest_init: setting 'inject_op_types' to '%lu' \n", (unsigned long)backend->inject_op_types);
	}
	
	g_print ("(II) g_vfs_backend_localtest_init done.\n");
}

static void
g_vfs_backend_localtest_finalize (GObject *object)
{
	GVfsBackendLocalTest *backend;

	g_print ("(II) g_vfs_backend_localtest_finalize \n");

	backend = G_VFS_BACKEND_LOCALTEST (object);

    if (backend->test)
    	g_free ((gpointer)backend->test);  
  
	if (G_OBJECT_CLASS (g_vfs_backend_localtest_parent_class)->finalize)
      (*G_OBJECT_CLASS (g_vfs_backend_localtest_parent_class)->finalize) (object);
}







/************************************************
 * 	  Mount
 * 
 */

static void
do_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendLocalTest *op_backend = G_VFS_BACKEND_LOCALTEST (backend);

  g_print ("(II) try_mount \n");

  g_vfs_backend_set_display_name (backend, "localtest");

  op_backend->mount_spec = g_mount_spec_new ("localtest");
  g_vfs_backend_set_mount_spec (backend, op_backend->mount_spec);

  g_vfs_backend_set_icon_name (backend, "folder-remote");

  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_MOUNT);
}


static void
do_unmount (GVfsBackend *backend, GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendLocalTest *op_backend;

  g_print ("(II) try_umount \n");

  op_backend = G_VFS_BACKEND_LOCALTEST (backend);
  g_mount_spec_unref (op_backend->mount_spec);
  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_UNMOUNT);
}







/************************************************
 * 	  Queries
 * 
 */

static void
do_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GFile *file;
  GFileInfo *info;
  GError *error;
  GFileEnumerator *enumerator;
  gboolean res;
  
  g_print ("(II) try_enumerate (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);
  res = TRUE;

  error = NULL;
  enumerator = g_file_enumerate_children (file, "*", flags, G_VFS_JOB (job)->cancellable, &error);
  if (enumerator) {
	  error = NULL;
	  while ((info = g_file_enumerator_next_file (enumerator, G_VFS_JOB (job)->cancellable, &error)) != NULL) {
    	  g_print ("  (II) try_enumerate (filename = %s): file '%s' \n", filename, g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME));
    	  g_vfs_job_enumerate_add_info (job, info);
      }
	  if (error) {
		  g_print ("  (EE) try_enumerate: error: %s \n", error->message);
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
		  g_error_free (error);
		  res = FALSE;
	  } else 
		  res = inject_error (backend, G_VFS_JOB (job), GVFS_JOB_ENUMERATE);
	  
	  error = NULL;
      g_file_enumerator_close (enumerator, G_VFS_JOB (job)->cancellable, &error);
	  if (error) {
		  g_print ("  (EE) try_enumerate: g_file_enumerator_close() error: %s \n", error->message);
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
		  g_error_free (error);
		  res = FALSE;
	  } 
      g_object_unref (enumerator);
      g_print ("  (II) try_enumerate: success. \n");
  } else {
	  if (error) {
		  g_print ("  (EE) try_enumerate: error: %s \n", error->message);
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
		  g_error_free (error);
		  res = FALSE;
	  } else { 
		  g_print ("  (EE) try_enumerate: error == NULL \n");
		  g_vfs_job_failed (G_VFS_JOB (job),
					G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					"Error try_enumerate");
		  res = FALSE;
	  }
  }

  g_object_unref (file);
  if (res) {
	  g_vfs_job_enumerate_done (job);  
  }
  
  g_print ("(II) try_enumerate done. \n");
}


static void
do_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GFile *file;
  GFileInfo *info2;

  g_print ("(II) try_query_info (filename = %s) \n", filename);
  
  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  info2 = get_g_file_info_from_local (filename, file, "*", flags, G_VFS_JOB (job));

  if (info2) {
      g_file_info_copy_into (info2, info);
      g_object_unref (info2);
      g_object_unref (file);
      inject_error (backend, G_VFS_JOB (job), GVFS_JOB_QUERY_INFO);
      g_print ("(II) try_query_info success. \n");
  } else
	  g_print ("(EE) try_query_info failed. \n");
}


static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  GFile *file;
  GFileInfo *info2;
  GError *error;

  g_print ("(II) try_query_fs_info (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));

  if (file) {
	  error = NULL;  
	  info2 = g_file_query_filesystem_info (file, "fs:*", G_VFS_JOB (job)->cancellable, &error);
	  if ((error) || (! info2) ) {
		  g_print ("  (EE) try_query_fs_info (filename = '%s'): g_file_query_filesystem_info failed: %s \n", filename, error->message);
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
		  g_error_free (error);
	  } else {
	      g_file_info_copy_into (info2, info);
	      g_object_unref (info2);
	      g_object_unref (file);
	      inject_error (backend, G_VFS_JOB (job), GVFS_JOB_QUERY_FS_INFO);
	      g_print ("(II) try_query_fs_info success. \n");
	  }
  } else {
	  g_print ("(EE) try_query_fs_info failed. \n");
  }
}


static void
do_query_settable_attributes (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename)
{
  GFileAttributeInfoList *attr_list;
  GError *error;
  GFile *file;

  g_print ("(II) try_query_settable_attributes (filename = '%s') \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  attr_list = g_file_query_settable_attributes (file, G_VFS_JOB (job)->cancellable, &error);
	  if ((attr_list) && (! error)) {
		  g_vfs_job_query_attributes_set_list (job, attr_list);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_QUERY_SETTABLE_ATTRIBUTES);
		  g_print ("(II) try_query_settable_attributes success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_query_settable_attributes: g_file_query_settable_attributes == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_query_settable_attributes: file == NULL \n");
  }
}

static void
do_query_writable_namespaces (GVfsBackend *backend,
					     GVfsJobQueryAttributes *job,
					     const char *filename)
{
  GFileAttributeInfoList *attr_list;
  GError *error;
  GFile *file;

  g_print ("(II) try_query_writable_namespaces (filename = '%s') \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  attr_list = g_file_query_writable_namespaces (file, G_VFS_JOB (job)->cancellable, &error);
	  if ((attr_list) && (! error)) {
		  g_vfs_job_query_attributes_set_list (job, attr_list);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_QUERY_WRITABLE_NAMESPACES);
		  g_print ("(II) try_query_writable_namespaces success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_query_writable_namespaces: g_file_query_writable_namespaces == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_query_writable_namespaces: file == NULL \n");
  }
}





/************************************************
 * 	  Operations
 * 
 */

static void
do_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_make_directory (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_make_directory (file, G_VFS_JOB (job)->cancellable, &error)) {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_MAKE_DIRECTORY);
		  g_print ("(II) try_make_directory success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_make_directory: g_file_make_directory == FALSE \n");
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_make_directory: file == NULL \n");
  }
}


static void
do_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_delete (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_delete (file, G_VFS_JOB (job)->cancellable, &error)) {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_DELETE);
		  g_print ("(II) try_delete success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_delete: g_file_delete == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_delete: file == NULL \n");
  }
}


static void
do_trash (GVfsBackend *backend,
			GVfsJobTrash *job,
            const char *filename)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_trash (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_trash (file, G_VFS_JOB (job)->cancellable, &error)) {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_TRASH);
		  g_print ("(II) try_trash success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_trash: g_file_trash == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_trash: file == NULL \n");
  }
}


static void
do_make_symlink (GVfsBackend *backend,
                  GVfsJobMakeSymlink *job,
                  const char *filename,
                  const char *symlink_value)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_make_symlink ('%s' --> '%s') \n", filename, symlink_value);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_make_symbolic_link (file, symlink_value, G_VFS_JOB (job)->cancellable, &error)) {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_MAKE_SYMLINK);
		  g_print ("(II) try_make_symlink success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_make_symlink: g_file_make_symbolic_link == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_make_symlink: file == NULL \n");
  }
}

#if 0
static void
do_copy (GVfsBackend *backend,
 		 GVfsJobCopy *job,
		 const char *source,
		 const char *destination,
		 GFileCopyFlags flags,
		 GFileProgressCallback progress_callback,
		 gpointer progress_callback_data)
{
  GFile *src_file, *dst_file;
  GError *error;
  
  g_print ("(II) try_copy '%s' --> '%s' \n", source, destination);
	  
  src_file = get_g_file_from_local (source, G_VFS_JOB (job));
  dst_file = get_g_file_from_local (destination, G_VFS_JOB (job));
  g_assert (src_file != NULL);

  if (src_file) {
	  error = NULL;
	  if (g_file_copy (src_file, dst_file, flags, G_VFS_JOB (job)->cancellable, 
			  progress_callback, progress_callback_data, &error)) 
	  {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_COPY);
		  g_print ("  (II) try_copy: success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_copy: g_file_copy == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (src_file);
	  g_object_unref (dst_file);
  } else { 
	  g_print ("  (EE) try_copy: file == NULL \n");
  }
}
#endif

static void
do_move (GVfsBackend *backend,
          GVfsJobMove *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GFile *src_file, *dst_file;
  GError *error;
  
  g_print ("(II) try_move '%s' --> '%s' \n", source, destination);
	  
  src_file = get_g_file_from_local (source, G_VFS_JOB (job));
  dst_file = get_g_file_from_local (destination, G_VFS_JOB (job));
  g_assert (src_file != NULL);

  if (src_file) {
	  error = NULL;
	  if (g_file_move (src_file, dst_file, flags, G_VFS_JOB (job)->cancellable, 
			  progress_callback, progress_callback_data, &error)) 
	  {
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_MOVE);
		  g_print ("  (II) try_move: success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_move: g_file_move == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (src_file);
	  g_object_unref (dst_file);
  } else { 
	  g_print ("  (EE) try_move: file == NULL \n");
  }
}


//  aka 'rename'
static void
do_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_set_display_name '%s' --> '%s' \n", filename, display_name);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_set_display_name (file, display_name, G_VFS_JOB (job)->cancellable, &error)) {
		  char *dirname, *new_path;
		  dirname = g_path_get_dirname (filename);
		  new_path = g_build_filename (dirname, display_name, NULL);
		  g_print ("(II) try_set_display_name: filename = '%s'... \n", filename);
		  g_print ("(II) try_set_display_name: display_name = '%s'... \n", display_name);
		  g_print ("(II) try_set_display_name: dirname = '%s'... \n", dirname);
		  g_print ("(II) try_set_display_name: new_path = '%s'... \n", new_path);
	      g_vfs_job_set_display_name_set_new_path (job, new_path);
		  g_free (dirname);
		  g_free (new_path);

		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_SET_DISPLAY_NAME);
		  g_print ("(II) try_set_display_name success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_set_display_name: g_file_set_display_name == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_set_display_name: file == NULL \n");
  }
}


static void
do_set_attribute (GVfsBackend *backend,
		 GVfsJobSetAttribute *set_attribute,
		 const char *filename,
		 const char *attribute,
		 GFileAttributeType type,
		 gpointer value_p,
		 GFileQueryInfoFlags flags)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_set_attribute (filename = '%s', attribute = '%s') \n", filename, attribute);

  file = get_g_file_from_local (filename, G_VFS_JOB (set_attribute));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  if (g_file_set_attribute (file, attribute, type,  value_p, flags, G_VFS_JOB (set_attribute)->cancellable, &error)) {
		  inject_error (backend, G_VFS_JOB (set_attribute), GVFS_JOB_SET_ATTRIBUTE);
		  g_print ("(II) try_set_attribute success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (set_attribute), error); 
		  g_print ("  (EE) try_set_attribute: g_file_set_attribute == FALSE, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_set_attribute: file == NULL \n");
  }
}







/************************************************
 * 	  Monitors
 * 
 */

/*  MonitorProxy has been stolen from gvfsbackendtrash  */
typedef struct {
  GVfsMonitor *vfs_monitor;
  GObject *monitor;
} MonitorProxy;

static void
monitor_proxy_free (MonitorProxy *proxy)
{
  g_print ("(II) monitor_proxy_free \n");
  g_object_unref (proxy->monitor);
  g_free (proxy);
}

static void
proxy_changed (GFileMonitor* monitor,
               GFile* file,
               GFile* other_file,
               GFileMonitorEvent event_type,
               MonitorProxy *proxy)
{
  char *file_path;
  char *other_file_path;

  file_path = g_file_get_path (file);
  g_print ("(II) monitor_proxy_changed: file_path = '%s' \n", file_path);

  if (other_file)
    {
      other_file_path = g_file_get_path (other_file);
      g_print ("(II) monitor_proxy_changed: other_file_path == '%s' \n", other_file_path);
    }
  else
    {
      other_file_path = NULL;
    }
  
  g_vfs_monitor_emit_event (proxy->vfs_monitor,
                            event_type,
                            file_path,
                            other_file_path);

  g_free (file_path);
  g_free (other_file_path);
}


static void
create_dir_file_monitor (GVfsBackend *backend,
				      GVfsJobCreateMonitor *job,
				      const char *filename,
				      GFileMonitorFlags flags,
				      const gboolean is_dir_monitor)
{
  GObject *monitor;
  MonitorProxy *proxy;
  GFile *file;
  
  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);
  
  if (is_dir_monitor) { 
	  monitor = G_OBJECT (g_file_monitor_directory (file, flags, G_VFS_JOB (job)->cancellable, NULL));
  } else {
	  monitor = G_OBJECT (g_file_monitor_file (file, flags, G_VFS_JOB (job)->cancellable, NULL));
  }
	  
  if (monitor) {
      proxy = g_new0 (MonitorProxy, 1); 
      proxy->vfs_monitor = g_vfs_monitor_new (backend);
      proxy->monitor = monitor;
      
      g_object_set_data_full (G_OBJECT (proxy->vfs_monitor), "monitor-proxy",
			      proxy, (GDestroyNotify) monitor_proxy_free);  //* hmm?
      g_signal_connect (monitor, "changed", G_CALLBACK (proxy_changed), proxy);

      g_vfs_job_create_monitor_set_monitor (job, proxy->vfs_monitor);
      
      g_object_unref (proxy->vfs_monitor);
      
      inject_error (backend, G_VFS_JOB (job), GVFS_JOB_CREATE_DIR_MONITOR);
      g_print ("(II) create_dir_file_monitor success. \n");
    }
  else  {
      g_print ("  (EE) create_dir_file_monitor: monitor == NULL \n");
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Directory notification not supported"));
    }
  g_object_unref (file);
}


static void
do_create_dir_monitor (GVfsBackend *backend,
				      GVfsJobCreateMonitor *job,
				      const char *filename,
				      GFileMonitorFlags flags)
{
  g_print ("(II) try_create_dir_monitor (filename = '%s') \n", filename);
  create_dir_file_monitor (backend, job, filename, flags, TRUE);
}


static void
do_create_file_monitor (GVfsBackend *backend,
				      GVfsJobCreateMonitor *job,
				      const char *filename,
				      GFileMonitorFlags flags)
{
  g_print ("(II) try_create_file_monitor (filename = '%s') \n", filename);
  create_dir_file_monitor (backend, job, filename, flags, FALSE);
}






/************************************************
 * 	  Read/write/create/close data operations
 * 
 */

static void
do_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GFileInputStream *stream;
  GError *error;
  GFile *file;

  g_print ("(II) try_open_for_read (filename = '%s') \n", filename);
  
  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  stream = g_file_read (file, G_VFS_JOB (job)->cancellable, &error);
	  if (stream) {
		  g_vfs_job_open_for_read_set_can_seek (job, g_seekable_can_seek (G_SEEKABLE (stream)));
		  g_vfs_job_open_for_read_set_handle (job, stream);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_OPEN_FOR_READ);
		  g_print ("(II) try_open_for_read success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_open_for_read: stream == NULL, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_open_for_read: file == NULL \n");
  }
}


static void
do_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle _handle,
          char *buffer,
          gsize bytes_requested)
{
  GError *error;
  GFileInputStream *stream = _handle;
  gssize s;

  g_print ("(II) try_read (handle = '%lx', buffer = '%lx', bytes_requested = %ld) \n", 
		  (long int)_handle, (long int)buffer, (long int)bytes_requested);

  g_assert (stream != NULL);
  
  error = NULL;
  s = g_input_stream_read (G_INPUT_STREAM (stream), buffer, bytes_requested, 
		  				   G_VFS_JOB (job)->cancellable, &error); 
  if (s >= 0) {
      g_vfs_job_read_set_size (job, s);
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_READ);
	  g_print ("(II) try_read success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_read: g_input_stream_read() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}


static void
do_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle _handle,
                  goffset    offset,
                  GSeekType  type)
{
  GError *error;
  GFileInputStream *stream = _handle;

  g_print ("(II) try_seek_on_read (handle = '%lx', offset = %ld) \n", (long int)_handle, (long int)offset);

  g_assert (stream != NULL);
  
  error = NULL;
  if (g_seekable_seek (G_SEEKABLE (stream), offset, type, G_VFS_JOB (job)->cancellable, &error)) {
	  g_vfs_job_seek_read_set_offset (job, g_seekable_tell (G_SEEKABLE (stream)));
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_SEEK_ON_READ);
	  g_print ("(II) try_seek_on_read success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_seek_on_read: g_file_input_stream_seek() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}


static void
do_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle _handle)
{
  GError *error;
  GFileInputStream *stream = _handle;

  g_print ("(II) try_close_read (handle = '%lx') \n", (long int)_handle);

  g_assert (stream != NULL);
  
  error = NULL;
  if (g_input_stream_close (G_INPUT_STREAM (stream), G_VFS_JOB (job)->cancellable, &error)) {
	  g_object_unref (stream);
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_CLOSE_READ);
	  g_print ("(II) try_close_read success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_close_read: g_input_stream_close() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}


static void
do_append_to (GVfsBackend *backend,
               GVfsJobOpenForWrite *job,
               const char *filename,
               GFileCreateFlags flags)
{
  GFileOutputStream *stream;
  GError *error;
  GFile *file;

  g_print ("(II) try_append_to (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  stream = g_file_append_to (file, flags, G_VFS_JOB (job)->cancellable, &error);
	  if (stream) {
		  /*  Should seek at the end of the file here  */
		  if ((g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_END, G_VFS_JOB (job)->cancellable, &error)) && (! error)) {
			  g_vfs_job_open_for_write_set_initial_offset (job, g_seekable_tell (G_SEEKABLE (stream)));
		  } else {
		  	  g_print ("  (EE) try_append_to: error during g_file_output_stream_seek(), error: %s \n", error->message);
		  }

		  g_vfs_job_open_for_write_set_can_seek (job, g_seekable_can_seek (G_SEEKABLE (stream)));
		  g_vfs_job_open_for_write_set_handle (job, stream);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_APPEND_TO);

		  g_print ("(II) try_append_to success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_append_to: stream == NULL, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_append_to: file == NULL \n");
  }
}


static void
do_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GFileOutputStream *stream;
  GError *error;
  GFile *file;

  g_print ("(II) try_create (filename = %s) \n", filename);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  stream = g_file_create (file, flags, G_VFS_JOB (job)->cancellable, &error);
	  if (stream) {
		  g_vfs_job_open_for_write_set_can_seek (job, g_seekable_can_seek (G_SEEKABLE (stream)));
		  g_vfs_job_open_for_write_set_handle (job, stream);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_CREATE);
		  g_print ("(II) try_create success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_create: stream == NULL, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_create: file == NULL \n");
  }
}


static void
do_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GFileOutputStream *stream;
  GError *error;
  GFile *file;

  g_print ("(II) try_replace (filename = '%s', etag = '%s') \n", filename, etag);

  file = get_g_file_from_local (filename, G_VFS_JOB (job));
  g_assert (file != NULL);

  if (file) {
	  error = NULL;
	  stream = g_file_replace (file, etag, make_backup, flags, G_VFS_JOB (job)->cancellable, &error);
	  if (stream) {
		  g_vfs_job_open_for_write_set_can_seek (job, g_seekable_can_seek (G_SEEKABLE (stream)));
		  g_vfs_job_open_for_write_set_handle (job, stream);
		  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_REPLACE);
		  g_print ("(II) try_replace success. \n");
	  } else {
		  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
		  g_print ("  (EE) try_replace: stream == NULL, error: %s \n", error->message);
		  g_error_free (error);
	  }
	  g_object_unref (file);
  } else {
	  g_print ("  (EE) try_replace: file == NULL \n");
  }
}


static void
do_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle _handle,
           char *buffer,
           gsize buffer_size)
{
  GError *error;
  GFileOutputStream *stream = _handle;
  gssize s;

  g_print ("(II) try_write (handle = '%lx', buffer = '%lx', buffer_size = %ld) \n", 
		  (long int)_handle, (long int)buffer, (long int)buffer_size);

  g_assert (stream != NULL);
  
  error = NULL;
  s = g_output_stream_write (G_OUTPUT_STREAM (stream), buffer, buffer_size, G_VFS_JOB (job)->cancellable, &error); 
  if (s >= 0) {
	  g_vfs_job_write_set_written_size (job, s);
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_WRITE);
	  g_print ("(II) try_write success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_write: g_output_stream_write() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}


static void
do_seek_on_write (GVfsBackend *backend,
                   GVfsJobSeekWrite *job,
                   GVfsBackendHandle _handle,
                   goffset    offset,
                   GSeekType  type)
{
  GError *error;
  GFileOutputStream *stream = _handle;

  g_print ("(II) try_seek_on_write (handle = '%lx', offset = %ld) \n", (long int)_handle, (long int)offset);

  g_assert (stream != NULL);
  
  error = NULL;
  if (g_seekable_seek (G_SEEKABLE (stream), offset, type, G_VFS_JOB (job)->cancellable, &error)) {
	  g_vfs_job_seek_write_set_offset (job, g_seekable_tell (G_SEEKABLE (stream)));
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_SEEK_ON_WRITE);
	  g_print ("(II) try_seek_on_write success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_seek_on_write: g_file_output_stream_seek() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}


static void
do_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle _handle)
{
  GError *error;
  GFileOutputStream *stream = _handle;

  g_print ("(II) try_close_write (handle = '%lx') \n", (long int)_handle);

  g_assert (stream != NULL);
  
  error = NULL;
  if (g_output_stream_close (G_OUTPUT_STREAM(stream), G_VFS_JOB (job)->cancellable, &error)) {
	  g_object_unref (stream);
	  inject_error (backend, G_VFS_JOB (job), GVFS_JOB_CLOSE_WRITE);
	  g_print ("(II) try_close_write success. \n");
  } else  {
	  g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
	  g_print ("  (EE) try_close_write: g_input_stream_close() failed, error: %s \n", error->message);
	  g_error_free (error);
  }
}








/************************************************
 * 	  Class init
 * 
 */


static void
g_vfs_backend_localtest_class_init (GVfsBackendLocalTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_localtest_finalize;

  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->close_write = do_close_write;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
/*  -- disabled, read/write operations can handle copy correctly  */  
/*   backend_class->copy = do_copy; */ 
  backend_class->move = do_move;
  backend_class->make_symlink = do_make_symlink;
  backend_class->make_directory = do_make_directory;
  backend_class->delete = do_delete;
  backend_class->trash = do_trash;
  backend_class->set_display_name = do_set_display_name;
  backend_class->set_attribute = do_set_attribute;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
  backend_class->query_settable_attributes = do_query_settable_attributes;
  backend_class->query_writable_namespaces = do_query_writable_namespaces;
}
