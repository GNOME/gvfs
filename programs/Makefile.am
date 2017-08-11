NULL =

bin_SCRIPTS =					\
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
	gvfs-mime				\
	gvfs-less				\
	$(NULL)

gvfs-mount: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|mount|" $< > $@

gvfs-cat: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|cat|" $< > $@

gvfs-open: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|open|" $< > $@

gvfs-save: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|save|" $< > $@

gvfs-ls: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|list|" $< > $@

gvfs-tree: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|tree|" $< > $@

gvfs-info: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|info|" $< > $@

gvfs-set-attribute: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|set|" $< > $@

gvfs-trash: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|trash|" $< > $@

gvfs-rename: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|rename|" $< > $@

gvfs-rm: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|remove|" $< > $@

gvfs-copy: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|copy|" $< > $@

gvfs-move: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|move|" $< > $@

gvfs-monitor-file: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|monitor|" $< > $@

gvfs-monitor-dir: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|monitor|" $< > $@

gvfs-mkdir: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|mkdir|" $< > $@

gvfs-mime: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@command\@|mime|" $< > $@

EXTRA_DIST = \
	deprecated.in \
	gvfs-less \
	meson.build \
	$(NULL)

CLEANFILES = \
	gvfs-mount \
	gvfs-cat \
	gvfs-open \
	gvfs-save \
	gvfs-ls \
	gvfs-tree \
	gvfs-info \
	gvfs-set-attribute \
	gvfs-trash \
	gvfs-rename \
	gvfs-rm \
	gvfs-copy \
	gvfs-move \
	gvfs-monitor-file \
	gvfs-monitor-dir \
	gvfs-mkdir \
	gvfs-mime \
	$(NULL)
