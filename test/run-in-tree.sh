#!/bin/sh

if [ $# -lt 1 ]; then
    echo missing argument
    exit
fi
    
# Set up env vars to make gvfs read mounts from the build tree
export GVFS_MOUNTABLE_EXTENSION=".localmount"
export GVFS_MOUNTABLE_DIR=`pwd`/../daemon
export GVFS_MONITOR_DIR=`pwd`
export PATH=/usr/local/sbin:/usr/sbin:/sbin:$PATH
export GIO_EXTRA_MODULES=`pwd`/../client/.libs:`pwd`/../monitor/proxy/.libs

# Start a custom session dbus, unless we run under "make check" (test suite
# starts its own)
if [ -z "$MAKEFLAGS" ]; then
    if [ -e $(pwd)/session.conf ]; then
        # case for out-of tree build (distcheck)
        DBUS_CONF=`pwd`/session.conf
    else
        # case for calling this manually in a built tree
        DBUS_CONF=`dirname $0`/session.conf
    fi

    PIDFILE=`mktemp`
    export DBUS_SESSION_BUS_ADDRESS=`dbus-daemon --config-file=$DBUS_CONF --fork --print-address=1 --print-pid=3 3>${PIDFILE}`
    DBUS_SESSION_BUS_PID=`cat $PIDFILE`
    rm $PIDFILE

    trap "kill -9 $DBUS_SESSION_BUS_PID" INT TERM EXIT
fi

$@

