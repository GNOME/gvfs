NULL =

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_builddir)			\
	$(GLIB_CFLAGS) 				\
	-DGVFS_LOCALEDIR=\""$(localedir)"\"	\
	-DG_DISABLE_DEPRECATED

libraries =					\
	$(GLIB_LIBS)

bin_PROGRAMS =					\
	gvfs-mount				\
	gvfs-cat				\
	gvfs-open				\
	gvfs-save				\
	gvfs-ls					\
	gvfs-tree				\
	gvfs-info				\
	gvfs-set-attribute			\
	gvfs-trash				\
	gvfs-rename                             \
	gvfs-rm					\
	gvfs-copy				\
	gvfs-move				\
	gvfs-monitor-file			\
	gvfs-monitor-dir			\
	gvfs-mkdir				\
	$(NULL)

bin_SCRIPTS =					\
	gvfs-less				\
	$(NULL)

if ENABLE_BASHCOMP
profiledir = $(BASHCOMP_DIR)
profile_DATA = gvfs-bash-completion.sh
endif

gvfs_cat_SOURCES = gvfs-cat.c
gvfs_cat_LDADD = $(libraries)

gvfs_open_SOURCES = gvfs-open.c
gvfs_open_LDADD = $(libraries)

gvfs_copy_SOURCES = gvfs-copy.c
gvfs_copy_LDADD = $(libraries)

gvfs_save_SOURCES = gvfs-save.c
gvfs_save_LDADD = $(libraries)

gvfs_info_SOURCES = gvfs-info.c
gvfs_info_LDADD = $(libraries)

gvfs_set_attribute_SOURCES = gvfs-set-attribute.c
gvfs_set_attribute_LDADD = $(libraries)

gvfs_trash_SOURCES = gvfs-trash.c
gvfs_trash_LDADD = $(libraries)

gvfs_rename_SOURCES = gvfs-rename.c
gvfs_rename_LDADD = $(libraries)

gvfs_rm_SOURCES = gvfs-rm.c
gvfs_rm_LDADD = $(libraries)

gvfs_ls_SOURCES = gvfs-ls.c
gvfs_ls_LDADD = $(libraries)

gvfs_tree_SOURCES = gvfs-tree.c
gvfs_tree_LDADD = $(libraries)

gvfs_move_SOURCES = gvfs-move.c
gvfs_move_LDADD = $(libraries)

gvfs_mount_SOURCES = gvfs-mount.c
gvfs_mount_LDADD = $(libraries)

gvfs_monitor_dir_SOURCES = gvfs-monitor-dir.c
gvfs_monitor_dir_LDADD = $(libraries)

gvfs_monitor_file_SOURCES = gvfs-monitor-file.c
gvfs_monitor_file_LDADD = $(libraries)

gvfs_mkdir_SOURCES = gvfs-mkdir.c
gvfs_mkdir_LDADD = $(libraries)

EXTRA_DIST = gvfs-less gvfs-bash-completion.sh
