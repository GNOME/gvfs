#!/bin/bash

if [ $# -lt 1 ]; then
    echo missing argument
    exit
fi
    
# Set up env vars to make gvfs read mounts from the build tree
export GVFS_MOUNTABLE_EXTENSION=".localmount"
export GVFS_MOUNTABLE_DIR=`pwd`/../daemon
export GVFS_MONITOR_DIR=`pwd`
export PATH=`pwd`/../programs:$PATH
export GIO_EXTRA_MODULES=`pwd`/../client/.libs:`pwd`/../monitor/proxy/.libs

DBUS_CONF=`dirname $0`/session.conf

# Start a custom session dbus
PIDFILE=`mktemp`
export DBUS_SESSION_BUS_ADDRESS=`dbus-daemon --config-file=$DBUS_CONF --fork --print-address=1 --print-pid=3 3>${PIDFILE}`
DBUS_SESSION_BUS_PID=`cat $PIDFILE`
rm $PIDFILE

trap "kill -9 $DBUS_SESSION_BUS_PID" SIGINT SIGTERM EXIT

$@

