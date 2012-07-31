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
	gvfs-tree.1 \
	gvfs.7 \
	gvfsd.1 \
	gvfsd-fuse.1 \
	gvfsd-metadata.1

xml_files = ${patsubst %.1,%.xml,${patsubst %.7,%.xml,$(man_MANS)}}

EXTRA_DIST = $(xml_files)

CLEANFILES = $(man_MANS)
