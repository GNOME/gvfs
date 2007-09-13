#ifndef __MOUNT_H__
#define __MOUNT_H__

#include <glib-object.h>
#include <gio/gmountoperation.h>
#include <gmountsource.h>

G_BEGIN_DECLS

typedef struct _Mountable Mountable;

typedef void (*MountCallback) (Mountable *mountable,
			       GError *error,
			       gpointer user_data);

void       mount_init             (void);
Mountable *lookup_mountable       (GMountSpec            *spec);
gboolean   mountable_is_automount (Mountable             *mountable);
void       mountable_mount        (Mountable             *mountable,
				   GMountSpec            *mount_spec,
				   GMountSource          *source,
				   gboolean               automount,
				   MountCallback          callback,
				   gpointer               user_data);

G_END_DECLS

#endif /* __MOUNT_H__ */

