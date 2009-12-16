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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors: Alexander Larsson <alexl@redhat.com>
 *          Cosimo Cecchi <cosimoc@gnome.org>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>

#include "gvfsbackendcomputer.h"
#include "gvfsmonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsdaemonprotocol.h"

typedef struct {
  char *filename;
  char *display_name;
  GIcon *icon;
  GFile *root;
  int prio;
  gchar *unix_device_file;
  gboolean can_mount;
  gboolean can_unmount;
  gboolean can_eject;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
  GDriveStartStopType start_stop_type;
  
  GDrive *drive;
  GVolume *volume;
  GMount *mount;
} ComputerFile;

static ComputerFile root = { "/" };

struct _GVfsBackendComputer
{
  GVfsBackend parent_instance;

  GVolumeMonitor *volume_monitor;

  GVfsMonitor *root_monitor;
  
  GList *files;
  
  guint recompute_idle_tag;
  
  GMountSpec *mount_spec;
};

G_DEFINE_TYPE (GVfsBackendComputer, g_vfs_backend_computer, G_VFS_TYPE_BACKEND)

static void
computer_file_free (ComputerFile *file)
{
  g_free (file->unix_device_file);
  g_free (file->filename);
  g_free (file->display_name);
  if (file->icon)
    g_object_unref (file->icon);
  if (file->root)
    g_object_unref (file->root);
  
  if (file->drive)
    g_object_unref (file->drive);
  if (file->volume)
    g_object_unref (file->volume);
  if (file->mount)
    g_object_unref (file->mount);
  
  g_slice_free (ComputerFile, file);
}

/* Assumes filename equal */
static gboolean
computer_file_equal (ComputerFile *a,
                     ComputerFile *b)
{
  if (strcmp (a->display_name, b->display_name) != 0)
    return FALSE;

  if (!g_icon_equal (a->icon, b->icon))
    return FALSE;
      
  if ((a->root != NULL && b->root != NULL &&
       !g_file_equal (a->root, b->root)) ||
      (a->root != NULL && b->root == NULL) ||
      (a->root == NULL && b->root != NULL))
    return FALSE;

  if (a->prio != b->prio)
    return FALSE;

  if (a->can_mount != b->can_mount ||
      a->can_unmount != b->can_unmount ||
      a->can_eject != b->can_eject ||
      a->can_start != b->can_start ||
      a->can_start_degraded != b->can_start_degraded ||
      a->can_stop != b->can_stop ||
      a->can_poll_for_media != b->can_poll_for_media ||
      a->is_media_check_automatic != b->is_media_check_automatic ||
      a->start_stop_type != b->start_stop_type)
    return FALSE;

  return TRUE;
}

static void object_changed (GVolumeMonitor *monitor,
                            gpointer object,
                            GVfsBackendComputer *backend);

static void
g_vfs_backend_computer_finalize (GObject *object)
{
  GVfsBackendComputer *backend;

  backend = G_VFS_BACKEND_COMPUTER (object);

  if (backend->volume_monitor)
    {
      g_signal_handlers_disconnect_by_func(backend->volume_monitor, object_changed, backend);
      g_object_unref (backend->volume_monitor);
    }
  
  g_mount_spec_unref (backend->mount_spec);

  if (backend->recompute_idle_tag)
    {
      g_source_remove (backend->recompute_idle_tag);
      backend->recompute_idle_tag = 0;
    }

  g_object_unref (backend->root_monitor);
  
  if (G_OBJECT_CLASS (g_vfs_backend_computer_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_computer_parent_class)->finalize) (object);
}

static void
g_vfs_backend_computer_init (GVfsBackendComputer *computer_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (computer_backend);
  GMountSpec *mount_spec;
  
  g_vfs_backend_set_display_name (backend, _("Computer"));
  g_vfs_backend_set_icon_name (backend, "computer");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("computer");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  computer_backend->mount_spec = mount_spec;
}

static gboolean
filename_is_used (GList *files, const char *filename)
{
  ComputerFile *file;

  while (files != NULL)
    {
      file = files->data;
      
      if (file->filename == NULL)
        return FALSE;
      
      if (strcmp (file->filename, filename) == 0)
        return TRUE;

      files = files->next;
    }
  return FALSE;
}

static int
sort_file_by_filename (ComputerFile *a, ComputerFile *b)
{
  return strcmp (a->filename, b->filename);
}

static void
convert_slashes (char *str)
{
  char *s;

  while ((s = strchr (str, '/')) != NULL)
    *s = '\\';
}

static void
update_from_files (GVfsBackendComputer *backend,
                   GList *files)
{
  GList *old_files;
  GList *oldl, *newl;
  char *filename;
  ComputerFile *old, *new;
  int cmp;

  old_files = backend->files;
  backend->files = files;
  
  /* Generate change events */
  oldl = old_files;
  newl = files;
  while (oldl != NULL || newl != NULL)
    {
      if (oldl == NULL)
        {
          cmp = 1;
          new = newl->data;
          old = NULL;
        }
      else if (newl == NULL)
        {
          cmp = -1;
          new = NULL;
          old = oldl->data;
        }
      else
        {
          new = newl->data;
          old = oldl->data;
          cmp = strcmp (old->filename, new->filename);
        }
      
      if (cmp == 0)
        {
          if (!computer_file_equal (old, new))
            {
              filename = g_strconcat ("/", new->filename, NULL);
              g_vfs_monitor_emit_event (backend->root_monitor,
                                        G_FILE_MONITOR_EVENT_CHANGED,
                                        filename,
                                        NULL);
              g_free (filename);
            }
          
          oldl = oldl->next;
          newl = newl->next;
        }
      else if (cmp < 0)
        {
          filename = g_strconcat ("/", old->filename, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_DELETED,
                                    filename,
                                    NULL);
          g_free (filename);
          oldl = oldl->next;
        }
      else
        {
          filename = g_strconcat ("/", new->filename, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_CREATED,
                                    filename,
                                    NULL);
          g_free (filename);
          newl = newl->next;
        }
    }
  
  g_list_foreach (old_files, (GFunc)computer_file_free, NULL);
}

static void
recompute_files (GVfsBackendComputer *backend)
{
  GVolumeMonitor *volume_monitor;
  GList *drives, *volumes, *mounts, *l, *ll;
  GDrive *drive;
  GVolume *volume;
  GMount *mount;
  ComputerFile *file;
  GList *files;
  char *basename, *filename;
  const char *extension;
  int uniq;
  gchar *s;
  gchar *display_name;
  gchar *drive_name;

  volume_monitor = backend->volume_monitor;

  files = NULL;
  
	/* first go through all connected drives */
	drives = g_volume_monitor_get_connected_drives (volume_monitor);
	for (l = drives; l != NULL; l = l->next)
    {
      drive = l->data;

      volumes = g_drive_get_volumes (drive);
      if (volumes != NULL)
        {
          for (ll = volumes; ll != NULL; ll = ll->next)
            {
              volume = ll->data;

              file = g_slice_new0 (ComputerFile);
              file->drive = g_object_ref (drive);
              file->volume = volume; /* Takes ref */
              file->mount = g_volume_get_mount (volume);
              file->prio = -3;
              files = g_list_prepend (files, file);
            }
        }
      else
        {
          /* No volume, single drive */
          
          file = g_slice_new0 (ComputerFile);
          file->drive = g_object_ref (drive);
          file->volume = NULL;
          file->mount = NULL;
          file->prio = -3;
          
          files = g_list_prepend (files, file);
        }
      
      g_object_unref (drive);
    }
	g_list_free (drives);
  
	/* add all volumes that is not associated with a drive */
	volumes = g_volume_monitor_get_volumes (volume_monitor);
	for (l = volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      drive = g_volume_get_drive (volume);
      if (drive == NULL)
        {
          file = g_slice_new0 (ComputerFile);
          file->drive = NULL;
          file->volume = g_object_ref (volume);
          file->mount = g_volume_get_mount (volume);
          file->prio = -2;

          files = g_list_prepend (files, file);
        }
      else
        g_object_unref (drive);
      
      g_object_unref (volume);
    }
	g_list_free (volumes);

	/* add mounts that has no volume (/etc/mtab mounts, ftp, sftp,...) */
	mounts = g_volume_monitor_get_mounts (volume_monitor);
	for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      volume = g_mount_get_volume (mount);
      if (volume == NULL && !g_mount_is_shadowed (mount))
        {
          file = g_slice_new0 (ComputerFile);
          file->drive = NULL;
          file->volume = NULL;
          file->mount = g_object_ref (mount);
          file->prio = -1;

          files = g_list_prepend (files, file);
        }
      else
        g_object_unref (volume);
      
      g_object_unref (mount);
    }
	g_list_free (mounts);

  files = g_list_reverse (files);
  
  for (l = files; l != NULL; l = l->next)
    {
      file = l->data;

      if (file->mount)
        {
          if (file->drive != NULL)
            {
              drive_name = g_drive_get_name (file->drive);
              s = g_mount_get_name (file->mount);
              display_name = g_strdup_printf ("%s: %s", drive_name, s);
              g_free (s);
              g_free (drive_name);
            }
          else
            {
              display_name = g_mount_get_name (file->mount);
            }
          if (file->volume != NULL)
            file->unix_device_file = g_volume_get_identifier (file->volume,
                                                              G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
          file->icon = g_mount_get_icon (file->mount);
          file->display_name = display_name;
          file->root = g_mount_get_default_location (file->mount);
          file->can_unmount = g_mount_can_unmount (file->mount);
          file->can_eject = g_mount_can_eject (file->mount);
        }
      else if (file->volume)
        {
          if (file->drive != NULL)
            {
              drive_name = g_drive_get_name (file->drive);
              s = g_volume_get_name (file->volume);
              display_name = g_strdup_printf ("%s: %s", drive_name, s);
              g_free (s);
              g_free (drive_name);
            }
          else
            {
              display_name = g_volume_get_name (file->volume);
            }
          file->unix_device_file = g_volume_get_identifier (file->volume,
                                                            G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
          file->icon = g_volume_get_icon (file->volume);
          file->display_name = display_name;
          file->can_mount = g_volume_can_mount (file->volume);
          file->root = NULL;
          file->can_eject = g_volume_can_eject (file->volume);
        }
      else /* drive */
        {
          file->unix_device_file = g_drive_get_identifier (file->drive,
                                                            G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
          file->icon = g_drive_get_icon (file->drive);
          file->display_name = g_drive_get_name (file->drive);
          file->can_eject = g_drive_can_eject (file->drive);
          file->can_mount = ! g_drive_is_media_removable (file->drive) || ! g_drive_is_media_check_automatic (file->drive) || g_drive_has_media (file->drive);
        }

      if (file->drive)
        {
          file->can_start = g_drive_can_start (file->drive);
          file->can_start_degraded = g_drive_can_start_degraded (file->drive);
          file->can_stop = g_drive_can_stop (file->drive);
          file->can_poll_for_media = g_drive_can_poll_for_media (file->drive);
          file->is_media_check_automatic = g_drive_is_media_check_automatic (file->drive);
          file->start_stop_type = g_drive_get_start_stop_type (file->drive);
          if (file->can_start)
            file->can_mount = FALSE;
          basename = g_drive_get_name (file->drive);
          extension = ".drive";
        }
      else if (file->volume)
        {
          basename = g_volume_get_name (file->volume);
          extension = ".volume";
        }
      else /* mount */
        {
          basename = g_mount_get_name (file->mount);
          extension = ".mount";
        }

      convert_slashes (basename); /* No slashes in filenames */
      uniq = 1;
      filename = g_strconcat (basename, extension, NULL);
      while (filename_is_used (files, filename))
        {
          g_free (filename);
          filename = g_strdup_printf ("%s-%d%s",
                                      basename,
                                      uniq++,
                                      extension);
        }
      
      g_free (basename);
      file->filename = filename;
    }
  
  file = g_slice_new0 (ComputerFile);
  file->filename = g_strdup ("root.link");
  file->display_name = g_strdup (_("File System"));
  file->icon = g_themed_icon_new ("drive-harddisk");
  file->root = g_file_new_for_path ("/");
  file->prio = 0;
  
  files = g_list_prepend (files, file);

  files = g_list_sort (files, (GCompareFunc)sort_file_by_filename);

  update_from_files (backend, files);
}

static gboolean
recompute_files_in_idle (GVfsBackendComputer *backend)
{
  backend->recompute_idle_tag = 0;

  recompute_files (backend);
  
  return FALSE;
}

static void
object_changed (GVolumeMonitor *monitor,
                gpointer object,
                GVfsBackendComputer *backend)
{
  if (backend->recompute_idle_tag == 0) 
    backend->recompute_idle_tag =
      g_idle_add ((GSourceFunc)recompute_files_in_idle,
                  backend);
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendComputer *computer_backend = G_VFS_BACKEND_COMPUTER (backend);
  int i;
  char *signals[] = {
    "volume-added",
    "volume-removed",
    "volume-changed",
    "mount-added",
    "mount-removed",
    "mount-changed",
    "drive-connected",
    "drive-disconnected",
    "drive-changed",
    NULL
  };

  computer_backend->volume_monitor = g_volume_monitor_get ();

  /* TODO: connect all signals to object_changed */

  for (i = 0; signals[i] != NULL; i++)
    g_signal_connect_data (computer_backend->volume_monitor,
                           signals[i],
                           (GCallback)object_changed,
                           backend,
                           NULL, 0);

  computer_backend->root_monitor = g_vfs_monitor_new (backend);
  
  recompute_files (computer_backend);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static ComputerFile *
lookup (GVfsBackendComputer *backend,
        GVfsJob *job,
        const char *filename)
{
  GList *l;
  ComputerFile *file;

  if (*filename != '/')
    goto out;

  while (*filename == '/')
    filename++;

  if (*filename == 0)
    return &root;
  
  if (strchr (filename, '/') != NULL)
    goto out;
  
  for (l = backend->files; l != NULL; l = l->next)
    {
      file = l->data;

      if (strcmp (file->filename, filename) == 0)
        return file;
    }

 out:
  g_vfs_job_failed (job, G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    _("File doesn't exist"));
  return NULL;
}


static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_IS_DIRECTORY,
                      _("Can't open directory"));
  else if (file != NULL)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_SUPPORTED,
                      _("Can't open mountable file"));
  return TRUE;
}

static void
file_info_from_file (ComputerFile *file,
                     GFileInfo *info)
{
  char *uri;
  
  g_file_info_set_name (info, file->filename);
  g_file_info_set_display_name (info, file->display_name);

  if (file->icon)
    g_file_info_set_icon (info, file->icon);

  if (file->root)
    {
      uri = g_file_get_uri (file->root);

      g_file_info_set_attribute_string (info,
                                        G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                        uri);
      g_free (uri);
    }

  g_file_info_set_sort_order (info, file->prio);

  g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);
  if (file->unix_device_file != NULL)
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_MOUNTABLE_UNIX_DEVICE_FILE, file->unix_device_file);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, file->can_mount);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, file->can_unmount);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_EJECT, file->can_eject);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_START, file->can_start);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_START_DEGRADED, file->can_start_degraded);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_STOP, file->can_stop);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_POLL, file->can_poll_for_media);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_IS_MEDIA_CHECK_AUTOMATIC, file->is_media_check_automatic);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_MOUNTABLE_START_STOP_TYPE, file->start_stop_type);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  ComputerFile *file;
  GList *l;
  GFileInfo *info;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file != &root)
    {
      if (file != NULL)
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_NOT_DIRECTORY,
                          _("The file is not a directory"));
      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  /* Enumerate root */
  for (l = G_VFS_BACKEND_COMPUTER (backend)->files; l != NULL; l = l->next)
    {
      file = l->data;
      
      info = g_file_info_new ();
      
      file_info_from_file (file, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    {
      GIcon *icon;
      
      g_file_info_set_name (info, "/");
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_display_name (info, _("Computer"));
      icon = g_themed_icon_new ("computer");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_content_type (info, "inode/directory");

      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (file != NULL)
    {
      file_info_from_file (file, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  return TRUE;
}

static gboolean
try_create_dir_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  ComputerFile *file;
  GVfsBackendComputer *computer_backend;

  computer_backend = G_VFS_BACKEND_COMPUTER (backend);

  file = lookup (computer_backend,
                 G_VFS_JOB (job), filename);

  if (file != &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_NOT_SUPPORTED,
			_("Not supported"));
      
      return TRUE;
    }
  
  g_vfs_job_create_monitor_set_monitor (job,
                                        computer_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
mount_volume_cb (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  GVolume *volume;
  GFile *root;
  char *uri;
  
  volume = G_VOLUME (source_object);

  error = NULL;
  if (g_volume_mount_finish (volume, res, &error))
    {
      mount = g_volume_get_mount (volume);

      if (mount)
        {
          root = g_mount_get_root (mount);
          uri = g_file_get_uri (root);
          g_vfs_job_mount_mountable_set_target_uri (job,
                                                    uri,
                                                    FALSE);
          g_free (uri);
          g_object_unref (root);
          g_object_unref (mount);
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_FAILED,
                          _("Internal error: %s"), "No mount object for mounted volume");
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
mount_volume_from_drive (GDrive *drive, 
                         GVfsJob *job,
                         GMountOperation *mount_op)
{
  GList *volumes;
  GVolume *volume;

  volumes = g_drive_get_volumes (drive);
  if (volumes)
    {
      volume = G_VOLUME (volumes->data);
      g_volume_mount (volume,
                      0,
                      mount_op,
                      G_VFS_JOB (job)->cancellable,
                      mount_volume_cb,
                      job);
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Can't mount file"));
    }
  
  g_list_foreach (volumes, (GFunc)g_object_unref, NULL);
  g_list_free (volumes);
}

static void
report_no_media_error (GVfsJob *job)
{
  g_vfs_job_failed (job, G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    _("No media in the drive"));
}

typedef struct {
  GVfsJobMountMountable *job;
  GMountOperation *mount_op;
} PollForMediaData;

static void
poll_for_media_cb (GObject *source_object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  PollForMediaData *data = user_data;
  GDrive *drive;
  GError *error;
  
  drive = G_DRIVE (source_object);
  error = NULL;
  
  if (g_drive_poll_for_media_finish (drive, res, &error))
    {
      gboolean has_media;
      has_media = g_drive_has_media (drive);

      if (!has_media)
        {
          report_no_media_error (G_VFS_JOB (data->job));
        }
      else
        {
          mount_volume_from_drive (drive, G_VFS_JOB (data->job), data->mount_op);
	  g_slice_free (PollForMediaData, data);
        }
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (data->job), error);
      g_error_free (error);
    }
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
                     GVfsJobMountMountable *job,
                     const char *filename,
                     GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->volume)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-mount-op", mount_op, g_object_unref);
          g_volume_mount (file->volume,
                          0,
                          mount_op,
                          G_VFS_JOB (job)->cancellable,
                          mount_volume_cb,
                          job);
        }

      else if (file->drive)
        {
          if (!g_drive_has_media (file->drive))
            {
              if (!g_drive_can_poll_for_media (file->drive))
                  report_no_media_error (G_VFS_JOB (job));
              else
                {
                  PollForMediaData *data;

                  data = g_slice_new0 (PollForMediaData);
                  mount_op = g_mount_source_get_operation (mount_source);
                  data->job = job;
                  data->mount_op = mount_op;
                  if (!g_drive_is_media_check_automatic (file->drive))
                    g_drive_poll_for_media (file->drive,
                                            G_VFS_JOB (job)->cancellable,
                                            poll_for_media_cb,
                                            data);
                  else
                    report_no_media_error (G_VFS_JOB (job));
                }
            }
          else
            {
              mount_op = g_mount_source_get_operation (mount_source);
              mount_volume_from_drive (file->drive, G_VFS_JOB (job), mount_op);
            }
        }

      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't mount file"));
        }
    }
  
  return TRUE;
}

static void
unmount_mount_cb (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  
  mount = G_MOUNT (source_object);

  error = NULL;
  if (g_mount_unmount_with_operation_finish (mount, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static gboolean
try_unmount_mountable (GVfsBackend *backend,
		       GVfsJobUnmountMountable *job,
		       const char *filename,
		       GMountUnmountFlags flags,
                       GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->mount)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-mount-op", mount_op, g_object_unref);
          g_mount_unmount_with_operation (file->mount,
                                          flags,
                                          mount_op,
                                          G_VFS_JOB (job)->cancellable,
                                          unmount_mount_cb,
                                          job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't unmount file"));
        }
    }
  
  return TRUE;
}

static void
eject_mount_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  
  mount = G_MOUNT (source_object);

  error = NULL;
  if (g_mount_eject_with_operation_finish (mount, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
eject_volume_cb (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GVolume *volume;
  
  volume = G_VOLUME (source_object);

  error = NULL;
  if (g_volume_eject_with_operation_finish (volume, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static void
eject_drive_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GDrive *drive;
  
  drive = G_DRIVE (source_object);

  error = NULL;
  if (g_drive_eject_with_operation_finish (drive, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_eject_mountable (GVfsBackend *backend,
                     GVfsJobUnmountMountable *job,
                     const char *filename,
                     GMountUnmountFlags flags,
                     GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->mount)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-mount-op", mount_op, g_object_unref);
          g_mount_eject_with_operation (file->mount,
                                        flags,
                                        mount_op,
                                        G_VFS_JOB (job)->cancellable,
                                        eject_mount_cb,
                                        job);
        }
      else if (file->volume)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-mount-op", mount_op, g_object_unref);
          g_volume_eject_with_operation (file->volume,
                                         flags,
                                         mount_op,
                                         G_VFS_JOB (job)->cancellable,
                                         eject_volume_cb,
                                         job);
        }
      else if (file->drive)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-mount-op", mount_op, g_object_unref);
          g_drive_eject_with_operation (file->drive,
                                        flags,
                                        mount_op,
                                        G_VFS_JOB (job)->cancellable,
                                        eject_drive_cb,
                                        job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't eject file"));
        }
    }
  
  return TRUE;
}


static void
drive_start_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GVfsJobStartMountable *job = user_data;
  GError *error;
  GDrive *drive;

  drive = G_DRIVE (source_object);

  error = NULL;
  if (g_drive_start_finish (drive, res, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_start_mountable (GVfsBackend *backend,
                     GVfsJobStartMountable *job,
                     const char *filename,
                     GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_MOUNTABLE_FILE,
                        _("Not a mountable file"));
    }
  else if (file != NULL)
    {
      if (file->drive != NULL)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-start-op", mount_op, g_object_unref);
          g_drive_start (file->drive,
                         0,
                         mount_op,
                         G_VFS_JOB (job)->cancellable,
                         drive_start_cb,
                         job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't start file"));
        }
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Can't start file"));
    }
  return TRUE;
}


static void
drive_stop_cb (GObject *source_object,
               GAsyncResult *res,
               gpointer user_data)
{
  GVfsJobStopMountable *job = user_data;
  GError *error;
  GDrive *drive;

  drive = G_DRIVE (source_object);

  error = NULL;
  if (g_drive_stop_finish (drive, res, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_stop_mountable (GVfsBackend *backend,
                    GVfsJobStopMountable *job,
                    const char *filename,
                    GMountUnmountFlags flags,
                    GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_MOUNTABLE_FILE,
                        _("Not a mountable file"));
    }
  else if (file != NULL)
    {
      if (file->drive != NULL)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          /* free mount_op when job is completed */
          g_object_set_data_full (G_OBJECT (job), "gvfs-backend-computer-start-op", mount_op, g_object_unref);
          g_drive_stop (file->drive,
                        flags,
                        mount_op,
                        G_VFS_JOB (job)->cancellable,
                        drive_stop_cb,
                        job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't stop file"));
        }
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Can't stop file"));
    }
  return TRUE;
}

static void
drive_poll_for_media_cb (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  GVfsJobPollMountable *job = user_data;
  GError *error;
  GDrive *drive;

  drive = G_DRIVE (source_object);

  error = NULL;
  if (g_drive_poll_for_media_finish (drive, res, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_poll_mountable (GVfsBackend *backend,
                    GVfsJobPollMountable *job,
                    const char *filename)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_MOUNTABLE_FILE,
                        _("Not a mountable file"));
    }
  else if (file != NULL)
    {
      if (file->drive != NULL)
        {
          g_drive_poll_for_media (file->drive,
                                  G_VFS_JOB (job)->cancellable,
                                  drive_poll_for_media_cb,
                                  job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't poll file"));
        }
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Can't poll file"));
    }
  return TRUE;
}

static void
g_vfs_backend_computer_class_init (GVfsBackendComputerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_computer_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_dir_monitor;
  backend_class->try_mount_mountable = try_mount_mountable;
  backend_class->try_unmount_mountable = try_unmount_mountable;
  backend_class->try_eject_mountable = try_eject_mountable;
  backend_class->try_start_mountable = try_start_mountable;
  backend_class->try_stop_mountable = try_stop_mountable;
  backend_class->try_poll_mountable = try_poll_mountable;
}
