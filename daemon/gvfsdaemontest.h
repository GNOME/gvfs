#ifndef __G_VFS_DAEMON_TEST_H__
#define __G_VFS_DAEMON_TEST_H__

#include <gvfsdaemon.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON_TEST         (g_vfs_daemon_test_get_type ())
#define G_VFS_DAEMON_TEST(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_DAEMON_TEST, GVfsDaemonTest))
#define G_VFS_DAEMON_TEST_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_DAEMON_TEST, GVfsDaemonTestClass))
#define G_IS_VFS_DAEMON_TEST(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_DAEMON_TEST))
#define G_IS_VFS_DAEMON_TEST_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_DAEMON_TEST))
#define G_VFS_DAEMON_TEST_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_DAEMON_TEST, GVfsDaemonTestClass))

typedef struct _GVfsDaemonTest        GVfsDaemonTest;
typedef struct _GVfsDaemonTestClass   GVfsDaemonTestClass;

struct _GVfsDaemonTest
{
  GVfsDaemon parent_instance;
};

struct _GVfsDaemonTestClass
{
  GVfsDaemonClass parent_class;
};

GType g_vfs_daemon_test_get_type (void) G_GNUC_CONST;
  
GVfsDaemonTest *g_vfs_daemon_test_new (const char *mountpoint);

G_END_DECLS

#endif /* __G_VFS_DAEMON_TEST_H__ */
