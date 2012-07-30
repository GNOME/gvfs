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

#ifndef __G_VFS_JOB_H__
#define __G_VFS_JOB_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB         (g_vfs_job_get_type ())
#define G_VFS_JOB(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB, GVfsJob))
#define G_VFS_JOB_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB, GVfsJobClass))
#define G_VFS_IS_JOB(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB))
#define G_VFS_IS_JOB_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB))
#define G_VFS_JOB_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB, GVfsJobClass))

typedef struct _GVfsJob        GVfsJob;
typedef struct _GVfsJobPrivate GVfsJobPrivate;
typedef struct _GVfsJobClass   GVfsJobClass;

/* Defined here to avoid circular includes */
typedef struct _GVfsJobSource GVfsJobSource;

struct _GVfsJob
{
  GObject parent_instance;

  /* TODO: Move stuff to private */
  gpointer backend_data;
  GDestroyNotify backend_data_destroy;
  
  guint failed : 1;
  guint cancelled : 1;
  guint sent_reply : 1;
  guint finished : 1;
  GError *error;
  GCancellable *cancellable;
  
  GVfsJobPrivate *priv;
};

struct _GVfsJobClass
{
  GObjectClass parent_class;

  /* signals */
  void (*cancelled)  (GVfsJob *job);
  void (*send_reply) (GVfsJob *job);
  void (*new_source) (GVfsJob *job,
		      GVfsJobSource *job_source);
  void (*finished)   (GVfsJob *job);

  /* vtable */

  void     (*run)    (GVfsJob *job);
  gboolean (*try)    (GVfsJob *job);
};

GType g_vfs_job_get_type (void) G_GNUC_CONST;

void     g_vfs_job_set_backend_data  (GVfsJob     *job,
				      gpointer     backend_data,
				      GDestroyNotify destroy);
gboolean g_vfs_job_is_finished       (GVfsJob     *job);
gboolean g_vfs_job_is_cancelled      (GVfsJob     *job);
void     g_vfs_job_cancel            (GVfsJob     *job);
void     g_vfs_job_run               (GVfsJob     *job);
gboolean g_vfs_job_try               (GVfsJob     *job);
void     g_vfs_job_emit_finished     (GVfsJob     *job);
void     g_vfs_job_failed            (GVfsJob     *job,
				      GQuark       domain,
				      gint         code,
				      const gchar *format,
				      ...) G_GNUC_PRINTF (4, 5);
void     g_vfs_job_failed_literal    (GVfsJob     *job,
                                      GQuark       domain,
                                      gint         code,
                                      const gchar *message);
void     g_vfs_job_failed_from_error (GVfsJob     *job,
				      const GError*error);
void     g_vfs_job_failed_from_errno (GVfsJob     *job,
				      gint         errno_arg);
void     g_vfs_job_succeeded         (GVfsJob     *job);

G_END_DECLS

#endif /* __G_VFS_JOB_H__ */
