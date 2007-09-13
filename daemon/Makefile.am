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
	gvfsbackend.c gvfsbackend.h \
	gvfsbackendtest.c gvfsbackendtest.h \
	gvfsreadstream.c gvfsreadstream.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	gvfsjob.c gvfsjob.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobread.c gvfsjobread.h \
	dbus-gmain.h dbus-gmain.c \
	main.c

gvfs_daemon_LDADD = $(libraries)
