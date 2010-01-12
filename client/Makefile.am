NULL =

INCLUDES = -I$(top_srcdir) \
	-I$(top_srcdir)/client \
	-I$(top_srcdir)/metadata \
	-I$(top_srcdir)/common \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DGVFS_MODULE_DIR=\"$(libdir)/gvfs/modules\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"     \
	-DDBUS_API_SUBJECT_TO_CHANGE

module_flags = -export_dynamic -avoid-version -module -no-undefined -export-symbols-regex '^g_vfs_.*|g_io_module_load|g_io_module_unload|g_io_module_query'

## Common to dynamic and static client libs

gvfsclientincludedir=$(includedir)/gvfs-client/gvfs/
gvfsclientinclude_HEADERS = \
	gvfsuriutils.h gvfsurimapper.h \
	$(NULL)

URI_PARSER_SOURCES = \
	smburi.c \
	httpuri.c \
	$(NULL)

vfssources = \
	gdaemonvfs.c gdaemonvfs.h \
	gdaemonmount.c gdaemonmount.h \
	gdaemonvolumemonitor.c gdaemonvolumemonitor.h \
	gdaemonfile.c gdaemonfile.h \
	gdaemonfileinputstream.c gdaemonfileinputstream.h \
	gdaemonfileoutputstream.c gdaemonfileoutputstream.h \
	gdaemonfileenumerator.c gdaemonfileenumerator.h \
	gdaemonfilemonitor.c gdaemonfilemonitor.h \
	gvfsdaemondbus.c gvfsdaemondbus.h \
	gvfsiconloadable.c gvfsiconloadable.h \
	gvfsuriutils.c gvfsuriutils.h \
	gvfsurimapper.c gvfsurimapper.h \
	$(URI_PARSER_SOURCES) \
	$(NULL)

vfslibs = \
	$(top_builddir)/common/libgvfscommon.la \
	$(DBUS_LIBS) \
	$(GLIB_LIBS) \
	$(NULL)

## Dynamic client lib

giomodules_LTLIBRARIES = libgvfsdbus.la

libgvfsdbus_la_LDFLAGS = $(module_flags)
libgvfsdbus_la_SOURCES = $(vfssources)
libgvfsdbus_la_LIBADD  = $(vfslibs) ../metadata/libmetadata.la

noinst_PROGRAMS = test-uri-utils

test_uri_utils_SOURCES = test-uri-utils.c gvfsuriutils.c gvfsuriutils.h
test_uri_utils_LDADD = $(vfslibs)
test_uri_utils_CFLAGS = $(INCLUDES)

if USE_FUSE

## FUSE daemon

libexec_PROGRAMS = gvfs-fuse-daemon

gvfs_fuse_daemon_SOURCES = \
	gvfsfusedaemon.c

gvfs_fuse_daemon_CFLAGS = \
	-I$(top_srcdir) \
        -I$(top_srcdir)/common \
        $(GLIB_CFLAGS) $(DBUS_CFLAGS) $(FUSE_CFLAGS) \
        -DG_LOG_DOMAIN=\"GVFS-FUSE\" -DG_DISABLE_DEPRECATED \
        -DDBUS_API_SUBJECT_TO_CHANGE

gvfs_fuse_daemon_LDADD = \
	$(top_builddir)/common/libgvfscommon.la \
	$(GLIB_LIBS) $(DBUS_LIBS) $(FUSE_LIBS)

endif
