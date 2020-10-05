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

#ifndef __G_VFS_DAEMON_UTILS_H__
#define __G_VFS_DAEMON_UTILS_H__

#include <glib-object.h>
#include <gmountsource.h>

G_BEGIN_DECLS

char *       g_error_to_daemon_reply              (GError           *error,
						   guint32           seq_nr,
						   gsize            *len_out);

void	     gvfs_file_info_populate_default	    (GFileInfo        *info,
						     const char       *name_string,
						     GFileType	      type);
char *	     gvfs_file_info_populate_names_as_local (GFileInfo        *info,
						     const char       *name_string);
void	     gvfs_file_info_populate_content_types  (GFileInfo        *info,
						     const char       *basename,
						     GFileType         type);

int          gvfs_seek_type_to_lseek                (GSeekType         type);

gboolean     gvfs_accept_certificate                (GMountSource *mount_source,
                                                     GTlsCertificate *certificate,
                                                     GTlsCertificateFlags errors);

gssize       gvfs_output_stream_splice              (GOutputStream *stream,
                                                     GInputStream *source,
                                                     GOutputStreamSpliceFlags flags,
                                                     goffset total_size,
                                                     GFileProgressCallback progress_callback,
                                                     gpointer progress_callback_data,
                                                     GCancellable *cancellable,
                                                     GError **error);

G_END_DECLS

#endif /* __G_VFS_DAEMON_UTILS_H__ */
