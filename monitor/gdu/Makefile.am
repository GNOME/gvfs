
NULL =

libexec_PROGRAMS = gvfs-gdu-volume-monitor


gvfs_gdu_volume_monitor_SOURCES =			\
	gdu-volume-monitor-daemon.c			\
	ggdudrive.c		ggdudrive.h		\
	ggduvolume.c		ggduvolume.h		\
	ggdumount.c		ggdumount.h		\
	ggduvolumemonitor.c	ggduvolumemonitor.h	\
	$(NULL)

gvfs_gdu_volume_monitor_CFLAGS =		\
	-DG_LOG_DOMAIN=\"GVFS-Gdu\"		\
	-I$(top_srcdir)/common                  \
	-I$(top_srcdir)/monitor/proxy           \
	$(GLIB_CFLAGS)                          \
	$(GDU_CFLAGS)                           \
	$(DBUS_CFLAGS)                          \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"	\
	-DG_DISABLE_DEPRECATED			\
	-DGDU_API_IS_SUBJECT_TO_CHANGE		\
	$(NULL)

gvfs_gdu_volume_monitor_LDFLAGS =	\
	$(NULL)

gvfs_gdu_volume_monitor_LDADD  =		     			      	\
	$(GLIB_LIBS)                                 			      	\
	$(GDU_LIBS)                                  			      	\
	$(DBUS_LIBS)                                 				\
	$(top_builddir)/common/libgvfscommon.la 			      	\
	$(top_builddir)/monitor/proxy/libgvfsproxyvolumemonitordaemon-noin.la 	\
	$(NULL)

remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors
remote_volume_monitors_DATA = gdu.monitor

servicedir       = $(datadir)/dbus-1/services
service_in_files = org.gtk.Private.GduVolumeMonitor.service.in
service_DATA     = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

clean-local:
	rm -f *~ *.loT $(BUILT_SOURCES) $(service_DATA)

EXTRA_DIST = $(service_in_files) gdu.monitor
