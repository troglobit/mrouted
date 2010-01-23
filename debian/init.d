#!/bin/sh
#
# This file was automatically customized by debmake on Mon,  1 Mar 1999 21:36:23 +0100
#
# Written by Miquel van Smoorenburg <miquels@cistron.nl>.
# Modified for Debian GNU/Linux by Ian Murdock <imurdock@gnu.org>.
# Modified for Debian by Christoph Lameter <clameter@debian.org>

PATH=/bin:/usr/bin:/sbin:/usr/sbin
DAEMON=/usr/sbin/mrouted
# The following value is extracted by debstd to figure out how to generate
# the postinst script. Edit the field to change the way the script is
# registered through update-rc.d (see the manpage for update-rc.d!)
FLAGS="defaults 50"

test -f $DAEMON || exit 0

case "$1" in
  start)
    insmod -k ipip 2> /dev/null || true
    start-stop-daemon --start --verbose --exec $DAEMON
    ;;
  stop)
    start-stop-daemon --stop --verbose --exec $DAEMON
    ;;
  reload|force-reload)
    start-stop-daemon --stop --signal 1 --verbose --exec $DAEMON
    ;;
  restart)
    start-stop-daemon --stop --verbose --exec $DAEMON
    sleep 1
    start-stop-daemon --start --verbose --exec $DAEMON
    ;;
  *)
    echo "Usage: /etc/init.d/mrouted {start|stop|restart|force-reload}"
    exit 1
    ;;
esac

exit 0
