NULL =

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) \
	-DG_DISABLE_DEPRECATED

libraries =						\
	$(top_builddir)/gio/libgio.la			\
	$(GLIB_LIBS)

bin_PROGRAMS =					\
	gvfs-cat				\
	gvfs-ls					\
	gvfs-info				\
	$(NULL)

gvfs_cat_SOURCES = gvfs-cat.c
gvfs_cat_LDADD = $(libraries)

gvfs_info_SOURCES = gvfs-info.c
gvfs_info_LDADD = $(libraries)

gvfs_ls_SOURCES = gvfs-ls.c
gvfs_ls_LDADD = $(libraries)
