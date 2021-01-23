#! /bin/sh
### BEGIN INIT INFO
# Provides:          mrouted
# Required-Start:    $remote_fs $syslog $network
# Required-Stop:     $remote_fs $syslog $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Multicast routing daemon, mrouted
# Description:       The original dynamic multicast routing daemon, mrouted
### END INIT INFO
. /lib/lsb/init-functions

PATH=/sbin:/bin:/usr/sbin:/usr/bin
DESC="Multicast routing daemon"
NAME=mrouted

DAEMON=/usr/sbin/mrouted
PIDFILE=/var/run/mrouted.pid
SCRIPTNAME=/etc/init.d/$NAME

# Exit if the package is not installed
[ -x "$DAEMON" ] || exit 0

# Read configuration variable file if it is present
[ -r /etc/default/$NAME ] && . /etc/default/$NAME

# Define LSB log_* functions.
. /lib/lsb/init-functions

do_start()
{
    start-stop-daemon --start --quiet --pidfile $PIDFILE --exec $DAEMON -- $MROUTED_OPTIONS
}

do_signal()
{
    start-stop-daemon --stop --quiet --signal $1 $2 --pidfile $PIDFILE --exec $DAEMON
}

do_stop()
{
    do_signal TERM --oknodo
}

do_reload()
{
    do_signal HUP
}

# mrouted has native support for ip-ip tunnels
load_module()
{
    lsmod |grep ipip
    if [ $? -ne 0 ]; then
        modprobe ipip 2> /dev/null || true
    fi
}

case "$1" in
    start)
        log_daemon_msg "Starting $DESC" "$NAME"
        load_module
        do_start
        case "$?" in
            0) log_end_msg 0 ;;
            1) log_progress_msg "already started"
               log_end_msg 0 ;;
            *) log_end_msg 1 ;;
        esac
        ;;

    stop)
        log_daemon_msg "Stopping $DESC" "$NAME"
        do_stop
        case "$?" in
            0) log_end_msg 0 ;;
            1) log_progress_msg "already stopped"
               log_end_msg 0 ;;
            *) log_end_msg 1 ;;
        esac
        ;;

    reload)
        log_daemon_msg "Reloading $DESC" "$NAME"
	do_reload
	;;

    restart|force-reload)
        $0 stop
        $0 start
        ;;

    try-restart)
        $0 status >/dev/null 2>&1 && $0 restart
        ;;

    status)
        status_of_proc -p $PIDFILE $DAEMON $NAME && exit 0 || exit $?
        ;;

    *)
        echo "Usage: $SCRIPTNAME {start|stop|reload|restart|force-reload|try-restart|status}" >&2
        exit 3
        ;;
esac

:
