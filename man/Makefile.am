.xml.1:
	$(AM_V_GEN) $(XSLTPROC) -o $@ --nonet http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

man_MANS = \
	gvfs-cat.1 \
	gvfs-copy.1 \
	gvfs-info.1 \
	gvfs-ls.1 \
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
	gvfs-tree.1

xml_files = $(man_MANS:.1=.xml)

EXTRA_DIST = $(xml_files)

DISTCLEANFILES = $(man_MANS)
