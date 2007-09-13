#ifndef __G_VFS_BACKEND_TEST_H__
#define __G_VFS_BACKEND_TEST_H__

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_TEST         (g_vfs_backend_test_get_type ())
#define G_VFS_BACKEND_TEST(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTest))
#define G_VFS_BACKEND_TEST_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTestClass))
#define G_VFS_IS_BACKEND_TEST(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_TEST))
#define G_VFS_IS_BACKEND_TEST_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_TEST))
#define G_VFS_BACKEND_TEST_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTestClass))

typedef struct _GVfsBackendTest        GVfsBackendTest;
typedef struct _GVfsBackendTestClass   GVfsBackendTestClass;

struct _GVfsBackendTest
{
  GVfsBackend parent_instance;
};

struct _GVfsBackendTestClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_test_get_type (void) G_GNUC_CONST;
  
GVfsBackendTest *g_vfs_backend_test_new (void);

G_END_DECLS

#endif /* __G_VFS_BACKEND_TEST_H__ */
