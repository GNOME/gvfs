LIBS=`pkg-config --libs glib-2.0 gobject-2.0 gthread-2.0`
CFLAGS=`pkg-config --cflags glib-2.0 gobject-2.0 gthread-2.0` -I./ -Wall -O -g -D_GNU_SOURCE

CFILES=gfileinfo.c ginputstream.c goutputstream.c gvfserror.c gfileenumerator.c gfile.c gvfs.c gvfssimple.c ginputstreamfile.c goutputstreamfile.c gioscheduler.c
HFILES=gfileinfo.h ginputstream.h goutputstream.h gvfserror.h gfileenumerator.h gfile.h gvfs.h gvfssimple.h ginputstreamfile.h goutputstreamfile.h gioscheduler.h gvfstypes.h

test: test.c $(CFILES) $(HFILES) Makefile
	gcc $(LIBS) $(CFLAGS) -o test test.c $(CFILES)

clean:
	rm test
