NULL =

gvfs_src_dir = $(top_srcdir)/@with_gvfs_source@

libexec_PROGRAMS = gvfs-afc-volume-monitor

gvfs_afc_volume_monitor_SOURCES = \
	afcvolume.c		afcvolume.h \
	afcvolumemonitor.c 	afcvolumemonitor.h \
	afcvolumemonitordaemon.c \
	$(NULL)

gvfs_afc_volume_monitor_CFLAGS = \
	-DG_LOG_DOMAIN=\"GVFS-AFC\" \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/monitor/proxy \
	$(GLIB_CFLAGS) \
	$(AFC_CFLAGS) \
	$(WARN_CFLAGS) \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\" \
	-DGVFS_LOCALEDIR=\"$(localedir)\" \
	$(NULL)

gvfs_afc_volume_monitor_LDADD = \
	$(GLIB_LIBS) \
	$(AFC_LIBS) \
	$(top_builddir)/common/libgvfscommon.la \
	$(top_builddir)/monitor/proxy/libgvfsproxyvolumemonitordaemon-noin.la \
	$(NULL)

remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors
remote_volume_monitors_DATA = afc.monitor

servicedir = $(datadir)/dbus-1/services
service_in_files = org.gtk.vfs.AfcVolumeMonitor.service.in
service_DATA = $(service_in_files:.service.in=.service)

systemd_user_in_files = gvfs-afc-volume-monitor.service.in
if USE_SYSTEMD_USER_UNITS
systemd_user_DATA = $(systemd_user_in_files:.service.in=.service)
endif

$(service_DATA): $(service_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

$(systemd_user_DATA): $(systemd_user_in_files) Makefile
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

CLEANFILES = \
	*~ \
	*.loT \
	$(BUILT_SOURCES) \
	$(service_DATA) \
	$(systemd_user_DATA) \
	$(NULL)

EXTRA_DIST = \
	meson.build \
	$(service_in_files) \
	$(systemd_user_in_files) \
	$(remote_volume_monitors_DATA) \
	$(NULL)
