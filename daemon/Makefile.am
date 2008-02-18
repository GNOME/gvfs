NULL =

mountdir = $(datadir)/gvfs/mounts

INCLUDES =					\
	$(SAMBA_CFLAGS)                         \
	-I$(top_srcdir)				\
	-I$(top_srcdir)/common			\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) 		\
	$(KEYRING_CFLAGS)			\
	-DDBUS_API_SUBJECT_TO_CHANGE		\
	-DLIBEXEC_DIR=\"$(libexecdir)/\" 	\
	-DMOUNTABLE_DIR=\"$(mountdir)/\" 	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"     \
	-DG_DISABLE_DEPRECATED

noinst_LTLIBRARIES=libdaemon.la

libraries =						\
	libdaemon.la					\
	$(top_builddir)/common/libgvfscommon.la		\
	$(GLIB_LIBS) $(DBUS_LIBS) $(KEYRING_LIBS)

# D-BUS service file
%.service: %.service.in ../config.log
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = gvfs-daemon.service.in
service_DATA = gvfs-daemon.service

%.mount: %.mount.in ../config.log
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

libexec_PROGRAMS=gvfsd gvfsd-ftp gvfsd-sftp gvfsd-trash gvfsd-computer gvfsd-burn gvfsd-localtest

mount_in_files = ftp.mount.in sftp.mount.in trash.mount.in computer.mount.in burn.mount.in localtest.mount.in
mount_DATA =  sftp.mount trash.mount computer.mount burn.mount localtest.mount
# Disabled until the implementation is working: ftp.mount

mount_in_files += http.mount.in dav.mount.in 
if HAVE_HTTP
mount_DATA += http.mount dav.mount  
libexec_PROGRAMS += gvfsd-http gvfsd-dav
endif

mount_in_files += smb.mount.in smb-browse.mount.in
if HAVE_SAMBA
mount_DATA += smb.mount smb-browse.mount
libexec_PROGRAMS += gvfsd-smb-browse gvfsd-smb
endif

mount_in_files += cdda.mount.in
if USE_CDDA
mount_DATA += cdda.mount
libexec_PROGRAMS += gvfsd-cdda
endif

EXTRA_DIST = gvfs-daemon.service.in $(mount_in_files)

DISTCLEANFILES = gvfs-daemon.service $(mount_DATA)


noinst_PROGRAMS =				\
	gvfsd-test			\
	$(NULL)

libdaemon_la_SOURCES = \
	gvfsdaemon.c gvfsdaemon.h \
	gvfsbackend.c gvfsbackend.h \
	gvfschannel.c gvfschannel.h \
	gvfsreadchannel.c gvfsreadchannel.h \
	gvfswritechannel.c gvfswritechannel.h \
	gvfsmonitor.c gvfsmonitor.h \
	gvfsdaemonutils.c gvfsdaemonutils.h \
	gvfsjob.c gvfsjob.h \
	gvfsjobsource.c gvfsjobsource.h \
	gvfsjobdbus.c gvfsjobdbus.h \
	gvfsjobmount.c gvfsjobmount.h \
	gvfsjobunmount.c gvfsjobunmount.h \
	gvfsjobmountmountable.c gvfsjobmountmountable.h \
	gvfsjobunmountmountable.c gvfsjobunmountmountable.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobread.c gvfsjobread.h \
	gvfsjobseekread.c gvfsjobseekread.h \
	gvfsjobcloseread.c gvfsjobcloseread.h \
	gvfsjobopenforwrite.c gvfsjobopenforwrite.h \
	gvfsjobwrite.c gvfsjobwrite.h \
	gvfsjobseekwrite.c gvfsjobseekwrite.h \
	gvfsjobclosewrite.c gvfsjobclosewrite.h \
	gvfsjobqueryinfo.c gvfsjobqueryinfo.h \
	gvfsjobqueryfsinfo.c gvfsjobqueryfsinfo.h \
	gvfsjobenumerate.c gvfsjobenumerate.h \
	gvfsjobsetdisplayname.c gvfsjobsetdisplayname.h \
	gvfsjobtrash.c gvfsjobtrash.h \
	gvfsjobdelete.c gvfsjobdelete.h \
	gvfsjobcopy.c gvfsjobcopy.h \
	gvfsjobupload.c gvfsjobupload.h \
	gvfsjobmove.c gvfsjobmove.h \
	gvfsjobmakedirectory.c gvfsjobmakedirectory.h \
	gvfsjobmakesymlink.c gvfsjobmakesymlink.h \
	gvfsjobsetattribute.c gvfsjobsetattribute.h \
	gvfsjobqueryattributes.c gvfsjobqueryattributes.h \
	gvfsjobcreatemonitor.c gvfsjobcreatemonitor.h \
	dbus-gmain.h dbus-gmain.c \
	gvfskeyring.h gvfskeyring.c \
        $(NULL)

gvfsd_SOURCES = \
	mount.c mount.h \
	main.c

gvfsd_LDADD = $(libraries)

gvfsd_test_SOURCES = \
	gvfsbackendtest.c gvfsbackendtest.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_test_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendtest.h \
	-DDEFAULT_BACKEND_TYPE=test \
	-DBACKEND_TYPES='"test", G_VFS_TYPE_BACKEND_TEST,'

gvfsd_test_LDADD = $(libraries)

gvfsd_localtest_SOURCES = \
	gvfsbackendlocaltest.c gvfsbackendlocaltest.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c

gvfsd_localtest_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendlocaltest.h \
	-DDEFAULT_BACKEND_TYPE=localtest \
	-DBACKEND_TYPES='"localtest", G_VFS_TYPE_BACKEND_LOCALTEST,'

gvfsd_localtest_LDADD = $(libraries)

gvfsd_smb_SOURCES = \
	gvfsbackendsmb.c gvfsbackendsmb.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_smb_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmb.h \
	-DDEFAULT_BACKEND_TYPE=smb-share \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"smb-share", G_VFS_TYPE_BACKEND_SMB,'

gvfsd_smb_LDADD = $(SAMBA_LIBS) $(libraries)

gvfsd_smb_browse_SOURCES = \
	gvfsbackendsmbbrowse.c gvfsbackendsmbbrowse.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_smb_browse_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmbbrowse.h \
	-DMOUNTABLE_DBUS_NAME=org.gtk.vfs.mountpoint.smb_browse \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"smb-network", G_VFS_TYPE_BACKEND_SMB_BROWSE,"smb-server", G_VFS_TYPE_BACKEND_SMB_BROWSE,'

gvfsd_smb_browse_LDADD = $(SAMBA_LIBS) $(libraries)

gvfsd_ftp_SOURCES = \
	gvfsbackendftp.c gvfsbackendftp.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_ftp_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendftp.h \
	-DDEFAULT_BACKEND_TYPE=ftp \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"ftp", G_VFS_TYPE_BACKEND_FTP,'

gvfsd_ftp_LDADD = $(libraries)

gvfsd_sftp_SOURCES = \
	sftp.h \
	gvfsbackendsftp.c gvfsbackendsftp.h \
	pty_open.c pty_open.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_sftp_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsftp.h \
	-DDEFAULT_BACKEND_TYPE=sftp \
	-DMAX_JOB_THREADS=1 \
	-DSSH_PROGRAM=\"$(SSH_PROGRAM)\"	\
	-DBACKEND_TYPES='"sftp", G_VFS_TYPE_BACKEND_SFTP,'

gvfsd_sftp_LDADD = $(libraries)

gvfsd_trash_SOURCES = \
	gvfsbackendtrash.c gvfsbackendtrash.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_trash_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendtrash.h \
	-DDEFAULT_BACKEND_TYPE=trash \
	-DMAX_JOB_THREADS=10 \
	-DBACKEND_TYPES='"trash", G_VFS_TYPE_BACKEND_TRASH,'

gvfsd_trash_LDADD = $(libraries)

gvfsd_computer_SOURCES = \
	gvfsbackendcomputer.c gvfsbackendcomputer.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_computer_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendcomputer.h \
	-DDEFAULT_BACKEND_TYPE=computer \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"computer", G_VFS_TYPE_BACKEND_COMPUTER,'

gvfsd_computer_LDADD = $(libraries)

gvfsd_burn_SOURCES = \
	gvfsbackendburn.c gvfsbackendburn.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_burn_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendburn.h \
	-DDEFAULT_BACKEND_TYPE=burn \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"burn", G_VFS_TYPE_BACKEND_BURN,'

gvfsd_burn_LDADD = $(libraries)

gvfsd_cdda_SOURCES = \
	gvfsbackendcdda.c gvfsbackendcdda.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_cdda_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendcdda.h \
	-DDEFAULT_BACKEND_TYPE=cdda \
	-DMAX_JOB_THREADS=1 \
	$(CDDA_CFLAGS) \
	-DBACKEND_TYPES='"cdda", G_VFS_TYPE_BACKEND_CDDA,'

gvfsd_cdda_LDADD = $(libraries) $(CDDA_LIBS)

gvfsd_http_SOURCES = \
	soup-input-stream.c soup-input-stream.h \
	soup-output-stream.c soup-output-stream.h \
	gvfsbackendhttp.c gvfsbackendhttp.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_http_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendhttp.h \
	-DMOUNTABLE_DBUS_NAME=org.gtk.vfs.mountpoint.http \
	-DDEFAULT_BACKEND_TYPE=http \
	-DMAX_JOB_THREADS=1 \
	$(HTTP_CFLAGS) \
	-DBACKEND_TYPES='"http", G_VFS_TYPE_BACKEND_HTTP,'

gvfsd_http_LDADD = $(libraries) $(HTTP_LIBS)

gvfsd_dav_SOURCES = \
	soup-input-stream.c soup-input-stream.h \
	soup-output-stream.c soup-output-stream.h \
	gvfsbackendhttp.c gvfsbackendhttp.h \
	gvfsbackenddav.c gvfsbackenddav.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_dav_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackenddav.h \
	-DDEFAULT_BACKEND_TYPE=dav \
	-DMAX_JOB_THREADS=1 \
	$(HTTP_CFLAGS) \
	-DBACKEND_TYPES='"dav", G_VFS_TYPE_BACKEND_DAV,'

gvfsd_dav_LDADD = $(libraries) $(HTTP_LIBS)

