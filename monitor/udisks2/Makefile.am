
NULL =

libexec_PROGRAMS = gvfs-udisks2-volume-monitor

gvfs_udisks2_volume_monitor_SOURCES =					\
	udisks2volumemonitordaemon.c					\
	gvfsudisks2volumemonitor.c	gvfsudisks2volumemonitor.h	\
	gvfsudisks2drive.c		gvfsudisks2drive.h		\
	gvfsudisks2volume.c		gvfsudisks2volume.h		\
	gvfsudisks2mount.c		gvfsudisks2mount.h		\
	gvfsudisks2utils.c		gvfsudisks2utils.h		\
	$(NULL)

gvfs_udisks2_volume_monitor_CFLAGS =		\
	-DG_LOG_DOMAIN=\"GVFS-UDisks2\"		\
	-I$(top_srcdir)/common                  \
	-I$(top_srcdir)/monitor/proxy           \
	$(GLIB_CFLAGS)                          \
	$(UDISKS2_CFLAGS)                       \
	$(GUDEV_CFLAGS)                         \
	$(LIBSYSTEMD_LOGIN_CFLAGS)		\
	$(KEYRING_CFLAGS)			\
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"	\
	-DG_DISABLE_DEPRECATED			\
	-DUDISKS_API_IS_SUBJECT_TO_CHANGE	\
	$(NULL)

gvfs_udisks2_volume_monitor_LDFLAGS =	\
	$(NULL)

gvfs_udisks2_volume_monitor_LDADD  =		     			      	\
	$(GLIB_LIBS)                                 			      	\
	$(UDISKS2_LIBS)                                  			\
	$(GUDEV_LIBS)                                  			      	\
	$(LIBSYSTEMD_LOGIN_LIBS)						\
	$(KEYRING_LIBS)								\
	$(top_builddir)/common/libgvfscommon.la 			      	\
	$(top_builddir)/common/libgvfscommon-monitor.la			      	\
	$(top_builddir)/monitor/proxy/libgvfsproxyvolumemonitordaemon-noin.la 	\
	$(NULL)

remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors
remote_volume_monitors_DATA = udisks2.monitor

servicedir       = $(datadir)/dbus-1/services
service_in_files = org.gtk.Private.UDisks2VolumeMonitor.service.in
service_DATA     = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

clean-local:
	rm -f *~ *.loT $(BUILT_SOURCES) $(service_DATA)

EXTRA_DIST = $(service_in_files) udisks2.monitor what-is-shown.txt
