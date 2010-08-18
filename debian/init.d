#! /bin/sh
#
### BEGIN INIT INFO
# Provides:          mrouted
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
### END INIT INFO
#
# Written by Miquel van Smoorenburg <miquels@cistron.nl>.
# Modified for Debian GNU/Linux by Ian Murdock <imurdock@gnu.org>.
# Modified for Debian by Christoph Lameter <clameter@debian.org>

PATH=/bin:/usr/bin:/sbin:/usr/sbin
DAEMON=/usr/sbin/mrouted
NAME=mrouted
DESC=mrouted

test -f $DAEMON || exit 0

case "$1" in
        start)
                echo -n "Starting $DESC: "
                modprobe ipip 2> /dev/null || true
                start-stop-daemon --start --quiet --pidfile /var/run/$NAME.pid \
                        --exec $DAEMON
                echo "$NAME."
                ;;
        stop)
                echo -n "Stopping $DESC: "
                start-stop-daemon --stop --quiet --pidfile /var/run/$NAME.pid \
                        --exec $DAEMON
                echo "$NAME."
                ;;
        reload|force-reload)
                echo -n "Reloading $DESC: "
                start-stop-daemon --stop --signal 1 --quiet --pidfile /var/run/$NAME.pid \
                        --exec $DAEMON
                echo "$NAME."
                ;;
        restart)
                
                echo -n "Restarting $DESC: "
                start-stop-daemon --stop --quiet --pidfile \
                        /var/run/$NAME.pid --exec $DAEMON
                sleep 1
                start-stop-daemon --start --quiet --pidfile \
                        /var/run/$NAME.pid --exec $DAEMON
                echo "$NAME."
                ;;
        *)
                N=/etc/init.d/$NAME
                echo "Usage: $N {start|stop|restart|reload|force-reload}"
                exit 1
                ;;
esac

exit 0
