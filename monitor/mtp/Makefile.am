
NULL =

libexec_PROGRAMS = gvfs-mtp-volume-monitor

gvfs_mtp_volume_monitor_SOURCES =

gvfs_mtp_volume_monitor_SOURCES +=			\
        mtp-volume-monitor-daemon.c \
	gmtpvolume.c	gmtpvolume.h	\
	gmtpvolumemonitor.c	gmtpvolumemonitor.h	\
	$(NULL)

gvfs_mtp_volume_monitor_CFLAGS =		\
	-DG_LOG_DOMAIN=\"GVFS-MTP\"		\
	-I$(top_srcdir)/common                  \
	-I$(top_srcdir)/monitor/proxy           \
	$(GLIB_CFLAGS)                          \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"	\
	-DG_UDEV_API_IS_SUBJECT_TO_CHANGE	\
	$(NULL)

gvfs_mtp_volume_monitor_CFLAGS += $(GUDEV_CFLAGS)

gvfs_mtp_volume_monitor_LDFLAGS =	\
	$(NULL)

gvfs_mtp_volume_monitor_LDADD  =		     			      \
	$(GLIB_LIBS)                                 			      \
	$(top_builddir)/common/libgvfscommon.la 			      \
	$(top_builddir)/monitor/proxy/libgvfsproxyvolumemonitordaemon-noin.la \
	$(NULL)

gvfs_mtp_volume_monitor_LDADD += $(GUDEV_LIBS)


remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors
remote_volume_monitors_DATA = mtp.monitor

servicedir       = $(datadir)/dbus-1/services
service_in_files = org.gtk.Private.MTPVolumeMonitor.service.in
service_DATA     = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

clean-local:
	rm -f *~ *.loT $(service_DATA)

EXTRA_DIST = $(service_in_files) mtp.monitor
