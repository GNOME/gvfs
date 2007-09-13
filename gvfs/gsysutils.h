#ifndef __G_SYS_UTILS_H__
#define __G_SYS_UTILS_H__

#include <glib.h>

G_BEGIN_DECLS

int _g_socket_receive_fd (int          socket_fd);
int _g_socket_connect    (const char  *address,
			  GError     **error);

G_END_DECLS


#endif /* __G_SYS_UTILS_H__ */
