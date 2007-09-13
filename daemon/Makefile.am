NULL =

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_srcdir)/common			\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) 		\
	-DDBUS_API_SUBJECT_TO_CHANGE		\
	-DMOUNTABLE_DIR=\"$(sysconfdir)/gvfs/mounts/\" \
	-DG_DISABLE_DEPRECATED

noinst_LTLIBRARIES=libdaemon.la

libraries =						\
	$(top_builddir)/gio/libgio.la			\
	libdaemon.la					\
	$(top_builddir)/common/libcommon.la		\
	$(GLIB_LIBS) $(DBUS_LIBS)

# D-BUS service file
%.service: %.service.in ../config.log
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = gvfs-daemon.service.in
service_DATA = gvfs-daemon.service

%.mount: %.mount.in ../config.log
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

mountdir = $(sysconfdir)/gvfs/mounts
mount_in_files = smb.mount.in
mount_DATA = smb.mount

EXTRA_DIST = gvfs-daemon.service.in

DISTCLEANFILES = gvfs-daemon.service

libexec_PROGRAMS=gvfs-daemon gvfs-daemon-smb

noinst_PROGRAMS =				\
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
	gvfsdaemonprotocol.c \
	dbus-gmain.h dbus-gmain.c \
        $(NULL)

gvfs_daemon_SOURCES = \
	mounttracker.c mounttracker.h \
	mount.c mount.h \
	main.c

gvfs_daemon_LDADD = $(libraries)

gvfs_daemon_test_SOURCES = \
	gvfsbackendtest.c gvfsbackendtest.h \
	test.c

gvfs_daemon_test_LDADD = $(libraries)

gvfs_daemon_smb_SOURCES = \
	gvfsbackendsmb.c gvfsbackendsmb.h \
	smb.c

gvfs_daemon_smb_LDADD = $(libraries) $(SAMBA_LIBS)
