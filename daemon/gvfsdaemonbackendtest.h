#ifndef __G_VFS_DAEMON_BACKEND_TEST_H__
#define __G_VFS_DAEMON_BACKEND_TEST_H__

#include <gvfsdaemonbackend.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON_BACKEND_TEST         (g_vfs_daemon_backend_test_get_type ())
#define G_VFS_DAEMON_BACKEND_TEST(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_DAEMON_BACKEND_TEST, GVfsDaemonBackendTest))
#define G_VFS_DAEMON_BACKEND_TEST_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_DAEMON_BACKEND_TEST, GVfsDaemonBackendTestClass))
#define G_IS_VFS_DAEMON_BACKEND_TEST(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_DAEMON_BACKEND_TEST))
#define G_IS_VFS_DAEMON_BACKEND_TEST_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_DAEMON_BACKEND_TEST))
#define G_VFS_DAEMON_BACKEND_TEST_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_DAEMON_BACKEND_TEST, GVfsDaemonBackendTestClass))

typedef struct _GVfsDaemonBackendTest        GVfsDaemonBackendTest;
typedef struct _GVfsDaemonBackendTestClass   GVfsDaemonBackendTestClass;

struct _GVfsDaemonBackendTest
{
  GVfsDaemonBackend parent_instance;
};

struct _GVfsDaemonBackendTestClass
{
  GVfsDaemonBackendClass parent_class;
};

GType g_vfs_daemon_backend_test_get_type (void) G_GNUC_CONST;
  
GVfsDaemonBackendTest *g_vfs_daemon_backend_test_new (void);

G_END_DECLS

#endif /* __G_VFS_DAEMON_BACKEND_TEST_H__ */
