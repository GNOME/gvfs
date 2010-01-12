
NULL =

module_flags = -export_dynamic -avoid-version -module -no-undefined -export-symbols-regex '^g_io_module_(load|unload|query)'

if USE_GCONF
giomodules_LTLIBRARIES = libgiogconf.la
endif

libgiogconf_la_SOURCES =				\
	gapplookupgconf.c gapplookupgconf.h		\
	gconf-module.c					\
	$(NULL)

libgiogconf_la_CFLAGS =		\
	-DG_LOG_DOMAIN=\"GVFS-GConf\"		\
	-I$(top_srcdir)/common                  \
	$(GLIB_CFLAGS)                          \
	$(GCONF_CFLAGS)                         \
	-DGIO_MODULE_DIR=\"$(GIO_MODULE_DIR)\"	\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"	\
	-DG_DISABLE_DEPRECATED			\
	$(NULL)

libgiogconf_la_LDFLAGS = 	\
	$(module_flags)         \
	$(NULL)

libgiogconf_la_LIBADD  =	     \
	$(GLIB_LIBS)                 \
	$(GCONF_LIBS)                \
	$(NULL)
