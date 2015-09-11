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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsjobdbus.h"

G_DEFINE_TYPE (GVfsJobDBus, g_vfs_job_dbus, G_VFS_TYPE_JOB)

/* TODO: Real P_() */
#define P_(_x) (_x)

enum {
  PROP_0,
  PROP_INVOCATION,
  PROP_OBJECT
};

static void send_reply                  (GVfsJob      *job);
static void g_vfs_job_dbus_get_property (GObject      *object,
					 guint         prop_id,
					 GValue       *value,
					 GParamSpec   *pspec);
static void g_vfs_job_dbus_set_property (GObject      *object,
					 guint         prop_id,
					 const GValue *value,
					 GParamSpec   *pspec);

static void
g_vfs_job_dbus_finalize (GObject *object)
{
  GVfsJobDBus *job;

  job = G_VFS_JOB_DBUS (object);

  g_clear_object (&job->invocation);
  g_clear_object (&job->object);
  
  if (G_OBJECT_CLASS (g_vfs_job_dbus_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_dbus_parent_class)->finalize) (object);
}

static void
g_vfs_job_dbus_class_init (GVfsJobDBusClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_dbus_finalize;
  gobject_class->set_property = g_vfs_job_dbus_set_property;
  gobject_class->get_property = g_vfs_job_dbus_get_property;

  job_class->send_reply = send_reply;

  g_object_class_install_property (gobject_class,
				   PROP_INVOCATION,
				   g_param_spec_pointer ("invocation",
							P_("VFS Backend"),
							P_("The implementation for this job operation."),
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT,
                                   g_param_spec_pointer ("object",
                                                        P_("VFS Backend"),
                                                        P_("The implementation for this job operation."),
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_job_dbus_init (GVfsJobDBus *job)
{
}

static void
g_vfs_job_dbus_set_property (GObject         *object,
			     guint            prop_id,
			     const GValue    *value,
			     GParamSpec      *pspec)
{
  GVfsJobDBus *job = G_VFS_JOB_DBUS (object);
  
  switch (prop_id)
    {
    case PROP_INVOCATION:
      job->invocation = g_object_ref (g_value_get_pointer (value));
      break;
    case PROP_OBJECT:
      job->object = g_object_ref (g_value_get_pointer (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_job_dbus_get_property (GObject    *object,
			     guint       prop_id,
			     GValue     *value,
			     GParamSpec *pspec)
{
  GVfsJobDBus *job = G_VFS_JOB_DBUS (object);

  switch (prop_id)
    {
    case PROP_INVOCATION:
      g_value_set_pointer (value, job->invocation);
      break;
    case PROP_OBJECT:
      g_value_set_pointer (value, job->object);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  GVfsJobDBusClass *class;

  g_debug ("send_reply(%p), failed=%d (%s)\n", job, job->failed,
           job->failed ? job->error->message : "");
  
  class = G_VFS_JOB_DBUS_GET_CLASS (job);
  
  if (job->failed)
    g_dbus_method_invocation_return_gerror (dbus_job->invocation, job->error);
  else
    class->create_reply (job, dbus_job->object, dbus_job->invocation);
 
  g_vfs_job_emit_finished (job);
}

gboolean
g_vfs_job_dbus_is_serial (GVfsJobDBus *job_dbus,
                          GDBusConnection *connection,
			  guint32 serial)
{
  GDBusMessage *message;
  GDBusConnection *message_connection;
  
  message = g_dbus_method_invocation_get_message (job_dbus->invocation);
  message_connection = g_dbus_method_invocation_get_connection (job_dbus->invocation);
  
  return message_connection == connection &&
      g_dbus_message_get_serial (message) == serial;
}
