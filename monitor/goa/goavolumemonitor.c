/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2012, 2013 Red Hat, Inc.
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
 * Author: Debarshi Ray <debarshir@gnome.org>
 */

#include <config.h>

#include <stdio.h>

#include <glib.h>
#include <gio/gio.h>

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>

#include <gvfsproxyvolumemonitordaemon.h>
#include "goavolume.h"
#include "goavolumemonitor.h"

struct _GVfsGoaVolumeMonitor
{
  GNativeVolumeMonitor parent;
  GList *accounts;
  GList *volumes;
  GoaClient *client;
};

struct _GVfsGoaVolumeMonitorClass
{
  GVolumeMonitorClass parent_class;
};

G_DEFINE_TYPE(GVfsGoaVolumeMonitor, g_vfs_goa_volume_monitor, G_TYPE_VOLUME_MONITOR)

/* ---------------------------------------------------------------------------------------------------- */

static void
diff_sorted_lists (GList         *list1,
                   GList         *list2,
                   GCompareFunc   compare,
                   GList        **added,
                   GList        **removed,
                   GList        **unchanged)
{
  int order;

  *added = *removed = NULL;
  if (unchanged != NULL)
    *unchanged = NULL;

  while (list1 != NULL &&
         list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          if (unchanged != NULL)
            *unchanged = g_list_prepend (*unchanged, list1->data);
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
account_compare (GoaObject *a, GoaObject *b)
{
  GoaAccount *account_a;
  GoaAccount *account_b;

  account_a = goa_object_peek_account (a);
  account_b = goa_object_peek_account (b);

  return g_strcmp0 (goa_account_get_id (account_a), goa_account_get_id (account_b));
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
uuid_compare (GVolume *volume_a, const gchar *uuid_b)
{
  gchar *uuid_a;
  gint ret_val;

  uuid_a = g_volume_get_uuid (volume_a);
  ret_val = g_strcmp0 (uuid_a, uuid_b);
  g_free (uuid_a);

  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
volume_compare (GVolume *a, GVolume *b)
{
  gchar *uuid_a;
  gchar *uuid_b;
  gint ret_val;

  uuid_a = g_volume_get_uuid (a);
  uuid_b = g_volume_get_uuid (b);

  ret_val = g_strcmp0 (uuid_a, uuid_b);

  g_free (uuid_a);
  g_free (uuid_b);

  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
object_list_emit (GVfsGoaVolumeMonitor *monitor,
                  const gchar          *monitor_signal,
                  const gchar          *object_signal,
                  GList                *objects)
{
  GList *l;

  for (l = objects; l != NULL; l = l->next)
    {
      g_signal_emit_by_name (monitor, monitor_signal, l->data);
      if (object_signal)
        g_signal_emit_by_name (l->data, object_signal);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_accounts (GVfsGoaVolumeMonitor *self, GList **added_accounts, GList **removed_accounts)
{
  GList *added;
  GList *l;
  GList *new_accounts;
  GList *new_files_accounts = NULL;
  GList *removed;

  if (added_accounts != NULL)
    *added_accounts = NULL;

  if (removed_accounts != NULL)
    *removed_accounts = NULL;

  new_accounts = goa_client_get_accounts (self->client);
  for (l = new_accounts; l != NULL; l = l->next)
    {
      GoaObject *object = GOA_OBJECT (l->data);

      if (goa_object_peek_files (object) != NULL)
        new_files_accounts = g_list_prepend (new_files_accounts, object);
    }

  new_files_accounts = g_list_sort (new_files_accounts, (GCompareFunc) account_compare);
  diff_sorted_lists (self->accounts,
                     new_files_accounts,
                     (GCompareFunc) account_compare,
                     &added,
                     &removed,
                     NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      GList *llink;
      GoaObject *object = GOA_OBJECT (l->data);

      if (removed_accounts != NULL)
        *removed_accounts = g_list_prepend (*removed_accounts, g_object_ref (object));

      llink = g_list_find_custom (self->accounts, object, (GCompareFunc) account_compare);
      self->accounts = g_list_remove_link (self->accounts, llink);
      g_list_free_full (llink, g_object_unref);
    }

  for (l = added; l != NULL; l = l->next)
    {
      GoaObject *object = GOA_OBJECT (l->data);

      self->accounts = g_list_prepend (self->accounts, g_object_ref (object));

      if (added_accounts != NULL)
        *added_accounts = g_list_prepend (*added_accounts, g_object_ref (object));
    }

  self->accounts = g_list_sort (self->accounts, (GCompareFunc) account_compare);

  g_list_free (added);
  g_list_free (removed);
  g_list_free (new_files_accounts);
  g_list_free_full (new_accounts, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_volumes (GVfsGoaVolumeMonitor *self, GList **added_volumes, GList **removed_volumes)
{
  GList *added;
  GList *l;
  GList *new_volumes = NULL;
  GList *removed;

  if (added_volumes != NULL)
    *added_volumes = NULL;

  if (removed_volumes != NULL)
    *removed_volumes = NULL;

  for (l = self->accounts; l != NULL; l = l->next)
    {
      GVolume *volume;
      GoaFiles *files;
      GoaObject *object = GOA_OBJECT (l->data);
      const gchar *uri;

      files = goa_object_peek_files (object);
      uri = goa_files_get_uri (files);

      volume = g_vfs_goa_volume_new (object, uri);
      new_volumes = g_list_prepend (new_volumes, volume);
    }

  new_volumes = g_list_sort (new_volumes, (GCompareFunc) volume_compare);
  diff_sorted_lists (self->volumes,
                     new_volumes,
                     (GCompareFunc) volume_compare,
                     &added,
                     &removed,
                     NULL);

  for (l = removed; l != NULL; l = l->next)
    {
      GList *llink;
      GVolume *volume = G_VOLUME (l->data);

      if (removed_volumes != NULL)
        *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));

      llink = g_list_find_custom (self->volumes, volume, (GCompareFunc) volume_compare);
      self->volumes = g_list_remove_link (self->volumes, llink);
      g_list_free_full (llink, g_object_unref);
    }

  for (l = added; l != NULL; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);

      self->volumes = g_list_prepend (self->volumes, g_object_ref (volume));

      if (added_volumes != NULL)
        *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
    }

  self->volumes = g_list_sort (self->volumes, (GCompareFunc) volume_compare);

  g_list_free (added);
  g_list_free (removed);
  g_list_free_full (new_volumes, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_all (GVfsGoaVolumeMonitor *self)
{
  GList *added_volumes;
  GList *removed_volumes;

  update_accounts (self, NULL, NULL);
  update_volumes (self, &added_volumes, &removed_volumes);

  object_list_emit (self, "volume-removed", "removed", removed_volumes);
  object_list_emit (self, "volume-added", NULL, added_volumes);

  g_list_free_full (added_volumes, g_object_unref);
  g_list_free_full (removed_volumes, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static GoaClient *
get_goa_client_sync (GError **error)
{
  static GoaClient *client = NULL;
  static GError *_error = NULL;
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      client = goa_client_new_sync (NULL, &_error);
      g_once_init_leave (&initialized, 1);
    }

  if (_error != NULL && error != NULL)
    *error = g_error_copy (_error);

  return client;
}

/* ---------------------------------------------------------------------------------------------------- */

static GList *
g_vfs_goa_volume_monitor_get_connected_drives (GVolumeMonitor *_self)
{
  return NULL;
}

static GMount *
g_vfs_goa_volume_monitor_get_mount_for_uuid (GVolumeMonitor *_self, const gchar *uuid)
{
  GMount *mount;
  GVolume *volume;

  mount = NULL;

  volume = g_volume_monitor_get_volume_for_uuid (_self, uuid);
  if (volume != NULL)
    mount = g_volume_get_mount (G_VOLUME (volume));

  return mount;
}

static GList *
g_vfs_goa_volume_monitor_get_mounts (GVolumeMonitor *_self)
{
  GVfsGoaVolumeMonitor *self = G_VFS_GOA_VOLUME_MONITOR (_self);
  GList *l;
  GList *mounts = NULL;

  for (l = self->volumes; l != NULL; l = l->next)
    {
      GMount *mount;
      GVolume *volume = G_VOLUME (l->data);

      mount = g_volume_get_mount (volume);
      if (mount != NULL)
        mounts = g_list_prepend (mounts, mount);
    }

  return mounts;
}

static GVolume *
g_vfs_goa_volume_monitor_get_volume_for_uuid (GVolumeMonitor *_self, const gchar *uuid)
{
  GVfsGoaVolumeMonitor *self = G_VFS_GOA_VOLUME_MONITOR (_self);
  GList *llink;
  GVolume *volume = NULL;

  llink = g_list_find_custom (self->volumes, uuid, (GCompareFunc) uuid_compare);
  if (llink != NULL)
    volume = G_VOLUME (g_object_ref (llink->data));

  return volume;
}

static GList *
g_vfs_goa_volume_monitor_get_volumes (GVolumeMonitor *_self)
{
  GVfsGoaVolumeMonitor *self = G_VFS_GOA_VOLUME_MONITOR (_self);

  return g_list_copy_deep (self->volumes, (GCopyFunc) g_object_ref, NULL);
}

static gboolean
g_vfs_goa_volume_monitor_is_supported (void)
{
  if (get_goa_client_sync (NULL) != NULL)
    return TRUE;
  return FALSE;
}

static void
g_vfs_goa_volume_monitor_dispose (GObject *_self)
{
  GVfsGoaVolumeMonitor *self = G_VFS_GOA_VOLUME_MONITOR (_self);

  g_list_free_full (self->accounts, g_object_unref);
  self->accounts = NULL;

  g_list_free_full (self->volumes, g_object_unref);
  self->volumes = NULL;

  g_clear_object (&self->client);

  G_OBJECT_CLASS (g_vfs_goa_volume_monitor_parent_class)->dispose (_self);
}

static void
g_vfs_goa_volume_monitor_class_init (GVfsGoaVolumeMonitorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);

  gobject_class->dispose = g_vfs_goa_volume_monitor_dispose;

  monitor_class->get_connected_drives = g_vfs_goa_volume_monitor_get_connected_drives;
  monitor_class->get_mount_for_uuid = g_vfs_goa_volume_monitor_get_mount_for_uuid;
  monitor_class->get_mounts = g_vfs_goa_volume_monitor_get_mounts;
  monitor_class->get_volume_for_uuid = g_vfs_goa_volume_monitor_get_volume_for_uuid;
  monitor_class->get_volumes = g_vfs_goa_volume_monitor_get_volumes;
  monitor_class->is_supported = g_vfs_goa_volume_monitor_is_supported;
}

static void
g_vfs_goa_volume_monitor_init (GVfsGoaVolumeMonitor *self)
{
  GError *error;

  error = NULL;
  self->client = get_goa_client_sync (&error);
  if (self->client == NULL)
    {
      g_warning ("Failed to connect to GOA: %s", error->message);
      g_error_free (error);
      return;
    }

  update_all (self);

  g_signal_connect_swapped (self->client, "account-added", G_CALLBACK (update_all), self);
  g_signal_connect_swapped (self->client, "account-changed", G_CALLBACK (update_all), self);
  g_signal_connect_swapped (self->client, "account-removed", G_CALLBACK (update_all), self);

  g_vfs_proxy_volume_monitor_daemon_set_always_call_mount (TRUE);
}

GVolumeMonitor *
g_vfs_goa_volume_monitor_new (void)
{
  return g_object_new (G_VFS_TYPE_GOA_VOLUME_MONITOR, NULL);
}
