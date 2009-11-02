SUBDIRS = trashlib

NULL =

mountdir = $(datadir)/gvfs/mounts

INCLUDES =					\
	$(SAMBA_CFLAGS)                         \
	-I$(top_srcdir)				\
	-I$(top_srcdir)/common			\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) $(DBUS_CFLAGS) 		\
	$(OBEXFTP_CFLAGS) $(EXPAT_CFLAGS)	\
	$(KEYRING_CFLAGS)			\
	-DDBUS_API_SUBJECT_TO_CHANGE		\
	-DG_UDEV_API_IS_SUBJECT_TO_CHANGE	\
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
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = gvfs-daemon.service.in
service_DATA = gvfs-daemon.service

%.mount: %.mount.in ../config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

libexec_PROGRAMS=gvfsd gvfsd-sftp gvfsd-trash gvfsd-computer gvfsd-burn gvfsd-localtest gvfsd-ftp

mount_in_files = sftp.mount.in trash.mount.in computer.mount.in burn.mount.in localtest.mount.in
mount_DATA =  sftp.mount trash.mount computer.mount burn.mount localtest.mount

mount_in_files += http.mount.in dav.mount.in dav+sd.mount.in ftp.mount.in 
if HAVE_HTTP
mount_DATA += http.mount dav.mount ftp.mount
if HAVE_AVAHI
mount_DATA += dav+sd.mount
endif
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

mount_in_files += gphoto2.mount.in
if USE_GPHOTO2
mount_DATA += gphoto2.mount
libexec_PROGRAMS += gvfsd-gphoto2
endif

mount_in_files += network.mount.in
if USE_GCONF
mount_DATA += network.mount
libexec_PROGRAMS += gvfsd-network
endif

mount_in_files += obexftp.mount.in
if USE_OBEXFTP
mount_DATA += obexftp.mount
libexec_PROGRAMS += gvfsd-obexftp
BUILT_SOURCES = obexftp-marshal.c obexftp-marshal.h

obexftp-marshal.h: obexftp-marshal.list
	$(AM_V_GEN) glib-genmarshal $< --prefix=obexftp_marshal --header > $@

obexftp-marshal.c: obexftp-marshal.list
	$(AM_V_GEN) echo "#include \"obexftp-marshal.h\"" > $@ && glib-genmarshal $< --prefix=obexftp_marshal --body >> $@

endif

mount_in_files += dns-sd.mount.in
if HAVE_AVAHI
mount_DATA += dns-sd.mount
libexec_PROGRAMS += gvfsd-dnssd
endif

mount_in_files += archive.mount.in
if HAVE_ARCHIVE
mount_DATA += archive.mount
libexec_PROGRAMS += gvfsd-archive
endif

mount_in_files += afc.mount.in
if USE_AFC
mount_DATA += afc.mount
libexec_PROGRAMS += gvfsd-afc
endif

EXTRA_DIST = gvfs-daemon.service.in $(mount_in_files) obexftp-marshal.list

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
	gvfsjobstartmountable.c gvfsjobstartmountable.h \
	gvfsjobstopmountable.c gvfsjobstopmountable.h \
	gvfsjobpollmountable.c gvfsjobpollmountable.h \
	gvfsjobopenforread.c gvfsjobopenforread.h \
	gvfsjobopeniconforread.c gvfsjobopeniconforread.h \
	gvfsjobread.c gvfsjobread.h \
	gvfsjobseekread.c gvfsjobseekread.h \
	gvfsjobcloseread.c gvfsjobcloseread.h \
	gvfsjobopenforwrite.c gvfsjobopenforwrite.h \
	gvfsjobwrite.c gvfsjobwrite.h \
	gvfsjobseekwrite.c gvfsjobseekwrite.h \
	gvfsjobclosewrite.c gvfsjobclosewrite.h \
	gvfsjobqueryinfo.c gvfsjobqueryinfo.h \
	gvfsjobqueryinforead.c gvfsjobqueryinforead.h \
	gvfsjobqueryinfowrite.c gvfsjobqueryinfowrite.h \
	gvfsjobqueryfsinfo.c gvfsjobqueryfsinfo.h \
	gvfsjobenumerate.c gvfsjobenumerate.h \
	gvfsjobsetdisplayname.c gvfsjobsetdisplayname.h \
	gvfsjobtrash.c gvfsjobtrash.h \
	gvfsjobdelete.c gvfsjobdelete.h \
	gvfsjobcopy.c gvfsjobcopy.h \
	gvfsjobmove.c gvfsjobmove.h \
	gvfsjobpush.c gvfsjobpush.h \
	gvfsjobpull.c gvfsjobpull.h \
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
	gvfsbackendsmb.c gvfsbackendsmb.h libsmb-compat.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_smb_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmb.h \
	-DDEFAULT_BACKEND_TYPE=smb-share \
	-DMAX_JOB_THREADS=1 \
	$(GCONF_CFLAGS) \
	-DBACKEND_TYPES='"smb-share", G_VFS_TYPE_BACKEND_SMB,'

gvfsd_smb_LDADD = $(SAMBA_LIBS) $(GCONF_LIBS) $(libraries)

gvfsd_smb_browse_SOURCES = \
	gvfsbackendsmbbrowse.c gvfsbackendsmbbrowse.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_smb_browse_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendsmbbrowse.h \
	-DMOUNTABLE_DBUS_NAME=org.gtk.vfs.mountpoint.smb_browse \
	-DMAX_JOB_THREADS=1 \
	$(GCONF_CFLAGS) \
	-DBACKEND_TYPES='"smb-network", G_VFS_TYPE_BACKEND_SMB_BROWSE,"smb-server", G_VFS_TYPE_BACKEND_SMB_BROWSE,'

gvfsd_smb_browse_LDADD = $(SAMBA_LIBS)  $(GCONF_LIBS) $(libraries)

gvfsd_obexftp_SOURCES = \
	gvfsbackendobexftp.c gvfsbackendobexftp.h \
	obexftp-marshal.c obexftp-marshal.h \
	gvfsbackendobexftp-fl-parser.c gvfsbackendobexftp-fl-parser.h \
	gvfsbackendobexftp-cap-parser.c gvfsbackendobexftp-cap-parser.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c

gvfsd_obexftp_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendobexftp.h \
	-DDEFAULT_BACKEND_TYPE=obex \
	-DMAX_JOB_THREADS=1 \
	-DBACKEND_TYPES='"obex", G_VFS_TYPE_BACKEND_OBEXFTP,'
if USE_HAL
gvfsd_obexftp_CPPFLAGS += $(HAL_CFLAGS)
endif

gvfsd_obexftp_LDADD = $(OBEXFTP_LIBS) $(EXPAT_LIBS) $(libraries)
if USE_HAL
gvfsd_obexftp_LDADD += $(HAL_LIBS)
endif

gvfsd_ftp_SOURCES = \
	gvfsftpconnection.c gvfsftpconnection.h \
	gvfsftpdircache.c gvfsftpdircache.h \
	gvfsftpfile.c gvfsftpfile.h \
	gvfsftptask.c gvfsftptask.h \
	gvfsbackendftp.c gvfsbackendftp.h \
	ParseFTPList.c ParseFTPList.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_ftp_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendftp.h \
	-DDEFAULT_BACKEND_TYPE=ftp \
	-DMAX_JOB_THREADS=10 \
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
	-DBACKEND_TYPES='"trash", G_VFS_TYPE_BACKEND_TRASH,' \
	-Itrashlib

gvfsd_trash_LDADD = trashlib/libtrash.a $(libraries)

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

gvfsd_network_SOURCES = \
	gvfsbackendnetwork.c gvfsbackendnetwork.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_network_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendnetwork.h \
	-DDEFAULT_BACKEND_TYPE=network \
	-DMAX_JOB_THREADS=1 \
	$(GCONF_CFLAGS) \
	-DBACKEND_TYPES='"network", G_VFS_TYPE_BACKEND_NETWORK,'

gvfsd_network_LDADD = $(libraries) $(GCONF_LIBS)

gvfsd_dnssd_SOURCES = \
	gvfsbackenddnssd.c gvfsbackenddnssd.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_dnssd_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackenddnssd.h \
	-DDEFAULT_BACKEND_TYPE=dns-sd \
	-DMAX_JOB_THREADS=1 \
	$(AVAHI_CFLAGS) \
	-DBACKEND_TYPES='"dns-sd", G_VFS_TYPE_BACKEND_DNS_SD,'

gvfsd_dnssd_LDADD = $(libraries) $(AVAHI_LIBS) $(top_builddir)/common/libgvfscommon-dnssd.la


gvfsd_archive_SOURCES = \
	gvfsbackendarchive.c gvfsbackendarchive.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_archive_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendarchive.h \
	-DDEFAULT_BACKEND_TYPE=archive \
	-DMAX_JOB_THREADS=1 \
	$(ARCHIVE_CFLAGS) \
	-DBACKEND_TYPES='"archive", G_VFS_TYPE_BACKEND_ARCHIVE,'

gvfsd_archive_LDADD = $(libraries) $(ARCHIVE_LIBS)

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
	-DBACKEND_TYPES='"cdda", G_VFS_TYPE_BACKEND_CDDA,' \
	$(CDDA_CFLAGS)
if USE_GUDEV
gvfsd_cdda_CPPFLAGS +=	$(GUDEV_CFLAGS)
else
gvfsd_cdda_CPPFLAGS +=	$(HAL_CFLAGS)
endif

if USE_GUDEV
gvfsd_cdda_LDADD = $(libraries) $(CDDA_LIBS) $(GUDEV_LIBS)
else
gvfsd_cdda_LDADD = $(libraries) $(CDDA_LIBS) $(HAL_LIBS)
endif

gvfsd_gphoto2_SOURCES = \
	gvfsbackendgphoto2.c gvfsbackendgphoto2.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c 

gvfsd_gphoto2_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendgphoto2.h \
	-DDEFAULT_BACKEND_TYPE=gphoto2 \
	-DMAX_JOB_THREADS=1 \
	$(GPHOTO2_CFLAGS) \
	-DBACKEND_TYPES='"gphoto2", G_VFS_TYPE_BACKEND_GPHOTO2,'
if USE_GUDEV
gvfsd_gphoto2_CPPFLAGS += $(GUDEV_CFLAGS)
else
gvfsd_gphoto2_CPPFLAGS += $(HAL_CFLAGS)
endif

if USE_GUDEV
gvfsd_gphoto2_LDADD = $(libraries) $(GPHOTO2_LIBS) $(GUDEV_LIBS)
else
gvfsd_gphoto2_LDADD = $(libraries) $(GPHOTO2_LIBS) $(HAL_LIBS)
endif

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
	$(HTTP_CFLAGS)

if HAVE_AVAHI
gvfsd_dav_CPPFLAGS += -DBACKEND_TYPES='"dav", G_VFS_TYPE_BACKEND_DAV, "dav+sd", G_VFS_TYPE_BACKEND_DAV, "davs+sd", G_VFS_TYPE_BACKEND_DAV,'
else
gvfsd_dav_CPPFLAGS += -DBACKEND_TYPES='"dav", G_VFS_TYPE_BACKEND_DAV,'
endif

gvfsd_dav_LDADD = $(libraries) $(HTTP_LIBS)
if HAVE_AVAHI
gvfsd_dav_LDADD += $(top_builddir)/common/libgvfscommon-dnssd.la
endif

gvfsd_afc_SOURCES = \
	gvfsbackendafc.c gvfsbackendafc.h \
	daemon-main.c daemon-main.h \
	daemon-main-generic.c

gvfsd_afc_CPPFLAGS = \
	-DBACKEND_HEADER=gvfsbackendafc.h \
	-DDEFAULT_BACKEND_TYPE=afc \
	-DMAX_JOB_THREADS=1 \
	$(AFC_CFLAGS) \
	-DBACKEND_TYPES='"afc", G_VFS_TYPE_BACKEND_AFC,'

gvfsd_afc_LDADD = \
	$(libraries) \
	$(AFC_LIBS)

