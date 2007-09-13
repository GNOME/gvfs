#ifndef __MOUNT_H__
#define __MOUNT_H__

#include <glib-object.h>
#include <gio/gmountoperation.h>
#include <gmountspec.h>

G_BEGIN_DECLS

typedef struct _Mountable Mountable;

void             mount_init             (void);
Mountable *      lookup_mountable       (GMountSpec  *spec);
gboolean         mountable_is_automount (Mountable   *mountable);
GMountOperation *mountable_mount        (Mountable   *mountable,
					 GMountSpec  *spec);

G_END_DECLS

#endif /* __MOUNT_H__ */

