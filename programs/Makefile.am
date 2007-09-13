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
	gvfs-mount				\
	gvfs-cat				\
	gvfs-create				\
	gvfs-ls					\
	gvfs-info				\
	gvfs-copy				\
	gvfs-move				\
	$(NULL)

gvfs_cat_SOURCES = gvfs-cat.c
gvfs_cat_LDADD = $(libraries)

gvfs_copy_SOURCES = gvfs-copy.c
gvfs_copy_LDADD = $(libraries)

gvfs_create_SOURCES = gvfs-create.c
gvfs_create_LDADD = $(libraries)

gvfs_info_SOURCES = gvfs-info.c
gvfs_info_LDADD = $(libraries)

gvfs_ls_SOURCES = gvfs-ls.c
gvfs_ls_LDADD = $(libraries)

gvfs_move_SOURCES = gvfs-move.c
gvfs_move_LDADD = $(libraries)

gvfs_mount_SOURCES = gvfs-mount.c
gvfs_mount_LDADD = $(libraries)
