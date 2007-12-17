
NULL =

module_flags = -export_dynamic -avoid-version -module -no-undefined -export-symbols-regex '^g_io_module_(load|unload)'

giomodules_LTLIBRARIES = libgiohal-volume-monitor.la
giomodulesdir = $(libdir)/gio/modules

BUILT_SOURCES =                                         \
	hal-marshal.h           hal-marshal.c

hal-marshal.h: hal-marshal.list
	glib-genmarshal $< --prefix=hal_marshal --header > $@

hal-marshal.c: hal-marshal.list
	echo "#include \"hal-marshal.h\"" > $@ && glib-genmarshal $< --prefix=hal_marshal --body >> $@


libgiohal_volume_monitor_la_SOURCES =				\
	hal-module.c					\
	hal-marshal.c		hal-marshal.h		\
	hal-device.c		hal-device.h		\
	hal-pool.c		hal-pool.h		\
	ghaldrive.c		ghaldrive.h		\
	ghalvolume.c		ghalvolume.h		\
	ghalmount.c		ghalmount.h		\
	ghalvolumemonitor.c	ghalvolumemonitor.h	\
	$(NULL)

libgiohal_volume_monitor_la_CFLAGS =				\
	-DG_LOG_DOMAIN=\"GVFS-Hal\"		\
	$(GLIB_CFLAGS)                          \
	$(HAL_CFLAGS)                           \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"	\
	-DG_DISABLE_DEPRECATED			\
	$(NULL)

libgiohal_volume_monitor_la_LDFLAGS = $(module_flags)
libgiohal_volume_monitor_la_LIBADD  = $(GLIB_LIBS) $(HAL_LIBS)

clean-local:
	rm -f *~ *.loT $(BUILT_SOURCES)

EXTRA_DIST = hal-marshal.list
