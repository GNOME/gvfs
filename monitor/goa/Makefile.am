NULL =

gvfs_src_dir = $(top_srcdir)/@with_gvfs_source@

libexec_PROGRAMS = gvfs-goa-volume-monitor

gvfs_goa_volume_monitor_SOURCES = \
	goavolume.c 		goavolume.h \
	goavolumemonitor.c 	goavolumemonitor.h \
	goavolumemonitordaemon.c \
	$(NULL)

gvfs_goa_volume_monitor_CFLAGS = \
	-DG_LOG_DOMAIN=\"GVFS-GOA\" \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/monitor/proxy \
	$(GLIB_CFLAGS) \
	$(GOA_CFLAGS) \
	$(WARN_CFLAGS) \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\" \
	-DGVFS_LOCALEDIR=\"$(localedir)\" \
	$(NULL)

gvfs_goa_volume_monitor_LDADD = \
	$(GLIB_LIBS) \
	$(GOA_LIBS) \
	$(top_builddir)/common/libgvfscommon.la \
	$(top_builddir)/monitor/proxy/libgvfsproxyvolumemonitordaemon-noin.la \
	$(NULL)

remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors
remote_volume_monitors_DATA = goa.monitor

servicedir = $(datadir)/dbus-1/services
service_in_files = org.gtk.Private.GoaVolumeMonitor.service.in
service_DATA = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

clean-local:
	rm -f *~ *.loT $(BUILT_SOURCES) $(service_DATA)

DISTCLEANFILES = $(service_DATA)

EXTRA_DIST = $(service_in_files) goa.monitor
