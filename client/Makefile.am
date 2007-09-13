NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/common \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DGVFS_MODULE_DIR=\"$(libdir)/gvfs/modules\"	\
	-DDBUS_API_SUBJECT_TO_CHANGE

module_flags = -export_dynamic -avoid-version -module -no-undefined
giomodulesdir = $(libdir)/gio/gvfs-modules

## Common to dynamic and static client libs

vfssources = \
	gdaemonvfs.c gdaemonvfs.h \
	gdaemonvolume.c gdaemonvolume.h \
	gdaemonvolumemonitor.c gdaemonvolumemonitor.h \
	gdaemonfile.c gdaemonfile.h \
	gvfsuriutils.c gvfsuriutils.h \
	gdaemonfileinputstream.c gdaemonfileinputstream.h \
	gdaemonfileoutputstream.c gdaemonfileoutputstream.h \
	gdaemonfileenumerator.c gdaemonfileenumerator.h \
	gvfsdaemondbus.c \
	$(NULL)

vfslibs = \
	$(top_builddir)/gio/libgio.la \
	$(top_builddir)/common/libgvfscommon.la \
	$(DBUS_LIBS) \
	$(GLIB_LIBS) \
	$(NULL)

## Dynamic client lib

giomodules_LTLIBRARIES = libgvfsdbus.la

libgvfsdbus_la_LDFLAGS = $(module_flags)
libgvfsdbus_la_SOURCES = $(vfssources)
libgvfsdbus_la_LIBADD  = $(vfslibs)

## Uri parser modules

libvfsmodulesdir = $(libdir)/gvfs/modules
libvfsmodules_LTLIBRARIES = libsmb.la

libsmb_la_LDFLAGS = $(module_flags)
libsmb_la_SOURCES = 	\
	smburi.c \
	$(NULL)

libsmb_la_LIBADD = \
	$(top_builddir)/gio/libgio.la	\
	$(top_builddir)/common/libgvfscommon.la	\
	$(GLIB_LIBS) \
	$(NULL)


if USE_FUSE

## Static client lib for FUSE daemon

noinst_LTLIBRARIES = libgvfsdbusfuse.la

libgvfsdbusfuse_la_SOURCES = $(vfssources)
libgvfsdbusfuse_la_LIBADD  = $(vfslibs)

## FUSE daemon

libexec_PROGRAMS = gvfs-fuse-daemon

gvfs_fuse_daemon_SOURCES = \
	gvfsfusedaemon.c

gvfs_fuse_daemon_INCLUDES = \
	-I$(top_srcdir) \
        -I$(top_srcdir)/common \
        $(GLIB_CFLAGS) $(DBUS_CFLAGS) $(FUSE_CFLAGS) \
        -DG_LOG_DOMAIN=\"GVFS-FUSE\" -DG_DISABLE_DEPRECATED \
        -DDBUS_API_SUBJECT_TO_CHANGE

gvfs_fuse_daemon_LDADD = \
	$(top_builddir)/gio/libgio.la \
	$(top_builddir)/common/libcommon.la \
	$(top_builddir)/client/libgvfsdbusfuse.la \
	$(GLIB_LIBS) $(DBUS_LIBS) $(FUSE_LIBS)

endif
