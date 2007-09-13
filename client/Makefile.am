NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/common \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DGVFS_MODULE_DIR=\"$(libdir)/gvfs/modules\"	\
	-DDBUS_API_SUBJECT_TO_CHANGE

giomodule_flags = -export_dynamic -avoid-version -module -no-undefined
giomodulesdir = $(libdir)/gio/gvfs-modules

giomodules_LTLIBRARIES = libgvfsdbus.la

libgvfsdbus_la_LDFLAGS = $(module_flags)
libgvfsdbus_la_SOURCES = 	\
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

libgvfsdbus_la_LIBADD = \
	$(top_builddir)/gio/libgio.la	\
	$(top_builddir)/common/libgvfscommon.la	\
	$(DBUS_LIBS) 			\
	$(GLIB_LIBS)

libvfsomodule_flags = -export_dynamic -avoid-version -module -no-undefined
libvfsmodules_LTLIBRARIES = libsmb.la
libvfsmodulesdir = $(libdir)/gvfs/modules

libsmb_la_LDFLAGS = $(module_flags)
libsmb_la_SOURCES = 	\
	smburi.c \
	$(NULL)

libsmb_la_LIBADD = \
	$(top_builddir)/gio/libgio.la	\
	$(top_builddir)/common/libgvfscommon.la	\
	$(GLIB_LIBS)
