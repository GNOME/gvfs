/*
 * gvfs/daemon/gvfsbackendafc.h
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@ucla.edu>
 */

#ifndef GVFSBACKENDAFC_H
#define GVFSBACKENDAFC_H

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_AFC   (g_vfs_backend_afc_get_type())
#define G_VFS_BACKEND_AFC(o) (G_TYPE_CHECK_INSTANCE_CAST((o), G_VFS_TYPE_BACKEND_AFC, GVfsBackendAfc))
#define G_VFS_BACKEND_AFC_CLASS(k) (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_AFC, GVfsBackendAfcClass))
#define G_VFS_IS_BACKEND_AFC(o) (G_TYPE_CHECK_INSTANCE_TYPE((o), G_VFS_TYPE_BACKEND_AFC))
#define G_VFS_IS_BACKEND_AFC_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE((k), G_VFS_TYPE_BACKEND_AFC))
#define G_VFS_BACKEND_AFC_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), G_VFS_TYPE_BACKEND_AFC, GVfsBackendAfcClass))

typedef struct _GVfsBackendAfc GVfsBackendAfc;
typedef struct _GVfsBackendAfcClass GVfsBackendAfcClass;

struct _GVfsBackendAfcClass {
    GVfsBackendClass parent_class;
};

GType g_vfs_backend_afc_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* GVFSBACKENDAFC_H */

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
