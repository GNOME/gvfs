NULL =

INCLUDES =					\
	$(SAMBA_CFLAGS)                         \
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
	$(top_builddir)/common/libgvfscommon.la		\
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

EXTRA_DIST = gvfs-daemon.service.in

DISTCLEANFILES = gvfs-daemon.service

libexec_PROGRAMS=gvfs-daemon gvfs-daemon-ftp

mount_in_files = ftp.mount.in
mount_DATA = ftp.mount

if HAVE_SAMBA
mount_in_files += smb.mount.in smb-browse.mount.in
mount_DATA += smb.mount smb-browse.mount
libexec_PROGRAMS += gvfs-daemon-smb-browse gvfs-daemon-smb
endif

noinst_PROGRAMS =				\
	gvfs-daemon-test			\
	$(NULL)


libdaemon_la_SOURCES = \
	gvfsdaemon.c gvfsdaemon.h \
	gvfsbackend.c gvfsbackend.h \
	gvfschannel.c gvfschannel.h \
	gvfsreadchannel.c gvfsreadchannel.h \
	gvfswritechannel.c gvfswritechannel.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	gvfsjob.c gvfsjob.h \
	gvfsjobsource.c gvfsjobsource.h \
	gvfsjobdbus.c gvfsjobdbus.h \
	gvfsjobmount.c gvfsjobmount.h \
	gvfsjobmountmountable.c gvfsjobmountmountable.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobread.c gvfsjobread.h \
	gvfsjobseekread.c gvfsjobseekread.h \
	gvfsjobcloseread.c gvfsjobcloseread.h \
	gvfsjobopenforwrite.c gvfsjobopenforwrite.h \
	gvfsjobwrite.c gvfsjobwrite.h \
	gvfsjobseekwrite.c gvfsjobseekwrite.h \
	gvfsjobclosewrite.c gvfsjobclosewrite.h \
	gvfsjobgetinfo.c gvfsjobgetinfo.h \
	gvfsjobgetfsinfo.c gvfsjobgetfsinfo.h \
	gvfsjobenumerate.c gvfsjobenumerate.h \
	gvfsjobsetdisplayname.c gvfsjobsetdisplayname.h \
	gvfsjobtrash.c gvfsjobtrash.h \
	gvfsjobdelete.c gvfsjobdelete.h \
	gvfsjobcopy.c gvfsjobcopy.h \
	gvfsjobmove.c gvfsjobmove.h \
	gvfsjobmakedirectory.c gvfsjobmakedirectory.h \
	gvfsjobmakesymlink.c gvfsjobmakesymlink.h \
	gvfsjobsetattribute.c gvfsjobsetattribute.h \
	gvfsjobqueryattributes.c gvfsjobqueryattributes.h \
	dbus-gmain.h dbus-gmain.c \
        $(NULL)

gvfs_daemon_SOURCES = \
	mount.c mount.h \
	main.c

gvfs_daemon_LDADD = $(libraries)

gvfs_daemon_test_SOURCES = \
	gvfsbackendtest.c gvfsbackendtest.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfs_daemon_test_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendtest.h \
	-DDEFAULT_BACKEND_TYPE=test \
	-DBACKEND_TYPES='"test", G_VFS_TYPE_BACKEND_TEST,'

gvfs_daemon_test_LDADD = $(libraries)

gvfs_daemon_smb_SOURCES = \
	gvfsbackendsmb.c gvfsbackendsmb.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfs_daemon_smb_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmb.h \
	-DDEFAULT_BACKEND_TYPE=smb-share \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"smb-share", G_VFS_TYPE_BACKEND_SMB,'

gvfs_daemon_smb_LDADD = $(SAMBA_LIBS) $(libraries)

gvfs_daemon_smb_browse_SOURCES = \
	gvfsbackendsmbbrowse.c gvfsbackendsmbbrowse.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfs_daemon_smb_browse_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmbbrowse.h \
	-DMOUNTABLE_DBUS_NAME=org.gtk.vfs.mountpoint.smb_browse \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"smb-network", G_VFS_TYPE_BACKEND_SMB_BROWSE,"smb-server", G_VFS_TYPE_BACKEND_SMB_BROWSE,'

gvfs_daemon_smb_browse_LDADD = $(SAMBA_LIBS) $(libraries)

gvfs_daemon_ftp_SOURCES = \
	gvfsbackendftp.c gvfsbackendftp.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfs_daemon_ftp_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendftp.h \
	-DDEFAULT_BACKEND_TYPE=ftp \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"ftp", G_VFS_TYPE_BACKEND_FTP,'

gvfs_daemon_ftp_LDADD = $(SAMBA_LIBS) $(libraries)
