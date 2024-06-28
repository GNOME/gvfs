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
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
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

#include "gvfsbackendnetwork.h"

#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsmonitor.h"
#include "gvfs-enums.h"

#define DEFAULT_WORKGROUP_NAME "X-GNOME-DEFAULT-WORKGROUP"

#define NETWORK_FILE_ATTRIBUTES "standard::name,standard::display-name,standard::target-uri,standard::content-type"

typedef struct {
  char *file_name;
  char *display_name;
  char *target_uri;
  GRefString *content_type;
  GIcon *icon;
  GIcon *symbolic_icon;
  guint num_duplicates;
} NetworkFile;

static NetworkFile root = { "/" };

struct _GVfsBackendNetwork
{
  GVfsBackend parent_instance;
  GVfsMonitor *root_monitor;
  GMountSpec *mount_spec;
  GList *files; /* list of NetworkFiles */
  int idle_tag;
  GSettings *smb_settings;
  GSettings *dnssd_settings;

  /* SMB Stuff */
  gboolean have_smb;
  GVfsBackendNetworkDisplayMode smb_display_mode;
  GMutex smb_mount_lock;
  GVfsJobMount *mount_job;
  GFileEnumerator *smb_enumerator;

  /* DNS-SD Stuff */
  gboolean have_dnssd;
  GVfsBackendNetworkDisplayMode dnssd_display_mode;
  char *extra_domains;
  GFileMonitor *dnssd_monitor;

  /* WSDD */
  gboolean have_wsdd;
  GVfsBackendNetworkDisplayMode wsdd_display_mode;
  GFileMonitor *wsdd_monitor;
  GSettings *wsdd_settings;

  /* Icons */
  GIcon *workgroup_icon; /* GThemedIcon = "network-workgroup" */
  GIcon *server_icon; /* GThemedIcon = "network-server" */
  GIcon *workgroup_symbolic_icon; /* GThemedIcon = "network-workgroup-symbolic" */
  GIcon *server_symbolic_icon; /* GThemedIcon = "network-server-symbolic" */
};

typedef struct _GVfsBackendNetwork GVfsBackendNetwork;

G_DEFINE_TYPE (GVfsBackendNetwork, g_vfs_backend_network, G_VFS_TYPE_BACKEND);

static NetworkFile *
network_file_new (const char *file_name, 
                  const char *display_name, 
                  const char *target_uri, 
                  const char *content_type,
                  GIcon      *icon,
                  GIcon      *symbolic_icon)
{
  NetworkFile *file;
  
  file = g_slice_new0 (NetworkFile);

  file->file_name = g_strdup (file_name);
  file->display_name = g_strdup (display_name);
  file->target_uri = g_strdup (target_uri);
  file->content_type = g_ref_string_new_intern (content_type);
  file->icon = g_object_ref (icon);
  file->symbolic_icon = g_object_ref (symbolic_icon);

  return file;
}

static GList *
network_files_from_enumerator (GList *files,
                               GFileEnumerator *enumerator,
                               const gchar *prefix,
                               GIcon *icon,
                               GIcon *symbolic_icon)
{
  GFileInfo *info = NULL;

  g_return_val_if_fail (enumerator != NULL, files);

  info = g_file_enumerator_next_file (enumerator, NULL, NULL);
  while (info != NULL)
    {
      g_autofree gchar *file_name = NULL;
      const gchar *uri;
      NetworkFile *file;

      file_name = g_strconcat (prefix, g_file_info_get_name (info), NULL);
      uri = g_file_info_get_attribute_string (info,
                                              G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
      file = network_file_new (file_name,
                               g_file_info_get_display_name (info),
                               uri,
                               g_file_info_get_content_type (info),
                               icon,
                               symbolic_icon);
      files = g_list_prepend (files, file);

      g_object_unref (info);
      info = g_file_enumerator_next_file (enumerator, NULL, NULL);
    }

  return files;
}

static GList *
network_files_from_directory (GList *files,
                              GFile *directory,
                              const gchar *prefix,
                              GIcon *icon,
                              GIcon *symbolic_icon)
{
  g_autoptr (GFileEnumerator) enumerator = NULL;

  g_return_val_if_fail (directory != NULL, files);

  enumerator = g_file_enumerate_children (directory,
                                          NETWORK_FILE_ATTRIBUTES,
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL,
                                          NULL);
  if (enumerator != NULL)
    {
      return network_files_from_enumerator (files,
                                            enumerator,
                                            prefix,
                                            icon,
                                            symbolic_icon);
    }

  return files;
}

static void
network_file_free (NetworkFile *file)
{
  g_free (file->file_name);
  g_free (file->display_name);
  g_free (file->target_uri);
  g_ref_string_release (file->content_type);
 
  if (file->icon)
    g_object_unref (file->icon);
  if (file->symbolic_icon)
    g_object_unref (file->symbolic_icon);

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
  if (!g_icon_equal (a->symbolic_icon, b->symbolic_icon))
    return FALSE;

  if (g_strcmp0 (a->display_name, b->display_name) != 0)
    return FALSE;

  if (g_strcmp0 (a->target_uri, b->target_uri) != 0)
    return FALSE;

  return TRUE;
}

static int
sort_file_by_file_name (NetworkFile *a, NetworkFile *b)
{
  return strcmp (a->file_name, b->file_name);
}

static char *
get_pretty_scheme_for_uri (const char *uri)
{
  GFile *file;
  char *scheme;
  char *pretty = NULL;

  file = g_file_new_for_uri (uri);
  if (file == NULL)
    return NULL;

  scheme = g_file_get_uri_scheme (file);
  if (g_strcmp0 (scheme, "afp") == 0
      || g_strcmp0 (scheme, "smb") == 0)
    {
      pretty = g_strdup (_("File Sharing"));
    }
  else if (g_strcmp0 (scheme, "sftp") == 0
           || g_strcmp0 (scheme, "ssh") == 0)
    {
      pretty = g_strdup (_("Remote Login"));
    }
  else
    {
      pretty = g_strdup (scheme);
    }

  g_free (scheme);

  return pretty;
}

static void
network_file_append_service_name (NetworkFile *file)
{
  char *name;
  char *service;

  service = get_pretty_scheme_for_uri (file->target_uri);
  name = g_strdup_printf ("%s (%s)", file->display_name, service);
  g_free (service);
  g_free (file->display_name);
  file->display_name = name;
}

static GList *
uniquify_display_names (GList *files)
{
  GHashTable *names;
  GList *l;

  names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  files = g_list_sort (files, (GCompareFunc)sort_file_by_file_name);
  l = files;
  while (l != NULL)
    {
      NetworkFile *prev_file;
      NetworkFile *file = l->data;

      prev_file = g_hash_table_lookup (names, file->display_name);
      if (prev_file != NULL)
        {
          /* The smb:// services come from 3 different backends (ie. dnssd,
           * smbbrowse, wsdd). It is not desired to list the same service
           * several times. The problem is that each backend uses different URIs
           * (e.g. "smb://foo.local:445", "smb://foo", "smb://127.0.0.1").
           * Resolving URIs just for their comparison doesn't sound like a good
           * idea. Let's make an assumption that the files with the same display
           * name and the same scheme point to the same service. Let's keep just
           * the first one.
           */
          if (g_strcmp0 (g_uri_peek_scheme (prev_file->target_uri),
                         g_uri_peek_scheme (file->target_uri)) == 0)
            {
              GList *next = l->next;

              g_debug ("Skipping %s in favor of %s\n",
                       file->file_name,
                       prev_file->file_name);

              network_file_free (file);
              files = g_list_delete_link (files, l);
              l = next;
              continue;
            }

          prev_file->num_duplicates++;
          /* only change the first file once */
          if (prev_file->num_duplicates == 1)
            network_file_append_service_name (prev_file);
          network_file_append_service_name (file);
        }
      g_hash_table_replace (names, g_strdup (file->display_name), file);

      l = l->next;
    }

  g_hash_table_destroy (names);

  return files;
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
  backend->files = uniquify_display_names (files);

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

  g_list_free_full (old_files, (GDestroyNotify)network_file_free);
}

static void
notify_dnssd_local_changed (GFileMonitor *monitor, GFile *file, GFile *other_file, 
                            GFileMonitorEvent event_type, gpointer user_data);
static void
wsdd_monitor_changed_cb (GFileMonitor *monitor,
                         GFile *file,
                         GFile *other_file,
                         GFileMonitorEvent event_type,
                         gpointer user_data);

static void
recompute_files (GVfsBackendNetwork *backend)
{
  GFile *server_file;
  GFileEnumerator *enumer;
  GFileMonitor *monitor;
  GError *error;
  GList *files;
  NetworkFile *file;
  char *file_name, *link_uri;

  files = NULL;
  error = NULL;
  if (backend->have_smb &&
      backend->smb_display_mode != G_VFS_BACKEND_NETWORK_DISPLAY_MODE_DISABLED)
    {
      if (backend->smb_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
        {
          server_file = g_file_new_for_uri ("smb:///");
          enumer = g_file_enumerate_children (server_file,
                                              NETWORK_FILE_ATTRIBUTES,
                                              G_FILE_QUERY_INFO_NONE,
                                              NULL,
                                              NULL);
          if (enumer != NULL)
            {
              GFileInfo *workgroup_info;

              workgroup_info = g_file_enumerator_next_file (enumer,
                                                            NULL,
                                                            NULL);
              while (workgroup_info != NULL)
                {
                  g_autoptr(GFile) workgroup = NULL;
                  g_autoptr(GFileEnumerator) workgroup_enumerator = NULL;
                  const gchar *workgroup_target;

                  workgroup_target = g_file_info_get_attribute_string (workgroup_info,
                                                                       G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
                  workgroup = g_file_new_for_uri (workgroup_target);
                  workgroup_enumerator = g_file_enumerate_children (workgroup,
                                                                    NETWORK_FILE_ATTRIBUTES,
                                                                    G_FILE_QUERY_INFO_NONE,
                                                                    NULL,
                                                                    NULL);
                  if (workgroup_enumerator != NULL)
                    {
                      files = network_files_from_enumerator (files,
                                                             workgroup_enumerator,
                                                             "smb-server-",
                                                             backend->server_icon,
                                                             backend->server_symbolic_icon);
                    }
                  else
                    {
                      file_name = g_strconcat ("smb-workgroup-",
                                               g_file_info_get_name (workgroup_info),
                                               NULL);
                      file = network_file_new (file_name,
                                               g_file_info_get_display_name (workgroup_info),
                                               workgroup_target,
                                               g_file_info_get_content_type (workgroup_info),
                                               backend->workgroup_icon,
                                               backend->workgroup_symbolic_icon);
                      files = g_list_prepend (files, file);

                      g_free (file_name);
                    }

                  g_object_unref (workgroup_info);

                  workgroup_info = g_file_enumerator_next_file (enumer,
                                                                NULL,
                                                                NULL);
                }

              g_file_enumerator_close (enumer, NULL, NULL);
              g_object_unref (enumer);
            }

          g_object_unref (server_file);
        }
      else if (backend->smb_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_SEPARATE)
        {
          file = network_file_new ("smb-root",
                                   _("Windows Network"),
                                   "smb:///",
                                   "inode/directory",
                                   backend->workgroup_icon,
                                   backend->workgroup_symbolic_icon);
          files = g_list_prepend (files, file);
        }
    }

  if (backend->have_dnssd &&
      backend->dnssd_display_mode != G_VFS_BACKEND_NETWORK_DISPLAY_MODE_DISABLED)
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
      
      if (backend->dnssd_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
        {
          files = network_files_from_directory (files,
                                                server_file,
                                                "dnssd-server-",
                                                backend->server_icon,
                                                backend->server_symbolic_icon);
        }
      else
        {
          /* "separate": a link to dns-sd://local/ */
          file = network_file_new ("dnssd-local",
                                   _("Local Network"),
				   "dns-sd://local/",
                                   "inode/directory",
                                   backend->workgroup_icon,
                                   backend->workgroup_symbolic_icon);
          files = g_list_prepend (files, file);   
        }
      
      g_object_unref (server_file);
      
      /* If gsettings key "extra-domains" (org.gnome.system.dns_sd) is set to a list of domains:
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
                                       "inode/directory",
				       backend->workgroup_icon,
				       backend->workgroup_symbolic_icon);
	      files = g_list_prepend (files, file);   
	      g_free (link_uri);
	      g_free (file_name);
	    }
	  g_strfreev (domains);
	}
    }

  if (backend->have_wsdd &&
      backend->wsdd_display_mode != G_VFS_BACKEND_NETWORK_DISPLAY_MODE_DISABLED)
    {
      server_file = g_file_new_for_uri ("wsdd:///");

      if (backend->wsdd_monitor == NULL)
        {
          monitor = g_file_monitor_directory (server_file,
                                              G_FILE_MONITOR_NONE,
                                              NULL,
                                              &error);
          if (monitor)
            {
              g_signal_connect (monitor,
                                "changed",
                                G_CALLBACK (wsdd_monitor_changed_cb),
                                backend);

              backend->wsdd_monitor = monitor;
            }
          else
            {
              gchar *uri = g_file_get_uri (server_file);

              g_warning ("Couldn't create directory monitor on %s. Error: %s",
                         uri,
                         error->message);

              g_free (uri);
              g_clear_error (&error);
            }
        }

      if (backend->wsdd_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
        {
          files = network_files_from_directory (files,
                                                server_file,
                                                "wsdd-server-",
                                                backend->server_icon,
                                                backend->server_symbolic_icon);
        }
      else
        {
          file = network_file_new ("wsdd-root",
                                   _("WSDD Network"),
                                   "wsdd:///",
                                   "inode/directory",
                                   backend->workgroup_icon,
                                   backend->workgroup_symbolic_icon);
          files = g_list_prepend (files, file);
        }

      g_object_unref (server_file);
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
schedule_recompute (GVfsBackendNetwork *backend)
{
  /* Don't re-issue recomputes if we've already queued one. */
  if (backend->idle_tag == 0)
    {
      backend->idle_tag = g_idle_add ((GSourceFunc)idle_add_recompute, backend);
    }
}

static void
mount_smb_finish (GVfsBackendNetwork *backend)
{
  schedule_recompute (backend);

  /*  We've been spawned from try_mount  */
  if (backend->mount_job)
    {
      g_vfs_job_succeeded (G_VFS_JOB (backend->mount_job));
      g_object_unref (backend->mount_job);
    }  
  g_mutex_unlock (&backend->smb_mount_lock);
  g_object_unref (backend);
}

static void mount_smb_next_workgroup (GVfsBackendNetwork *backend);

static void
mount_smb_next_workgroup_cb (GObject *object,
                             GAsyncResult *res,
                             gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK (user_data);
  GFile *workgroup = G_FILE (object);

  g_file_mount_enclosing_volume_finish (workgroup, res, NULL);

  mount_smb_next_workgroup (backend);
}

static void
mount_smb_next_workgroup (GVfsBackendNetwork *backend)
{
  GFileInfo *info;
  GFile *workgroup;
  const gchar *workgroup_target;

  info = g_file_enumerator_next_file (backend->smb_enumerator, NULL, NULL);
  if (info == NULL)
    {
      g_file_enumerator_close (backend->smb_enumerator, NULL, NULL);
      g_clear_object (&backend->smb_enumerator);

      mount_smb_finish (backend);
      return;
    }

  workgroup_target = g_file_info_get_attribute_string (info,
                                                       G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
  workgroup = g_file_new_for_uri (workgroup_target);
  g_file_mount_enclosing_volume (workgroup,
                                 G_MOUNT_MOUNT_NONE,
                                 NULL,
                                 NULL,
                                 mount_smb_next_workgroup_cb,
                                 backend);
}

static void
mount_smb_root_cb (GObject *object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK (user_data);
  GFile *root = G_FILE (object);

  g_file_mount_enclosing_volume_finish (root, res, NULL);

  backend->smb_enumerator = g_file_enumerate_children (root,
                                                       NETWORK_FILE_ATTRIBUTES,
                                                       G_FILE_QUERY_INFO_NONE,
                                                       NULL,
                                                       NULL);
  if (backend->smb_enumerator == NULL)
    {
      mount_smb_finish (backend);
      return;
    }

  mount_smb_next_workgroup (backend);
}

static void
remount_smb (GVfsBackendNetwork *backend, GVfsJobMount *job)
{
  GFile *root;

  if (! g_mutex_trylock (&backend->smb_mount_lock))
    /*  Do nothing when the mount operation is already active  */
    return;
  
  backend->mount_job = job ? g_object_ref (job) : NULL;

  root = g_file_new_for_uri ("smb:///");
  g_file_mount_enclosing_volume (root,
                                 G_MOUNT_MOUNT_NONE,
                                 NULL,
                                 NULL,
                                 mount_smb_root_cb,
                                 g_object_ref (backend));

  g_object_unref (root);
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
      schedule_recompute (backend);
      break;
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      /* in either event, our dns-sd backend is/will be gone. */
      schedule_recompute (backend);
      /* stop monitoring as the backend's gone. */
      g_file_monitor_cancel (backend->dnssd_monitor);
      g_object_unref (backend->dnssd_monitor);
      backend->dnssd_monitor = NULL;
      break;
    default:
      break;
    }
}

static gboolean
dnssd_settings_change_event_cb (GSettings *settings,
                                gpointer   keys,
                                gint       n_keys,
                                gpointer   user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(user_data);

  g_free (backend->extra_domains);
  backend->extra_domains = g_settings_get_string (settings, "extra-domains");
  backend->dnssd_display_mode = g_settings_get_enum (settings, "display-local");

  schedule_recompute (backend);

  return FALSE;
}

static gboolean
smb_settings_change_event_cb (GSettings *settings,
                              gpointer   keys,
                              gint       n_keys,
                              gpointer   user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK(user_data);

  backend->smb_display_mode = g_settings_get_enum (settings, "display-mode");

  if (backend->smb_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
    {
      remount_smb (backend, NULL);
    }
  else
    {
      schedule_recompute (backend);
    }

  return FALSE;
}

static void
wsdd_monitor_changed_cb (GFileMonitor *monitor,
                         GFile *file,
                         GFile *other_file,
                         GFileMonitorEvent event_type,
                         gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK (user_data);

  switch (event_type)
    {
      case G_FILE_MONITOR_EVENT_UNMOUNTED:
        g_clear_object (&backend->wsdd_monitor);

      default:
        schedule_recompute (backend);
        break;
    }
}

static gboolean
wsdd_settings_change_event_cb (GSettings *settings,
                               gpointer keys,
                               gint n_keys,
                               gpointer user_data)
{
  GVfsBackendNetwork *backend = G_VFS_BACKEND_NETWORK (user_data);

  backend->wsdd_display_mode = g_settings_get_enum (settings, "display-mode");

  schedule_recompute (backend);

  return FALSE;
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
		    _("File doesn’t exist"));

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
  if (file->symbolic_icon)
    g_file_info_set_symbolic_icon (info, file->symbolic_icon);

  g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
  g_file_info_set_content_type (info, file->content_type);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
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
  GVfsBackendNetwork *network_backend = G_VFS_BACKEND_NETWORK (backend);
  NetworkFile *file;
  GList *l;
  GFileInfo *info;
  file = lookup_network_file (network_backend,
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

  /* The smb backend doesn't support monitoring, so let's recompute here. */
  if (network_backend->have_smb &&
      network_backend->smb_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
    {
      recompute_files (network_backend);
    }
  
  /* Enumerate root */
  for (l = network_backend->files; l != NULL; l = l->next)
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
      icon = g_themed_icon_new ("network-workgroup-symbolic");
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
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

  if (network_backend->have_smb &&
      network_backend->smb_display_mode == G_VFS_BACKEND_NETWORK_DISPLAY_MODE_MERGED)
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
			_("Can’t monitor file or directory."));
      return TRUE;
    }
  
  g_vfs_job_create_monitor_set_monitor (job, network_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "network");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_network_init (GVfsBackendNetwork *network_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (network_backend);
  GMountSpec *mount_spec;
  const char * const* supported_vfs;
  int i;

  g_mutex_init (&network_backend->smb_mount_lock);

  supported_vfs = g_vfs_get_supported_uri_schemes (g_vfs_get_default ());

  network_backend->have_smb = FALSE;
  network_backend->have_dnssd = FALSE;
  network_backend->have_wsdd = FALSE;

  for (i=0; supported_vfs[i]!=NULL; i++)
    {
      if (strcmp(supported_vfs[i], "smb") == 0)
	network_backend->have_smb = TRUE;

      if (strcmp(supported_vfs[i], "dns-sd") == 0)
        network_backend->have_dnssd = TRUE;

      if (g_strcmp0 (supported_vfs[i], "wsdd") == 0)
        network_backend->have_wsdd = TRUE;
    }

  if (network_backend->have_smb)
    {
      network_backend->smb_settings = g_settings_new ("org.gnome.system.smb");

      network_backend->smb_display_mode = g_settings_get_enum (network_backend->smb_settings, "display-mode");

      g_signal_connect (network_backend->smb_settings,
                        "change-event",
                        G_CALLBACK (smb_settings_change_event_cb),
                        network_backend);
    }

  if (network_backend->have_dnssd) 
    {
      network_backend->dnssd_settings = g_settings_new ("org.gnome.system.dns_sd");

      network_backend->dnssd_display_mode = g_settings_get_enum (network_backend->dnssd_settings, "display-local");
      network_backend->extra_domains = g_settings_get_string (network_backend->dnssd_settings, "extra-domains");

      g_signal_connect (network_backend->dnssd_settings,
                        "change-event",
                        G_CALLBACK (dnssd_settings_change_event_cb),
                        network_backend);
    }

  if (network_backend->have_wsdd)
    {
      network_backend->wsdd_settings = g_settings_new ("org.gnome.system.wsdd");

      network_backend->wsdd_display_mode = g_settings_get_enum (network_backend->wsdd_settings,
                                                                "display-mode");

      g_signal_connect (network_backend->wsdd_settings,
                        "change-event",
                        G_CALLBACK (wsdd_settings_change_event_cb),
                        network_backend);
    }

  g_vfs_backend_set_display_name (backend, _("Network"));
  g_vfs_backend_set_stable_name (backend, _("Network"));
  g_vfs_backend_set_icon_name (backend, "network-workgroup");
  g_vfs_backend_set_symbolic_icon_name (backend, "network-workgroup-symbolic");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("network");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  network_backend->mount_spec = mount_spec;

  network_backend->workgroup_icon = g_themed_icon_new ("network-workgroup");
  network_backend->server_icon = g_themed_icon_new ("network-server");
  network_backend->workgroup_symbolic_icon = g_themed_icon_new ("network-workgroup-symbolic");
  network_backend->server_symbolic_icon = g_themed_icon_new ("network-server-symbolic");
}

static void
g_vfs_backend_network_finalize (GObject *object)
{
  GVfsBackendNetwork *backend;
  backend = G_VFS_BACKEND_NETWORK (object);

  g_mutex_clear (&backend->smb_mount_lock);
  g_mount_spec_unref (backend->mount_spec);
  g_object_unref (backend->root_monitor);
  g_object_unref (backend->workgroup_icon);
  g_object_unref (backend->server_icon);
  g_object_unref (backend->workgroup_symbolic_icon);
  g_object_unref (backend->server_symbolic_icon);
  if (backend->smb_settings)
    {
      g_signal_handlers_disconnect_by_func (backend->smb_settings, smb_settings_change_event_cb, backend);
      g_clear_object (&backend->smb_settings);
    }
  if (backend->dnssd_settings)
    {
      g_signal_handlers_disconnect_by_func (backend->dnssd_settings, dnssd_settings_change_event_cb, backend);
      g_clear_object (&backend->dnssd_settings);
    }
  if (backend->dnssd_monitor)
    {
      g_signal_handlers_disconnect_by_func (backend->dnssd_monitor, notify_dnssd_local_changed, backend);
      g_clear_object (&backend->dnssd_monitor);
    }

  if (backend->wsdd_settings)
    {
      g_signal_handlers_disconnect_by_func (backend->wsdd_settings, wsdd_settings_change_event_cb, backend);
      g_clear_object (&backend->wsdd_settings);
    }

  if (backend->wsdd_monitor)
    {
      g_signal_handlers_disconnect_by_func (backend->wsdd_monitor, wsdd_monitor_changed_cb, backend);
      g_clear_object (&backend->wsdd_monitor);
    }

  if (backend->idle_tag)
    {
      g_source_remove (backend->idle_tag);
      backend->idle_tag = 0;
    }
  if (backend->files)
    {
      g_list_free_full (backend->files, (GDestroyNotify)network_file_free);
      backend->files = NULL;
    }

  g_free (backend->extra_domains);

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
  backend_class->try_query_fs_info = try_query_fs_info;
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

