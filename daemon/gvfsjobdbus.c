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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobdbus.h"
#include "gvfsdbusutils.h"

G_DEFINE_TYPE (GVfsJobDBus, g_vfs_job_dbus, G_VFS_TYPE_JOB)

/* TODO: Real P_() */
#define P_(_x) (_x)

enum {
  PROP_0,
  PROP_MESSAGE,
  PROP_CONNECTION
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

  if (job->message)
    dbus_message_unref (job->message);

  if (job->connection)
    dbus_connection_unref (job->connection);
  
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
				   PROP_CONNECTION,
				   g_param_spec_pointer ("connection",
							P_("VFS Backend"),
							P_("The implementation for this job operartion."),
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
				   PROP_MESSAGE,
				   g_param_spec_pointer ("message",
							P_("VFS Backend"),
							P_("The implementation for this job operartion."),
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
    case PROP_MESSAGE:
      job->message = dbus_message_ref (g_value_get_pointer (value));
      break;
    case PROP_CONNECTION:
      job->connection = dbus_connection_ref (g_value_get_pointer (value));
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
    case PROP_MESSAGE:
      g_value_set_pointer (value, job->message);
      break;
    case PROP_CONNECTION:
      g_value_set_pointer (value, job->connection);
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
  DBusMessage *reply;
  GVfsJobDBusClass *class;

  g_debug ("send_reply(%p), failed=%d (%s)\n", job, job->failed, job->failed?job->error->message:"");
  
  class = G_VFS_JOB_DBUS_GET_CLASS (job);
  
  if (job->failed) 
    reply = _dbus_message_new_from_gerror (dbus_job->message, job->error);
  else
    reply = class->create_reply (job, dbus_job->connection, dbus_job->message);
 
  g_assert (reply != NULL);

  /* Queues reply (threadsafely), actually sends it in mainloop */
  dbus_connection_send (dbus_job->connection, reply, NULL);
  dbus_message_unref (reply);

  g_vfs_job_emit_finished (job);
}

DBusConnection *
g_vfs_job_dbus_get_connection (GVfsJobDBus *job_dbus)
{
  return job_dbus->connection;
}

DBusMessage *
g_vfs_job_dbus_get_message (GVfsJobDBus *job_dbus)
{
  return job_dbus->message;
}

gboolean
g_vfs_job_dbus_is_serial (GVfsJobDBus *job_dbus,
			  DBusConnection *connection,
			  dbus_uint32_t serial)
{
  return job_dbus->connection == connection &&
    dbus_message_get_serial (job_dbus->message) == serial;
}
