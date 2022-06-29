/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2012, 2013, 2014 Red Hat, Inc.
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
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "goavolume.h"

struct _GVfsGoaVolume
{
  GObject parent;
  GFile *root;
  GMount *mount;
  GoaObject *object;
  gchar *uuid;
  gchar *icon;
  gchar *symbolic_icon;
  gulong account_attention_needed_id;
};

struct _GVfsGoaVolumeClass
{
  GObjectClass parent_class;
};

enum
{
  PROP_0,
  PROP_ACCOUNT,
  PROP_UUID
};

static void g_vfs_goa_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED (GVfsGoaVolume, g_vfs_goa_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
                                               g_vfs_goa_volume_iface_init))

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GMountOperation *mount_operation;
  GMountOperation *mount_operation_orig;
  gchar *passwd;
} MountOp;

static void
mount_op_free (MountOp *data)
{
  g_clear_object (&data->mount_operation);
  g_free (data->passwd);

  if (data->mount_operation_orig != NULL)
    {
      g_signal_handlers_disconnect_by_data (data->mount_operation_orig, data);
      g_object_unref (data->mount_operation_orig);
    }

  g_slice_free (MountOp, data);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
account_attention_needed_cb (GObject *_object, GParamSpec *pspec, gpointer user_data)
{
  GoaAccount *account = GOA_ACCOUNT (_object);
  GVfsGoaVolume *self = user_data;

  if (goa_account_get_attention_needed (account))
    {
      if (self->mount != NULL)
        {
          g_mount_unmount_with_operation (self->mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
          g_clear_object (&self->mount);
        }
    }
}

/* ---------------------------------------------------------------------------------------------------- */

GType g_vfs_goa_mount_operation_get_type (void) G_GNUC_CONST;

typedef struct
{
  GMountOperation parent_instance;
} GVfsGoaMountOperation;

typedef struct
{
  GMountOperationClass parent_class;
} GVfsGoaMountOperationClass;

static GMountOperation *
g_vfs_goa_mount_operation_new (void)
{
  return G_MOUNT_OPERATION (g_object_new (g_vfs_goa_mount_operation_get_type (), NULL));
}

G_DEFINE_TYPE (GVfsGoaMountOperation, g_vfs_goa_mount_operation, G_TYPE_MOUNT_OPERATION)

static void
g_vfs_goa_mount_operation_init (GVfsGoaMountOperation *mount_operation)
{
}

static void
g_vfs_goa_mount_operation_ask_question (GMountOperation *op,
                                        const char *message,
                                        const char *choices[])
{
  /* This is needed to prevent G_MOUNT_OPERATION_UNHANDLED reply in idle. */
}

static void
g_vfs_goa_mount_operation_class_init (GVfsGoaMountOperationClass *klass)
{
  GMountOperationClass *mount_op_class;

  mount_op_class = G_MOUNT_OPERATION_CLASS (klass);
  mount_op_class->ask_question  = g_vfs_goa_mount_operation_ask_question;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
ask_question_reply_cb (GMountOperation      *op,
                       GMountOperationResult result,
                       gpointer              user_data)
{
  MountOp *data = g_task_get_task_data (user_data);

  g_mount_operation_set_choice (data->mount_operation,
                                g_mount_operation_get_choice (op));
  g_mount_operation_reply (data->mount_operation, result);
}

static void
mount_operation_ask_question_cb (GMountOperation *op,
                                 gchar           *message,
                                 GStrv            choices,
                                 gpointer         user_data)
{
  MountOp *data = g_task_get_task_data (user_data);

  if (data->mount_operation_orig != NULL)
    {
      g_signal_connect (data->mount_operation_orig,
                        "reply",
                        G_CALLBACK (ask_question_reply_cb),
                        user_data);
      g_signal_emit_by_name (data->mount_operation_orig,
                             "ask-question",
                             message,
                             choices);
    }
  else
    {
      g_mount_operation_reply (data->mount_operation,
                               G_MOUNT_OPERATION_UNHANDLED);
    }
}

static void
mount_operation_ask_password_cb (GMountOperation   *op,
                                 gchar             *message,
                                 gchar             *default_user,
                                 gchar             *default_domain,
                                 GAskPasswordFlags  flags,
                                 gpointer           user_data)
{
  MountOp *data = g_task_get_task_data (user_data);

  g_mount_operation_set_password (data->mount_operation, data->passwd);
  g_mount_operation_reply (data->mount_operation, G_MOUNT_OPERATION_HANDLED);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_unmounted_cb (GMount        *mount,
                    GVfsGoaVolume *self)
{
  /* If this assert fails, we're leaking a reference to mount */
  g_assert (self->mount == mount);

  g_clear_object (&self->mount);
}

static void
find_enclosing_mount_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GFile *root = G_FILE (source_object);
  GTask *task = G_TASK (user_data);
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (g_task_get_source_object (task));
  GError *error;

  error = NULL;
  g_clear_object (&self->mount);
  self->mount = g_file_find_enclosing_mount_finish (root, res, &error);
  if (self->mount == NULL)
    {
      g_task_return_error (task, error);
    }
  else
    {
      g_signal_connect (self->mount, "unmounted", G_CALLBACK (mount_unmounted_cb), self);
      g_task_return_boolean (task, TRUE);
    }

  g_object_unref (task);
}

static void
mount_enclosing_volume_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GFile *root = G_FILE (source_object);
  GTask *task = G_TASK (user_data);
  GError *error;

  error = NULL;
  if (!g_file_mount_enclosing_volume_finish (root, res, &error))
    {
      if (error->code != G_IO_ERROR_ALREADY_MOUNTED)
        {
          g_task_return_error (task, error);
          g_object_unref (task);
          return;
        }
      else
        {
          gchar *uri;

          uri = g_file_get_uri (root);
          g_warning ("Already mounted %s: %s", uri, error->message);
          g_free (uri);
          g_error_free (error);
        }
    }

  g_file_find_enclosing_mount_async (root,
                                     G_PRIORITY_DEFAULT,
                                     g_task_get_cancellable (task),
                                     find_enclosing_mount_cb,
                                     task);
}

static void
get_access_token_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GoaOAuth2Based *oauth2_based = GOA_OAUTH2_BASED (source_object);
  GTask *task = G_TASK (user_data);
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (g_task_get_source_object (task));
  GError *error;
  GoaAccount *account;
  GoaFiles *files;
  MountOp *data = g_task_get_task_data (task);

  error = NULL;
  if (!goa_oauth2_based_call_get_access_token_finish (oauth2_based, &data->passwd, NULL, res, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  account = goa_object_peek_account (self->object);
  files = goa_object_peek_files (self->object);
  if (files == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Failed to get org.gnome.OnlineAccounts.Files for %s"),
                               goa_account_get_id (account));
      g_object_unref (task);
      return;
    }

  g_mount_operation_set_username (data->mount_operation, goa_account_get_identity (account));
  g_file_mount_enclosing_volume (self->root,
                                 G_MOUNT_MOUNT_NONE,
                                 data->mount_operation,
                                 g_task_get_cancellable (task),
                                 mount_enclosing_volume_cb,
                                 task);
}

static void
get_password_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GoaPasswordBased *passwd_based = GOA_PASSWORD_BASED (source_object);
  GTask *task = G_TASK (user_data);
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (g_task_get_source_object (task));
  GError *error;
  GoaAccount *account;
  GoaFiles *files;
  MountOp *data = g_task_get_task_data (task);

  error = NULL;
  if (!goa_password_based_call_get_password_finish (passwd_based, &data->passwd, res, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  account = goa_object_peek_account (self->object);
  files = goa_object_peek_files (self->object);
  if (files == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Failed to get org.gnome.OnlineAccounts.Files for %s"),
                               goa_account_get_id (account));
      g_object_unref (task);
      return;
    }

  g_mount_operation_set_username (data->mount_operation, goa_account_get_identity (account));
  g_file_mount_enclosing_volume (self->root,
                                 G_MOUNT_MOUNT_NONE,
                                 data->mount_operation,
                                 g_task_get_cancellable (task),
                                 mount_enclosing_volume_cb,
                                 task);
}

static void
ensure_credentials_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GoaAccount *account = GOA_ACCOUNT (source_object);
  GTask *task = G_TASK (user_data);
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (g_task_get_source_object (task));
  GError *error;
  GoaOAuth2Based *oauth2_based;
  GoaPasswordBased *passwd_based;

  error = NULL;
  if (!goa_account_call_ensure_credentials_finish (account, NULL, res, &error))
    {
      if (error->domain == GOA_ERROR && error->code == GOA_ERROR_NOT_AUTHORIZED)
        {
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   _("Invalid credentials for %s"),
                                   goa_account_get_presentation_identity (account));
          g_error_free (error);
        }
      else
        g_task_return_error (task, error);

      g_object_unref (task);
      return;
    }

  oauth2_based = goa_object_peek_oauth2_based (self->object);
  if (oauth2_based != NULL)
    {
      goa_oauth2_based_call_get_access_token (oauth2_based,
                                              g_task_get_cancellable (task),
                                              get_access_token_cb,
                                              task);
      return;
    }

  passwd_based = goa_object_peek_password_based (self->object);
  if (passwd_based != NULL)
    {
      goa_password_based_call_get_password (passwd_based,
                                            "password",
                                            g_task_get_cancellable (task),
                                            get_password_cb,
                                            task);
      return;
    }

  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Unsupported authentication method for %s"),
                           goa_account_get_presentation_identity (account));
  g_object_unref (task);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
g_vfs_goa_volume_can_eject (GVolume *_self)
{
  return FALSE;
}

static gboolean
g_vfs_goa_volume_can_mount (GVolume *_self)
{
  return TRUE;
}

static char **
g_vfs_goa_volume_enumerate_identifiers (GVolume *_self)
{
  GPtrArray *res;

  res = g_ptr_array_new ();
  g_ptr_array_add (res, g_strdup (G_VOLUME_IDENTIFIER_KIND_CLASS));
  g_ptr_array_add (res, g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));
  g_ptr_array_add (res, NULL);

  return (gchar **) g_ptr_array_free (res, FALSE);
}

static GFile *
g_vfs_goa_volume_get_activation_root (GVolume *_self)
{
  return g_file_new_for_uri (G_VFS_GOA_VOLUME (_self)->uuid);
}

static GDrive *
g_vfs_goa_volume_get_drive (GVolume *_self)
{
  return NULL;
}

static GIcon *
g_vfs_goa_volume_get_icon (GVolume *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  return g_themed_icon_new_with_default_fallbacks (self->icon);
}

static char *
g_vfs_goa_volume_get_identifier (GVolume *_self, const gchar *kind)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);

  if (g_strcmp0 (kind, G_VOLUME_IDENTIFIER_KIND_CLASS) == 0)
    return g_strdup ("network");
  else if (g_strcmp0 (kind, G_VOLUME_IDENTIFIER_KIND_UUID) == 0)
    return g_strdup (self->uuid);

  return NULL;
}

static GMount *
g_vfs_goa_volume_get_mount (GVolume *_self)
{
  /* _self->mount is only used to unmount when we see
     AttentionNeeded, it should not be exported by the
     volume monitor, because we can't export a GDaemonMount
     on the bus, and it's already handled as a shadow mount
     anyway
  */

  return NULL;
}

static char *
g_vfs_goa_volume_get_name (GVolume *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  GoaAccount *account;

  account = goa_object_peek_account (self->object);
  return goa_account_dup_presentation_identity (account);
}

static GIcon *
g_vfs_goa_volume_get_symbolic_icon (GVolume *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  return g_themed_icon_new_with_default_fallbacks (self->symbolic_icon);
}

static char *
g_vfs_goa_volume_get_uuid (GVolume *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  return g_strdup (self->uuid);
}

static void
g_vfs_goa_volume_mount (GVolume             *_self,
                        GMountMountFlags     flags,
                        GMountOperation     *mount_operation_orig,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  MountOp *data;
  GTask *task;
  GoaAccount *account;

  data = g_slice_new0 (MountOp);
  if (mount_operation_orig != NULL)
    data->mount_operation_orig = g_object_ref (mount_operation_orig);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_vfs_goa_volume_mount);
  g_task_set_task_data (task, data, (GDestroyNotify) mount_op_free);

  /* We ignore the GMountOperation handed to us by the proxy volume
   * monitor because it is set up to emit MountOpAskPassword on
   * ask-password.
   */
  data->mount_operation = g_vfs_goa_mount_operation_new ();
  g_signal_connect (data->mount_operation, "ask-password", G_CALLBACK (mount_operation_ask_password_cb), task);
  g_signal_connect (data->mount_operation, "ask-question", G_CALLBACK (mount_operation_ask_question_cb), task);

  account = goa_object_peek_account (self->object);
  goa_account_call_ensure_credentials (account, cancellable, ensure_credentials_cb, task);
}

static gboolean
g_vfs_goa_volume_mount_finish (GVolume *_self, GAsyncResult *res, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (res, _self), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (res, g_vfs_goa_volume_mount), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
g_vfs_goa_volume_removed (GVolume *volume)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (volume);

  if (self->mount == NULL)
    return;

  g_mount_unmount_with_operation (self->mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL, NULL, NULL);
  g_clear_object (&self->mount);
}

static gboolean
g_vfs_goa_volume_should_automount (GVolume *volume)
{
  return FALSE;
}

static void
g_vfs_goa_volume_constructed (GObject *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);
  GoaAccount *account;

  G_OBJECT_CLASS (g_vfs_goa_volume_parent_class)->constructed (_self);

  self->root = g_file_new_for_uri (self->uuid);

  account = goa_object_peek_account (self->object);
  self->account_attention_needed_id = g_signal_connect (account,
                                                        "notify::attention-needed",
                                                        G_CALLBACK (account_attention_needed_cb),
                                                        self);
}

static void
g_vfs_goa_volume_dispose (GObject *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);

  if (self->account_attention_needed_id != 0)
    {
      GoaAccount *account;

      account = goa_object_peek_account (self->object);
      g_signal_handler_disconnect (account, self->account_attention_needed_id);
      self->account_attention_needed_id = 0;
    }

  g_clear_object (&self->root);
  g_clear_object (&self->mount);
  g_clear_object (&self->object);

  G_OBJECT_CLASS (g_vfs_goa_volume_parent_class)->dispose (_self);
}

static void
g_vfs_goa_volume_finalize (GObject *_self)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);

  g_free (self->uuid);
  g_free (self->icon);
  g_free (self->symbolic_icon);

  G_OBJECT_CLASS (g_vfs_goa_volume_parent_class)->finalize (_self);
}

static void
g_vfs_goa_volume_set_property (GObject *_self, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  GVfsGoaVolume *self = G_VFS_GOA_VOLUME (_self);

  switch (prop_id)
    {
    case PROP_ACCOUNT:
      self->object = g_value_dup_object (value);
      break;

    case PROP_UUID:
      self->uuid = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (_self, prop_id, pspec);
      break;
    }
}

static void
g_vfs_goa_volume_class_init (GVfsGoaVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = g_vfs_goa_volume_constructed;
  gobject_class->dispose = g_vfs_goa_volume_dispose;
  gobject_class->finalize = g_vfs_goa_volume_finalize;
  gobject_class->set_property = g_vfs_goa_volume_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_ACCOUNT,
                                   g_param_spec_object ("account",
                                                        "GoaObject object",
                                                        "The GOA account represented by the volume",
                                                        GOA_TYPE_OBJECT,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class,
                                   PROP_UUID,
                                   g_param_spec_string ("uuid",
                                                        "UUID",
                                                        "The UUID of the volume",
                                                        NULL,
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_WRITABLE));
}

static void
g_vfs_goa_volume_init (GVfsGoaVolume *self)
{
  self->icon = g_strdup ("network-server");
  self->symbolic_icon = g_strdup ("network-server-symbolic");
}

static void
g_vfs_goa_volume_iface_init (GVolumeIface *iface)
{
  iface->removed = g_vfs_goa_volume_removed;

  iface->can_eject = g_vfs_goa_volume_can_eject;
  iface->can_mount = g_vfs_goa_volume_can_mount;
  iface->enumerate_identifiers = g_vfs_goa_volume_enumerate_identifiers;
  iface->get_activation_root = g_vfs_goa_volume_get_activation_root;
  iface->get_drive = g_vfs_goa_volume_get_drive;
  iface->get_icon = g_vfs_goa_volume_get_icon;
  iface->get_identifier = g_vfs_goa_volume_get_identifier;
  iface->get_mount = g_vfs_goa_volume_get_mount;
  iface->get_name = g_vfs_goa_volume_get_name;
  iface->get_symbolic_icon = g_vfs_goa_volume_get_symbolic_icon;
  iface->get_uuid = g_vfs_goa_volume_get_uuid;
  iface->mount_fn = g_vfs_goa_volume_mount;
  iface->mount_finish = g_vfs_goa_volume_mount_finish;
  iface->should_automount = g_vfs_goa_volume_should_automount;
}

GVolume *
g_vfs_goa_volume_new (GoaObject *object, const gchar *uuid)
{
  return g_object_new (G_VFS_TYPE_GOA_VOLUME,
                       "account", object,
                       "uuid", uuid,
                       NULL);
}
