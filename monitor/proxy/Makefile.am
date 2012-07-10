
NULL =

remote_volume_monitorsdir = $(datadir)/gvfs/remote-volume-monitors

module_flags = -export_dynamic -avoid-version -module -no-undefined -export-symbols-regex '^g_io_module_(load|unload|query)'

dbus_built_sources = gvfsvolumemonitordbus.c gvfsvolumemonitordbus.h
BUILT_SOURCES = $(dbus_built_sources)

$(dbus_built_sources) : Makefile.am dbus-interfaces.xml
	$(AM_V_GEN) gdbus-codegen									\
		--interface-prefix org.gtk.Private.					\
		--c-namespace GVfs							\
		--generate-c-code gvfsvolumemonitordbus					\
		$(srcdir)/dbus-interfaces.xml							\
		$(NULL)



giomodules_LTLIBRARIES = libgioremote-volume-monitor.la

libgioremote_volume_monitor_la_SOURCES =				\
	remote-volume-monitor-module.c					\
	gproxydrive.c			gproxydrive.h			\
	gproxyvolume.c			gproxyvolume.h			\
	gproxymount.c			gproxymount.h			\
	gproxyshadowmount.c		gproxyshadowmount.h		\
	gproxyvolumemonitor.c		gproxyvolumemonitor.h		\
	gproxymountoperation.c		gproxymountoperation.h		\
	$(dbus_built_sources)						\
	$(NULL)

libgioremote_volume_monitor_la_CFLAGS =					\
	-DG_LOG_DOMAIN=\"GVFS-RemoteVolumeMonitor\"			\
	-I$(top_srcdir)/common                  			\
	$(GLIB_CFLAGS)                          			\
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"				\
	-DREMOTE_VOLUME_MONITORS_DIR=\"$(remote_volume_monitorsdir)\" 	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"				\
	$(NULL)

libgioremote_volume_monitor_la_LDFLAGS = 				\
	$(module_flags)               					\
	$(NULL)

libgioremote_volume_monitor_la_LIBADD  =		     		\
	$(GLIB_LIBS)                                 			\
	$(top_builddir)/common/libgvfscommon.la 			\
	$(NULL)

############################################################################

noinst_LTLIBRARIES = libgvfsproxyvolumemonitordaemon-noin.la

libgvfsproxyvolumemonitordaemon_noin_la_SOURCES =		\
	gvfsproxyvolumemonitordaemon.c				\
	gvfsproxyvolumemonitordaemon.h				\
	$(dbus_built_sources)					\
	$(NULL)

libgvfsproxyvolumemonitordaemon_noin_la_CFLAGS =		\
	-I$(top_srcdir)/common                  		\
	$(GLIB_CFLAGS)                          		\
	$(GDU_CFLAGS)                           		\
	-DG_LOG_DOMAIN=\"GVFS-RemoteVolumeMonitorDaemon\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"			\
	$(NULL)

libgvfsproxyvolumemonitordaemon_noin_la_LIBADD  =     		\
	$(GLIB_LIBS)                                 		\
	$(top_builddir)/common/libgvfscommon.la 		\
	$(NULL)

clean-local:
	rm -f *~ *.loT

install-data-local:
	mkdir -p $(DESTDIR)$(remote_volume_monitorsdir)

EXTRA_DIST =							\
	dbus-interfaces.xml					\
	$(NULL)

CLEANFILES =							\
	$(dbus_built_sources)					\
	$(NULL)
