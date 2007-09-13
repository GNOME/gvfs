NULL =

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) 		\
	-DDBUS_API_SUBJECT_TO_CHANGE		\
	-DG_DISABLE_DEPRECATED

libraries =						\
	$(top_builddir)/gvfs/libgvfs.la			\
	$(GLIB_LIBS) $(DBUS_LIBS)

noinst_PROGRAMS =				\
	gvfs-daemon				\
	$(NULL)

gvfs_daemon_SOURCES = \
	gvfsdaemon.c gvfsdaemon.h \
	gvfsdaemontest.c gvfsdaemontest.h \
	gvfsreadrequest.c gvfsreadrequest.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	dbus-gmain.h dbus-gmain.c \
	main.c

gvfs_daemon_LDADD = $(libraries)
