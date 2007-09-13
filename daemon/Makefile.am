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
	gvfsreadchannel.c gvfsreadchannel.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	gvfsjob.c gvfsjob.h \
	gvfsjobdbus.c gvfsjobdbus.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobread.c gvfsjobread.h \
	gvfsjobseekread.c gvfsjobseekread.h \
	gvfsjobcloseread.c gvfsjobcloseread.h \
	dbus-gmain.h dbus-gmain.c \
	main.c

gvfs_daemon_LDADD = $(libraries)
