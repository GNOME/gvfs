NULL =

bashcompdir = $(BASHCOMP_DIR)
bashcomp_DATA = gvfs-cat

EXTRA_DIST = $(bashcomp_DATA)

.PHONY = install-symlinks uninstall-symlinks

symlinks =					\
	gvfs-copy				\
	gvfs-info				\
	gvfs-less				\
	gvfs-ls					\
	gvfs-mkdir				\
	gvfs-monitor-dir			\
	gvfs-monitor-file			\
	gvfs-mount                              \
	gvfs-move				\
	gvfs-open				\
	gvfs-rename				\
	gvfs-rm					\
	gvfs-save				\
	gvfs-set-attribute			\
	gvfs-trash				\
	gvfs-tree				\
	$(NULL)

install-symlinks:
	set -e; for file in $(symlinks); do \
		$(LN_S) -f gvfs-cat $(DESTDIR)$(BASHCOMP_DIR)/$$file; \
	done

uninstall-symlinks:
	set -e; for file in $(symlinks); do \
		rm -f $(DESTDIR)$(BASHCOMP_DIR)/$$file; \
	done

install-data-hook: install-symlinks

uninstall-local: uninstall-symlinks
