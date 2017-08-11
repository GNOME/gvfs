XSLTPROC_FLAGS = \
	--nonet \
	--stringparam man.output.quietly 1 \
	--stringparam funcsynopsis.style ansi \
	--stringparam man.th.extra1.suppress 1 \
	--stringparam man.authors.section.enabled 0 \
	--stringparam man.copyright.section.enabled 0

%.1: %.xml
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

%.7: %.xml
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

gvfs-mount.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-mount|" -e "s|\@replacement\@|gio mount|" $< > $@

gvfs-cat.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-cat|" -e "s|\@replacement\@|gio cat|" $< > $@

gvfs-open.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-open|" -e "s|\@replacement\@|gio open|" $< > $@

gvfs-save.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-save|" -e "s|\@replacement\@|gio save|" $< > $@

gvfs-ls.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-ls|" -e "s|\@replacement\@|gio list|" $< > $@

gvfs-tree.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-tree|" -e "s|\@replacement\@|gio tree|" $< > $@

gvfs-info.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-info|" -e "s|\@replacement\@|gio info|" $< > $@

gvfs-set-attribute.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-set-attribute|" -e "s|\@replacement\@|gio set|" $< > $@

gvfs-trash.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-trash|" -e "s|\@replacement\@|gio trash|" $< > $@

gvfs-rename.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-rename|" -e "s|\@replacement\@|gio rename|" $< > $@

gvfs-rm.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-rm|" -e "s|\@replacement\@|gio remove|" $< > $@

gvfs-copy.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-copy|" -e "s|\@replacement\@|gio copy|" $< > $@

gvfs-move.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-move|" -e "s|\@replacement\@|gio move|" $< > $@

gvfs-monitor-file.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-monitor-file|" -e "s|\@replacement\@|gio monitor|" $< > $@

gvfs-monitor-dir.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-monitor-dir|" -e "s|\@replacement\@|gio monitor|" $< > $@

gvfs-mkdir.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-mkdir|" -e "s|\@replacement\@|gio mkdir|" $< > $@

gvfs-mime.xml: deprecated.xml.in Makefile
	$(AM_V_GEN) $(SED) -e "s|\@original\@|gvfs-mime|" -e "s|\@replacement\@|gio mime|" $< > $@

man_MANS = \
	gvfs-cat.1 \
	gvfs-copy.1 \
	gvfs-info.1 \
	gvfs-ls.1 \
	gvfs-less.1 \
	gvfs-mime.1 \
	gvfs-mkdir.1 \
	gvfs-monitor-dir.1 \
	gvfs-monitor-file.1 \
	gvfs-mount.1 \
	gvfs-move.1 \
	gvfs-open.1 \
	gvfs-rename.1 \
	gvfs-rm.1 \
	gvfs-save.1 \
	gvfs-set-attribute.1 \
	gvfs-trash.1 \
	gvfs-tree.1 \
	gvfs.7 \
	gvfsd.1 \
	gvfsd-fuse.1 \
	gvfsd-metadata.1

EXTRA_DIST = \
	deprecated.xml.in \
	gvfsd-fuse.xml \
	gvfsd-metadata.xml \
	gvfsd.xml \
	gvfs-less.xml \
	gvfs.xml \
	meson.build \
	$(NULL)

CLEANFILES = \
	$(man_MANS) \
	gvfs-cat.xml \
	gvfs-copy.xml \
	gvfs-info.xml \
	gvfs-ls.xml \
	gvfs-mime.xml \
	gvfs-mkdir.xml \
	gvfs-monitor-dir.xml \
	gvfs-monitor-file.xml \
	gvfs-mount.xml \
	gvfs-move.xml \
	gvfs-open.xml \
	gvfs-rename.xml \
	gvfs-rm.xml \
	gvfs-save.xml \
	gvfs-set-attribute.xml \
	gvfs-trash.xml \
	gvfs-tree.xml \
	$(NULL)
