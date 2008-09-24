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
#include "gvfsjobsource.h"

static void g_vfs_job_source_base_init (gpointer g_class);

enum {
  NEW_JOB,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

GType
g_vfs_job_source_get_type (void)
{
  static GType vfs_job_source_type = 0;

  if (! vfs_job_source_type)
    {
      static const GTypeInfo vfs_job_source_info =
      {
        sizeof (GVfsJobSourceIface), /* class_size */
	g_vfs_job_source_base_init,   /* base_init */
	NULL,		/* base_finalize */
	NULL,           /* class_init */
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      vfs_job_source_type =
	g_type_register_static (G_TYPE_INTERFACE, "GVfsJobSource",
				&vfs_job_source_info, 0);

      g_type_interface_add_prerequisite (vfs_job_source_type, G_TYPE_OBJECT);
    }

  return vfs_job_source_type;
}


static void
g_vfs_job_source_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;

  if (! initialized)
    {
      initialized = TRUE;

      signals[NEW_JOB] =
	g_signal_new ("new_job",
		      G_VFS_TYPE_JOB_SOURCE,
		      G_SIGNAL_RUN_LAST,
		      G_STRUCT_OFFSET (GVfsJobSourceIface, new_job),
		      NULL, NULL,
		      g_cclosure_marshal_VOID__OBJECT,
		      G_TYPE_NONE, 1, G_VFS_TYPE_JOB);
      
      signals[CLOSED] =
	g_signal_new ("closed",
		      G_VFS_TYPE_JOB_SOURCE,
		      G_SIGNAL_RUN_LAST,
		      G_STRUCT_OFFSET (GVfsJobSourceIface, closed),
		      NULL, NULL,
		      g_cclosure_marshal_VOID__VOID,
		      G_TYPE_NONE, 0);
    }      
}

void
g_vfs_job_source_new_job (GVfsJobSource *job_source,
			  GVfsJob       *job)
{
  g_signal_emit (job_source, signals[NEW_JOB], 0, job);
}

void
g_vfs_job_source_closed (GVfsJobSource *job_source)
{
  g_signal_emit (job_source, signals[CLOSED], 0);
}
