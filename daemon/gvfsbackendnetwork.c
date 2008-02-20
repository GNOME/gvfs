/* GIO - GLib Input, Output and Streaming Library
 * Original work, Copyright (C) 2003 Red Hat, Inc
 * GVFS port, Copyright (c) 2008 Andrew Walton.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Andrew Walton <awalton@svn.gnome.org> (port only)
 */

/* 
  TODO: 
  Add DNS-SD when there's an appropriate backend.
  Add file/directory monitor events (only really necessary after we add DNS-SD).
 */

#include <config.h>

#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gurifuncs.h>
#include <gio/gio.h>
#include <gconf/gconf-client.h>

#include "gvfsbackendnetwork.h"

#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsmonitor.h"

#define PATH_GCONF_GVFS_SMB "/system/smb"
#define PATH_GCONF_GVFS_SMB_WORKGROUP "/system/smb/workgroup"
#define DEFAULT_WORKGROUP_NAME "X-GNOME-DEFAULT-WORKGROUP"

typedef struct {
  char *filename; 
  char *display_name;
  char *target_uri;
  GFile *root;
  GIcon *icon;
} NetworkFile;

static NetworkFile root = { "/" };

struct _GVfsBackendNetwork
{
  GVfsBackend parent_instance;
  GVfsMonitor *root_monitor;
  GMountSpec *mount_spec;

  GList *files; /* list of NetworkFiles */

  /* SMB Stuff */
  gboolean have_smb;
  char *current_workgroup; 
};

typedef struct _GVfsBackendNetwork GVfsBackendNetwork;

G_DEFINE_TYPE (GVfsBackendNetwork, g_vfs_backend_network, G_VFS_TYPE_BACKEND);

static NetworkFile *
network_file_new (const char *filename, 
                  const char *display_name, 
                  const char *target_uri, 
                  const char *icon)
{
  NetworkFile *file;
  
  file = g_slice_new (NetworkFile);

  file->filename = g_strdup (filename);
  file->display_name = g_strdup (display_name);
  file->target_uri = g_strdup (target_uri);
  file->icon = g_themed_icon_new (icon);
  file->root = g_file_new_for_path ("/");

  return file;
}

static void
network_file_free (NetworkFile *file)
{
  g_free (file->filename);
  g_free (file->display_name);
  g_free (file->target_uri);
 
  if (file->icon)
    g_object_unref (file->icon);

  if (file->root)
    g_object_unref (file->root);

  g_slice_free (NetworkFile, file);
}

static int
sort_file_by_filename (NetworkFile *a, NetworkFile *b)
{
  return strcmp (a->filename, b->filename);
}

static void
recompute_files (GVfsBackendNetwork *backend)
{
  NetworkFile *file;
  char *workgroup;
  char *workgroup_name;

  g_list_foreach (backend->files, (GFunc)network_file_free, NULL);

  if (backend->have_smb) 
    {
      file = network_file_new ("smblink-root", _("Windows Network"), 
			       "smb:///", "network-workgroup");
      backend->files = g_list_prepend (backend->files, file);

      if (backend->current_workgroup == NULL ||
	  backend->current_workgroup[0] == 0)
        {
          workgroup = g_strdup_printf("smb://%s/", DEFAULT_WORKGROUP_NAME);
          workgroup_name = g_strdup (_("Default Workgroup"));
        }
      else
	{
          char *tmp_workgroup = g_strdup_printf("smb://%s/", backend->current_workgroup);
          workgroup = g_uri_escape_string (tmp_workgroup, NULL, FALSE);
          g_free(tmp_workgroup);
          workgroup_name = g_strdup (backend->current_workgroup);
        }
      
      file = network_file_new ("smblink-workgroup", workgroup_name, 
                               workgroup, "network-workgroup");
      
      backend->files = g_list_prepend (backend->files, file);
      g_free(workgroup);
      g_free(workgroup_name);
    }
  
  backend->files = g_list_sort (backend->files, 
                                (GCompareFunc)sort_file_by_filename);
}

static void
notify_gconf_value_changed (GConfClient *client,
                            guint        cnxn_id,
                            GConfEntry  *entry,
                            gpointer     data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(data);

  char *current_workgroup;

  current_workgroup = gconf_client_get_string (client,
					       PATH_GCONF_GVFS_SMB_WORKGROUP, NULL);

  g_free(backend->current_workgroup);
  backend->current_workgroup = current_workgroup;
  
  recompute_files (backend);
  g_vfs_monitor_emit_event (backend->root_monitor, G_FILE_MONITOR_EVENT_CHANGED, 
                            "/smblink-workgroup", NULL);
}

static NetworkFile *
lookup_network_file (GVfsBackendNetwork *backend,
                     GVfsJob *job,
                     const char *filename)
{
  GList *l;
  NetworkFile *file;

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


static void
file_info_from_file (NetworkFile *file,
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
      g_file_info_set_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, uri);
      g_free (uri);
    }

  g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
  g_file_info_set_size(info, 0);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  /* little bit of stuff here to make looking up network links faster for those who 
   * who know about the STANDARD_TARGET_URI attrib. */
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, 
                                    file->target_uri);
}

/* Backend Functions */
static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  NetworkFile *file;
  GList *l;
  GFileInfo *info;

  file = lookup_network_file (G_VFS_BACKEND_NETWORK (backend),
			      G_VFS_JOB (job), filename);
  
  if (file != &root)
    {
      if (file != NULL)
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_NOT_DIRECTORY,
                          _("The file is not a directory"));
      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB(job));
  
  /* Enumerate root */
  for (l = G_VFS_BACKEND_NETWORK (backend)->files; l != NULL; l = l->next)
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
  NetworkFile *file;

  file = lookup_network_file (G_VFS_BACKEND_NETWORK (backend), 
                              G_VFS_JOB (job), filename);

  if (file == &root)
    {
      GIcon *icon;
      g_file_info_set_name (info, "/");
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_display_name (info, _("Network"));
      icon = g_themed_icon_new ("network-workgroup");
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
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendNetwork *network_backend = G_VFS_BACKEND_NETWORK (backend);
  network_backend->root_monitor = g_vfs_monitor_new (backend);
  recompute_files (network_backend);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

/* handles both file and dir monitors, 
 * as we really don't "support" (e.g. fire events for) either, yet. */
static gboolean
try_create_monitor (GVfsBackend *backend,
                    GVfsJobCreateMonitor *job,
                    const char *filename,
                    GFileMonitorFlags flags)
{
  NetworkFile *file;
  GVfsBackendNetwork *network_backend;

  network_backend = G_VFS_BACKEND_NETWORK (backend);

  file = lookup_network_file (network_backend, G_VFS_JOB (job), filename);

  if (file != &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_NOT_SUPPORTED,
			_("Can't monitor file or directory."));
      return TRUE;
    }
  
  g_vfs_job_create_monitor_set_monitor (job, network_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
g_vfs_backend_network_init (GVfsBackendNetwork *network_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (network_backend);
  GMountSpec *mount_spec;
  char *current_workgroup;
  GConfClient *gconf_client;
  const char * const* supported_vfs;
  int i;

  gconf_client = gconf_client_get_default ();

  gconf_client_add_dir (gconf_client, PATH_GCONF_GVFS_SMB, 
                        GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);

  current_workgroup = gconf_client_get_string (gconf_client, 
                                               PATH_GCONF_GVFS_SMB_WORKGROUP, NULL);

  if (current_workgroup == NULL ||
      current_workgroup[0] == 0) 
    /* it's okay if current_workgroup is null here, 
     * it's checked before the NetworkFile is added anyway. */
    network_backend->current_workgroup = NULL;
  else 
    network_backend->current_workgroup = current_workgroup;

  gconf_client_notify_add (gconf_client, PATH_GCONF_GVFS_SMB_WORKGROUP, 
                           notify_gconf_value_changed, backend, NULL, NULL);

  g_object_unref (gconf_client);

  supported_vfs = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

  network_backend->have_smb = FALSE;
  for (i=0; supported_vfs[i]!=NULL; i++)
    {
      if (strcmp(supported_vfs[i], "smb") == 0 )
	network_backend->have_smb = TRUE;
    }

  g_vfs_backend_set_display_name (backend, _("Network"));
  g_vfs_backend_set_stable_name (backend, _("Network"));
  g_vfs_backend_set_icon_name (backend, "network-workgroup");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("network");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  network_backend->mount_spec = mount_spec;
}

static void
g_vfs_backend_network_finalize (GObject *object)
{
  GVfsBackendNetwork *backend;

  backend = G_VFS_BACKEND_NETWORK (object);

  g_mount_spec_unref (backend->mount_spec);

  g_object_unref (backend->root_monitor);
  
  if (G_OBJECT_CLASS (g_vfs_backend_network_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_network_parent_class)->finalize) (object);
}

static void
g_vfs_backend_network_class_init (GVfsBackendNetworkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_network_finalize;

  backend_class->try_mount        = try_mount;
  backend_class->try_query_info   = try_query_info;
  backend_class->try_enumerate    = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_monitor;
  backend_class->try_create_file_monitor = try_create_monitor;
}

