#! /bin/sh
### BEGIN INIT INFO
# Provides:          mrouted
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Multicast routing daemon, mrouted
# Description:       The original dynamic multicast routing daemon, mrouted
### END INIT INFO
. /lib/lsb/init-functions

PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
DAEMON=/usr/sbin/mrouted
STATUS=/usr/sbin/mroutectl
NAME=mrouted
DESC=mrouted

test -x $DAEMON || exit 0

set -e

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
                start-stop-daemon --stop --quiet --oknodo --pidfile /var/run/$NAME.pid \
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

	status)
		$STATUS
		;;

        *)
                N=/etc/init.d/$NAME
                echo "Usage: $N {start|stop|restart|reload|force-reload}" >&2
                exit 1
                ;;
esac

exit 0
