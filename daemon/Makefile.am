NULL =

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) 		\
	-DDBUS_API_SUBJECT_TO_CHANGE		\
	-DG_DISABLE_DEPRECATED

noinst_LTLIBRARIES=libdaemon.la

libraries =						\
	$(top_builddir)/gio/libgio.la			\
	libdaemon.la					\
	$(GLIB_LIBS) $(DBUS_LIBS)


noinst_PROGRAMS =				\
	gvfs-daemon				\
	gvfs-daemon-test			\
	$(NULL)

libdaemon_la_SOURCES = \
	gvfsdaemon.c gvfsdaemon.h \
	gvfsbackend.c gvfsbackend.h \
	gvfsreadchannel.c gvfsreadchannel.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	gvfsjob.c gvfsjob.h \
	gvfsjobsource.c gvfsjobsource.h \
	gvfsjobdbus.c gvfsjobdbus.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobread.c gvfsjobread.h \
	gvfsjobseekread.c gvfsjobseekread.h \
	gvfsjobcloseread.c gvfsjobcloseread.h \
	gvfsjobgetinfo.c gvfsjobgetinfo.h \
	gvfsjobenumerate.c gvfsjobenumerate.h \
	gvfsdaemonprotocol.h gvfsdaemonprotocol.c \
	dbus-gmain.h dbus-gmain.c \
        $(NULL)

gvfs_daemon_SOURCES = \
	main.c

gvfs_daemon_LDADD = $(libraries)

gvfs_daemon_test_SOURCES = \
	gvfsbackendtest.c gvfsbackendtest.h \
	test.c

gvfs_daemon_test_LDADD = $(libraries)
