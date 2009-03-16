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

#include <config.h>

#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
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

#define PATH_GCONF_GVFS_DNS_SD "/system/dns_sd"
#define PATH_GCONF_GVFS_DNS_SD_DISPLAY_LOCAL "/system/dns_sd/display_local"
#define PATH_GCONF_GVFS_DNS_SD_EXTRA_DOMAINS "/system/dns_sd/extra_domains"

#define NETWORK_FILE_ATTRIBUTES "standard::name,standard::display-name,standard::target-uri"

typedef struct {
  char *file_name; 
  char *display_name;
  char *target_uri;
  GIcon *icon;
} NetworkFile;

static NetworkFile root = { "/" };

typedef enum {
	NETWORK_LOCAL_DISABLED,
	NETWORK_LOCAL_MERGED,
	NETWORK_LOCAL_SEPARATE
} NetworkLocalSetting;

struct _GVfsBackendNetwork
{
  GVfsBackend parent_instance;
  GVfsMonitor *root_monitor;
  GMountSpec *mount_spec;
  GList *files; /* list of NetworkFiles */
  int idle_tag;

  /* SMB Stuff */
  gboolean have_smb;
  char *current_workgroup;
  GFileMonitor *smb_monitor;
  GMutex *smb_mount_lock;
  GVfsJobMount *mount_job;

  /* DNS-SD Stuff */
  gboolean have_dnssd;
  NetworkLocalSetting local_setting;
  char *extra_domains;
  GFileMonitor *dnssd_monitor;

  /* Icons */
  GIcon *workgroup_icon; /* GThemedIcon = "network-workgroup" */
  GIcon *server_icon; /* GThemedIcon = "network-server" */
};

typedef struct _GVfsBackendNetwork GVfsBackendNetwork;

G_DEFINE_TYPE (GVfsBackendNetwork, g_vfs_backend_network, G_VFS_TYPE_BACKEND);

static NetworkFile *
network_file_new (const char *file_name, 
                  const char *display_name, 
                  const char *target_uri, 
                  GIcon *icon)
{
  NetworkFile *file;
  
  file = g_slice_new0 (NetworkFile);

  file->file_name = g_strdup (file_name);
  file->display_name = g_strdup (display_name);
  file->target_uri = g_strdup (target_uri);
  file->icon = g_object_ref (icon);

  return file;
}

static void
network_file_free (NetworkFile *file)
{
  g_free (file->file_name);
  g_free (file->display_name);
  g_free (file->target_uri);
 
  if (file->icon)
    g_object_unref (file->icon);

  g_slice_free (NetworkFile, file);
}

/* Assumes file_name is equal and compares for
   metadata changes */
static gboolean
network_file_equal (NetworkFile *a,
                    NetworkFile *b)
{
  if (!g_icon_equal (a->icon, b->icon))
    return FALSE;

  if ((a->display_name != NULL && b->display_name == NULL) ||
      (a->display_name == NULL && b->display_name != NULL))
    return FALSE;

  if ((a->display_name != NULL && b->display_name != NULL) &&
      strcmp (a->display_name, b->display_name) != 0)
    return FALSE;

  return TRUE;
}

static int
sort_file_by_file_name (NetworkFile *a, NetworkFile *b)
{
  return strcmp (a->file_name, b->file_name);
}

static NetworkLocalSetting
parse_network_local_setting (const char *setting)
{
  if (setting == NULL)
    return NETWORK_LOCAL_DISABLED;
  if (strcmp (setting, "separate") == 0)
    return NETWORK_LOCAL_SEPARATE;
  if (strcmp (setting, "merged") == 0)
    return NETWORK_LOCAL_MERGED;
  return NETWORK_LOCAL_DISABLED;
}

static void
update_from_files (GVfsBackendNetwork *backend,
                   GList *files)
{
  GList *old_files;
  GList *oldl, *newl;
  char *file_name;
  NetworkFile *old, *new;
  int cmp;

  old_files = backend->files;
  backend->files = g_list_sort (files, (GCompareFunc)sort_file_by_file_name);

  /* Generate change events */
  oldl = old_files;
  newl = backend->files;
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
          cmp = sort_file_by_file_name (old, new);
        }
      
      if (cmp == 0)
        {
          if (!network_file_equal (old, new))
            {
              file_name = g_strconcat ("/", new->file_name, NULL);
              g_vfs_monitor_emit_event (backend->root_monitor,
                                        G_FILE_MONITOR_EVENT_CHANGED,
                                        file_name,
                                        NULL);
              g_free (file_name);
            }
          
          oldl = oldl->next;
          newl = newl->next;
        }
      else if (cmp < 0)
        {
          file_name = g_strconcat ("/", old->file_name, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_DELETED,
                                    file_name,
                                    NULL);
          g_free (file_name);
          oldl = oldl->next;
        }
      else
        {
          file_name = g_strconcat ("/", new->file_name, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_CREATED,
                                    file_name,
                                    NULL);
          g_free (file_name);
          newl = newl->next;
        }
    }

  g_list_foreach (old_files, (GFunc)network_file_free, NULL);
}

static void
notify_dnssd_local_changed (GFileMonitor *monitor, GFile *file, GFile *other_file, 
                            GFileMonitorEvent event_type, gpointer user_data);

static void
notify_smb_files_changed (GFileMonitor *monitor, GFile *file, GFile *other_file, 
                          GFileMonitorEvent event_type, gpointer user_data);

static void
recompute_files (GVfsBackendNetwork *backend)
{
  GFile *server_file;
  GFileEnumerator *enumer;
  GFileInfo *info;
  GFileMonitor *monitor;
  GError *error;
  GList *files;
  NetworkFile *file;
  char *file_name, *link_uri;

  files = NULL;
  error = NULL;
  if (backend->have_smb) 
    {
      char *workgroup;
      
      /* smb:/// root link */
      file = network_file_new ("smb-root", _("Windows Network"), 
                               "smb:///", backend->workgroup_icon);
      files = g_list_prepend (files, file);

      if (backend->current_workgroup == NULL ||
          backend->current_workgroup[0] == 0)
        workgroup = g_strconcat ("smb://", DEFAULT_WORKGROUP_NAME, "/", NULL);  
      else 
        workgroup = g_strconcat ("smb://", backend->current_workgroup, "/", NULL);    

      server_file = g_file_new_for_uri (workgroup);

      /* recreate monitor if our workgroup changed or we don't have a monitor */
      if (backend->smb_monitor == NULL)
        {
          monitor = g_file_monitor_directory (server_file, G_FILE_MONITOR_NONE, NULL, &error);
          if (monitor) 
            {
              g_signal_connect (monitor, "changed", 
                                (GCallback)notify_smb_files_changed, (gpointer)backend); 
              /* takes ref */
              backend->smb_monitor = monitor;
            }
          else
            {
	      char *uri = g_file_get_uri (server_file);
              g_warning ("Couldn't create directory monitor on %s. Error: %s", 
	     		 uri, error->message);
	      g_free (uri);
              g_clear_error (&error);
            }
        }

      /* children of current workgroup */
      enumer = g_file_enumerate_children (server_file, 
                                          NETWORK_FILE_ATTRIBUTES, 
                                          G_FILE_QUERY_INFO_NONE, 
                                          NULL, NULL);

      if (enumer != NULL)
        {
          info = g_file_enumerator_next_file (enumer, NULL, NULL);
          while (info != NULL)
            {
              file_name = g_strconcat("smb-server-", g_file_info_get_name (info), NULL);
              link_uri = g_strconcat("smb://", g_file_info_get_name (info), "/", NULL);
              file = network_file_new (file_name, 
                                       g_file_info_get_display_name (info), 
                                       link_uri, 
                                       backend->server_icon);
              files = g_list_prepend (files, file);

              g_free (link_uri);
              g_free (file_name);
              g_object_unref (info);
              info = g_file_enumerator_next_file (enumer, NULL, NULL);
            }
          g_file_enumerator_close (enumer, NULL, NULL);
          g_object_unref (enumer);
        }

      g_object_unref (server_file);

      g_free (workgroup);
    }

  if (backend->have_dnssd)
    {
      server_file = g_file_new_for_uri ("dns-sd://local/");
      /* create directory monitor if we haven't already */
      if (backend->dnssd_monitor == NULL)
	{
	  monitor = g_file_monitor_directory (server_file, G_FILE_MONITOR_NONE, NULL, &error);
	  if (monitor) 
	    {
	      g_signal_connect (monitor, "changed", 
				(GCallback)notify_dnssd_local_changed, (gpointer)backend); 
	      /* takes ref */
	      backend->dnssd_monitor = monitor;
	    }
	  else
	    {
	      char *uri = g_file_get_uri(server_file);
	      g_warning ("Couldn't create directory monitor on %s. Error: %s", 
			 uri, error->message);
	      g_free (uri);
	      g_clear_error (&error);
	    }
	}
      
      if (backend->local_setting == NETWORK_LOCAL_MERGED)
        {
          /* "merged": add local domains to network:/// */
          enumer = g_file_enumerate_children (server_file, 
                                              NETWORK_FILE_ATTRIBUTES, 
                                              G_FILE_QUERY_INFO_NONE, 
                                              NULL, NULL);
          if (enumer != NULL)
            {
              info = g_file_enumerator_next_file (enumer, NULL, NULL);
              while (info != NULL)
                {
                  file_name = g_strconcat("dnssd-domain-", g_file_info_get_name (info), NULL);
                  link_uri = g_strdup(g_file_info_get_attribute_string (info,
                                      "standard::target-uri"));
                  file = network_file_new (file_name, 
					   g_file_info_get_display_name (info), 
					   link_uri, 
					   backend->server_icon);
                  files = g_list_prepend (files, file);
		  
                  g_free (link_uri);
                  g_free (file_name);
                  g_object_unref (info);
                  info = g_file_enumerator_next_file (enumer, NULL, NULL);
                }
            }
	  
          g_file_enumerator_close (enumer, NULL, NULL);
          g_object_unref (enumer);
        }
      else
        {
          /* "separate": a link to dns-sd://local/ */
          file = network_file_new ("dnssd-local", _("Local Network"), 
				   "dns-sd://local/", backend->workgroup_icon);
          files = g_list_prepend (files, file);   
        }
      
      g_object_unref (server_file);
      
      /* If gconf setting "/system/dns_sd/extra_domains" is set to a list of domains:
       * links to dns-sd://$domain/ */
      if (backend->extra_domains != NULL &&
	  backend->extra_domains[0] != 0)
	{
	  char **domains;
	  int i;
	  domains = g_strsplit (backend->extra_domains, ",", 0);
	  for (i=0; domains[i] != NULL; i++)
	    {
	      file_name = g_strconcat("dnssd-domain-", domains[i], NULL);
	      link_uri = g_strconcat("dns-sd://", domains[i], "/", NULL);
	      file = network_file_new (file_name,
				       domains[i],
				       link_uri,
				       backend->workgroup_icon);
	      files = g_list_prepend (files, file);   
	      g_free (link_uri);
	      g_free (file_name);
	    }
	  g_strfreev (domains);
	}
    }
  
  update_from_files (backend, files);
}

static gboolean
idle_add_recompute (GVfsBackendNetwork *backend)
{
  backend->idle_tag = 0;

  recompute_files (backend);
  
  return FALSE;
}

static void
mount_smb_done_cb (GObject *object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(user_data);
  GError *error = NULL;

  g_file_mount_enclosing_volume_finish (G_FILE (object), res, &error);
  
  if (error)
    g_error_free (error);

  recompute_files (backend);

  /*  We've been spawned from try_mount  */
  if (backend->mount_job)
    {
      g_vfs_job_succeeded (G_VFS_JOB (backend->mount_job));
      g_object_unref (backend->mount_job);
    }  
  g_mutex_unlock (backend->smb_mount_lock);
}

static void
remount_smb (GVfsBackendNetwork *backend, GVfsJobMount *job)
{
  GFile *file;
  char *workgroup;

  if (! g_mutex_trylock (backend->smb_mount_lock))
    /*  Do nothing when the mount operation is already active  */
    return;
  
  backend->mount_job = job ? g_object_ref (job) : NULL;

  if (backend->current_workgroup == NULL ||
      backend->current_workgroup[0] == 0)
    workgroup = g_strconcat ("smb://", DEFAULT_WORKGROUP_NAME, "/", NULL);  
  else 
    workgroup = g_strconcat ("smb://", backend->current_workgroup, "/", NULL);    

  file = g_file_new_for_uri (workgroup);

  g_file_mount_enclosing_volume (file, G_MOUNT_MOUNT_NONE,
                                 NULL, NULL, mount_smb_done_cb, backend);
  g_free (workgroup);
  g_object_unref (file);
}

static void
notify_smb_files_changed (GFileMonitor *monitor, GFile *file, GFile *other_file, 
                          GFileMonitorEvent event_type, gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(user_data);
  
  switch (event_type)
    {
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
      if (backend->idle_tag == 0)
        backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
      break;
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      /* in either event, our smb backend is/will be gone. */
      if (backend->idle_tag == 0)
        backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
      
      /* stop monitoring as the backend's gone. */
      if (backend->smb_monitor)
        {
          g_file_monitor_cancel (backend->smb_monitor);
          g_object_unref (backend->smb_monitor);
          backend->smb_monitor = NULL;
        }  
      break;
    default:
      break;
    }
}

static void
notify_dnssd_local_changed (GFileMonitor *monitor, GFile *file, GFile *other_file, 
                            GFileMonitorEvent event_type, gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(user_data);
  switch (event_type)
    {
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
      if (backend->idle_tag == 0)
        backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
      break;
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      /* in either event, our dns-sd backend is/will be gone. */
      if (backend->idle_tag == 0)
        backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
      /* stop monitoring as the backend's gone. */
      g_file_monitor_cancel (backend->dnssd_monitor);
      g_object_unref (backend->dnssd_monitor);
      backend->dnssd_monitor = NULL;
      break;
    default:
      break;
    }
}

static void
notify_gconf_dnssd_domains_changed (GConfClient *client,
				    guint        cnxn_id,
				    GConfEntry  *entry,
				    gpointer     data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(data);
  char *extra_domains;
  extra_domains = gconf_client_get_string (client, 
                                           PATH_GCONF_GVFS_DNS_SD_EXTRA_DOMAINS, NULL);

  g_free (backend->extra_domains);
  backend->extra_domains = extra_domains;

  /* don't re-issue recomputes if we've already queued one. */
  if (backend->idle_tag == 0)
    backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
}

static void
notify_gconf_dnssd_display_local_changed (GConfClient *client,
                                          guint        cnxn_id,
                                          GConfEntry  *entry,
                                          gpointer     data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(data);
  char *display_local;
 
  display_local = gconf_client_get_string (client, 
                                           PATH_GCONF_GVFS_DNS_SD_DISPLAY_LOCAL, NULL);

  backend->local_setting = parse_network_local_setting (display_local);
  g_free (display_local);

  /* don't re-issue recomputes if we've already queued one. */
  if (backend->idle_tag == 0)
    backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
}

static void
notify_gconf_smb_workgroup_changed (GConfClient *client,
				    guint        cnxn_id,
				    GConfEntry  *entry,
				    gpointer     data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(data);
  char *current_workgroup;

  current_workgroup = gconf_client_get_string (client,
					       PATH_GCONF_GVFS_SMB_WORKGROUP, NULL);

  g_free (backend->current_workgroup);
  backend->current_workgroup = current_workgroup;

  /* cancel the smb monitor */
  if (backend->smb_monitor)
    {
      g_signal_handlers_disconnect_by_func (backend->smb_monitor,
					    notify_smb_files_changed,
					    backend->smb_monitor);
      g_file_monitor_cancel (backend->smb_monitor);
      g_object_unref (backend->smb_monitor);
      backend->smb_monitor = NULL;
    }  

  remount_smb (backend, NULL);
}

static NetworkFile *
lookup_network_file (GVfsBackendNetwork *backend,
                     GVfsJob *job,
                     const char *file_name)
{
  GList *l;
  NetworkFile *file;

  if (*file_name != '/')
    goto out;

  while (*file_name == '/')
    file_name++;

  if (*file_name == 0)
    return &root;
  
  if (strchr (file_name, '/') != NULL)
    goto out;
  
  for (l = backend->files; l != NULL; l = l->next)
    {
      file = l->data;
      if (strcmp (file->file_name, file_name) == 0)
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
  g_return_if_fail (file != NULL || info != NULL);

  g_file_info_set_name (info, file->file_name);
  g_file_info_set_display_name (info, file->display_name);

  if (file->icon) 
    g_file_info_set_icon (info, file->icon);

  g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
  g_file_info_set_size (info, 0);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, 
                                    file->target_uri);
}

/* Backend Functions */
static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *file_name,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  NetworkFile *file;
  GList *l;
  GFileInfo *info;
  file = lookup_network_file (G_VFS_BACKEND_NETWORK (backend),
			      G_VFS_JOB (job), file_name);
  
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
                const char *file_name,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  NetworkFile *file;

  file = lookup_network_file (G_VFS_BACKEND_NETWORK (backend), 
                              G_VFS_JOB (job), file_name);

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

  if (network_backend->have_smb)
    {
      remount_smb (network_backend, job);
    }
  else
    {
      recompute_files (network_backend);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }  
    
  return TRUE;
}

/* handles both file and dir monitors
 * for now we only fire events on the root directory.
 * are individual file monitors needed for this backend?
 */
static gboolean
try_create_monitor (GVfsBackend *backend,
                    GVfsJobCreateMonitor *job,
                    const char *file_name,
                    GFileMonitorFlags flags)
{
  NetworkFile *file;
  GVfsBackendNetwork *network_backend;

  network_backend = G_VFS_BACKEND_NETWORK (backend);

  file = lookup_network_file (network_backend, G_VFS_JOB (job), file_name);

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
  GConfClient *gconf_client;
  char *display_local, *extra_domains;
  char *current_workgroup;
  const char * const* supported_vfs;
  int i;

  network_backend->smb_mount_lock = g_mutex_new ();

  supported_vfs = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

  network_backend->have_smb = FALSE;
  network_backend->have_dnssd = FALSE;
  for (i=0; supported_vfs[i]!=NULL; i++)
    {
      if (strcmp(supported_vfs[i], "smb") == 0)
	network_backend->have_smb = TRUE;

      if (strcmp(supported_vfs[i], "dns-sd") == 0)
        network_backend->have_dnssd = TRUE;
    }

  gconf_client = gconf_client_get_default ();

  if (network_backend->have_smb) 
    {
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
                               notify_gconf_smb_workgroup_changed, network_backend, NULL, NULL);

     }

  if (network_backend->have_dnssd) 
    {
      gconf_client_add_dir (gconf_client, PATH_GCONF_GVFS_DNS_SD, 
                            GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);

      display_local = gconf_client_get_string (gconf_client, 
                                               PATH_GCONF_GVFS_DNS_SD_DISPLAY_LOCAL, NULL);
      extra_domains = gconf_client_get_string (gconf_client, 
                                               PATH_GCONF_GVFS_DNS_SD_EXTRA_DOMAINS, NULL);
      
      if (display_local != NULL && display_local[0] != 0)
         network_backend->local_setting = parse_network_local_setting (display_local);

      g_free (display_local);
      network_backend->extra_domains = extra_domains;

      gconf_client_notify_add (gconf_client, PATH_GCONF_GVFS_DNS_SD_EXTRA_DOMAINS, 
                               notify_gconf_dnssd_domains_changed, network_backend, NULL, NULL);
     
      gconf_client_notify_add (gconf_client, PATH_GCONF_GVFS_DNS_SD_DISPLAY_LOCAL, 
                               notify_gconf_dnssd_display_local_changed, network_backend, NULL, NULL);

    }

  g_object_unref (gconf_client);

  g_vfs_backend_set_display_name (backend, _("Network"));
  g_vfs_backend_set_stable_name (backend, _("Network"));
  g_vfs_backend_set_icon_name (backend, "network-workgroup");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("network");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  network_backend->mount_spec = mount_spec;

  network_backend->workgroup_icon = g_themed_icon_new ("network-workgroup");
  network_backend->server_icon = g_themed_icon_new ("network-server");
}

static void
g_vfs_backend_network_finalize (GObject *object)
{
  GVfsBackendNetwork *backend;
  backend = G_VFS_BACKEND_NETWORK (object);

  g_mutex_free (backend->smb_mount_lock);
  g_mount_spec_unref (backend->mount_spec);
  g_object_unref (backend->root_monitor);
  g_object_unref (backend->workgroup_icon);
  g_object_unref (backend->server_icon);
  
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

void 
g_vfs_network_daemon_init (void)
{
  /* Translators: this is the friendly name of the 'network://' backend that
   * shows computers in your local network. */
  g_set_application_name (_("Network Location Monitor"));
}

