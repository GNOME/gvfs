NULL =

noinst_LTLIBRARIES=libmetadata.la

APPS = 	\
	$(NULL)

if HAVE_LIBXML
APPS += convert-nautilus-metadata
endif

noinst_PROGRAMS = $(APPS)

libexec_PROGRAMS =\
	gvfsd-metadata	\
	$(NULL)

INCLUDES = -I$(top_srcdir) -I$(top_srcdir)/common \
	$(LIBXML_CFLAGS) $(GLIB_CFLAGS) $(DBUS_CFLAGS) \
	$(UDEV_CFLAGS) \
	-DGVFS_LOCALEDIR=\""$(localedir)"\"     \
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

gvfsd_metadata_LDADD = libmetadata.la $(DBUS_LIBS) ../common/libgvfscommon.la
gvfsd_metadata_SOURCES = meta-daemon.c

# D-BUS service file
%.service: %.service.in ../config.log
	sed -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = gvfs-metadata.service.in
service_DATA = gvfs-metadata.service
