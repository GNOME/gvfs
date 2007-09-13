NULL =

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	-I$(top_srcdir)/common \
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) $(FUSE_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS-FUSE\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

libraries = \
	$(top_builddir)/gio/libgio.la \
	$(top_builddir)/common/libcommon.la \
	$(GLIB_LIBS) $(DBUS_LIBS) $(FUSE_LIBS)

libexec_PROGRAMS = gvfs-fuse-daemon

gvfs_fuse_daemon_SOURCES = \
	gvfsfusedaemon.c

gvfs_fuse_daemon_LDADD = $(libraries)
