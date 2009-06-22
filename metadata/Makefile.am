NULL =

noinst_LTLIBRARIES=libmetadata.la

APPS = 	\
	$(NULL)

if HAVE_LIBXML
APPS += convert-nautilus-metadata
endif

noinst_PROGRAMS = $(APPS)


INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/gvfs \
	$(LIBXML_CFLAGS) $(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	$(UDEV_CFLAGS) \
	-DG_LOG_DOMAIN=\"GVFS\" -DG_DISABLE_DEPRECATED \
	-DDBUS_API_SUBJECT_TO_CHANGE

libmetadata_la_SOURCES = 		\
	metatree.c metatree.h		\
	metabuilder.c metabuilder.h 	\
	crc32.c crc32.h			\
	$(NULL)

libmetadata_la_LIBADD = $(GLIB_LIBS) $(UDEV_LIBS)

convert_nautilus_metadata_LDADD = libmetadata.la $(LIBXML_LIBS)
convert_nautilus_metadata_SOURCES = metadata-nautilus.c
