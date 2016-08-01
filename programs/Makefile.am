NULL =

bin_in_files = program.in

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
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio mount|" $< > $@

gvfs-cat: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio cat|" $< > $@

gvfs-open: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio open|" $< > $@

gvfs-save: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio save|" $< > $@

gvfs-ls: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio list|" $< > $@

gvfs-tree: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio tree|" $< > $@

gvfs-info: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio info|" $< > $@

gvfs-set-attribute: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio set|" $< > $@

gvfs-trash: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio trash|" $< > $@

gvfs-rename: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio rename|" $< > $@

gvfs-rm: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio remove|" $< > $@

gvfs-copy: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio copy|" $< > $@

gvfs-move: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio move|" $< > $@

gvfs-monitor-file: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio monitor|" $< > $@

gvfs-monitor-dir: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio monitor|" $< > $@

gvfs-mkdir: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio mkdir|" $< > $@

gvfs-mime: deprecated.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@replacement\@|gio mime|" $< > $@

EXTRA_DIST = $(bin_in_files)
