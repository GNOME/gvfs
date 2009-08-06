/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#ifndef __G_VFS_FTP_TASK_H__
#define __G_VFS_FTP_TASK_H__

#include "gvfsbackendftp.h"
#include "gvfsftpconnection.h"

G_BEGIN_DECLS

typedef enum {
  G_VFS_FTP_PASS_100 = (1 << 0),
  G_VFS_FTP_PASS_300 = (1 << 1),
  G_VFS_FTP_PASS_500 = (1 << 2),
  G_VFS_FTP_PASS_550 = (1 << 3),
  G_VFS_FTP_FAIL_200 = (1 << 4)
} GVfsFtpResponseFlags;

#define G_VFS_FTP_RESPONSE_GROUP(response) ((response) / 100)

typedef struct _GVfsFtpTask GVfsFtpTask;
struct _GVfsFtpTask
{
  GVfsBackendFtp *      backend;        /* backend this task is running on */
  GVfsJob *             job;            /* job that is processed or NULL if not bound to a task */
  GCancellable *        cancellable;    /* cancellable in use */

  GError *              error;          /* NULL or current error - will be propagated to task */
  GVfsFtpConnection *   conn;           /* connection in use by this task or NULL if none */
  GVfsFtpMethod         method;         /* method currently in use (only valid after call to _setup_data_connection() */
};

typedef void (* GVfsFtpErrorFunc) (GVfsFtpTask *task, gpointer data);

#define G_VFS_FTP_TASK_INIT(backend,job) { (backend), (job), (job)->cancellable, }
void                    g_vfs_ftp_task_done                     (GVfsFtpTask *          task);

#define g_vfs_ftp_task_is_in_error(task) ((task)->error != NULL)
#define g_vfs_ftp_task_error_matches(task, domain, code) (g_error_matches ((task)->error, (domain), (code)))
#define g_vfs_ftp_task_clear_error(task) (g_clear_error (&(task)->error))
void                    g_vfs_ftp_task_set_error_from_response  (GVfsFtpTask *          task,
                                                                 guint                  response);

void                    g_vfs_ftp_task_give_connection          (GVfsFtpTask *          task,
                                                                 GVfsFtpConnection *    conn);
GVfsFtpConnection *     g_vfs_ftp_task_take_connection          (GVfsFtpTask *          task);

guint                   g_vfs_ftp_task_send                     (GVfsFtpTask *          task,
                                                                 GVfsFtpResponseFlags   flags,
                                                                 const char *           format,
                                                                 ...) G_GNUC_PRINTF (3, 4);
guint                   g_vfs_ftp_task_send_and_check           (GVfsFtpTask *          task,
                                                                 GVfsFtpResponseFlags   flags,
                                                                 const GVfsFtpErrorFunc *funcs,
                                                                 gpointer               data,
                                                                 char ***               reply,
                                                                 const char *           format,
                                                                 ...) G_GNUC_PRINTF (6, 7);
guint                   g_vfs_ftp_task_sendv                    (GVfsFtpTask *          task,
                                                                 GVfsFtpResponseFlags   flags,
                                                                 char ***               reply,
                                                                 const char *           format,
        	                                                 va_list                varargs);
guint                   g_vfs_ftp_task_receive                  (GVfsFtpTask *          task,
                                                                 GVfsFtpResponseFlags   flags,
                                                                 char ***               reply);
void                    g_vfs_ftp_task_setup_data_connection    (GVfsFtpTask *          task);
void                    g_vfs_ftp_task_open_data_connection     (GVfsFtpTask *          task);
void                    g_vfs_ftp_task_close_data_connection    (GVfsFtpTask *          task);

gboolean                g_vfs_ftp_task_login                    (GVfsFtpTask *          task,
                                                                 const char *           username,
                                                                 const char *           password);
void                    g_vfs_ftp_task_setup_connection         (GVfsFtpTask *          task);


G_END_DECLS

#endif /* __G_VFS_FTP_TASK_H__ */
