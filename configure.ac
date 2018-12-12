AC_PREREQ(2.61)
AC_INIT(mrouted, 4.0-dev, https://github.com/troglobit/mrouted/issues, , http://troglobit.com/mrouted.html)
AM_INIT_AUTOMAKE([1.11 foreign])
AM_SILENT_RULES([yes])

AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile src/Makefile man/Makefile])

# Check for standard programs, headers, and functions
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_YACC

# Check for linux/netlink.h is only to be able to define LINUX below
AC_HEADER_STDC
AC_CHECK_HEADERS([fcntl.h ifaddrs.h netinet/igmp.h sys/time.h linux/netlink.h])
AC_CHECK_HEADERS([net/if.h], [], [], [
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif])

# Check if some func is not in libc
AC_CHECK_LIB([util], [pidfile])

# Check for required functions in libc
AC_CHECK_FUNCS([atexit getifaddrs])

# Check for usually missing API's, which we can replace
AC_REPLACE_FUNCS([pidfile strlcpy strtonum])
AC_CONFIG_LIBOBJ_DIR([lib])

AC_ARG_ENABLE(rsrr,
	AS_HELP_STRING([--enable-rsrr], [Routing Support for Resource Reservation
			currently used by RSVP (EXPERIMENTAL).  For details, see
			http://tools.ietf.org/html/draft-ietf-rsvp-routing-02]),,
	enable_rsrr=no)

AS_IF([test "x$enable_rsrr" = "xyes"], [
        AC_DEFINE(RSRR, 1, [Routing Support for Resource Reservation.])])

AM_CONDITIONAL(BSD,     [test "x$ac_cv_header_net_if_dl_h"     = "xyes"])
AM_CONDITIONAL(LINUX,   [test "x$ac_cv_header_linux_netlink_h" = "xyes"])
AM_CONDITIONAL(RSRR,    [test "x$enable_rsrr"                  = "xyes"])

# Generate all files
AC_OUTPUT

cat <<EOF

------------------ Summary ------------------
 $PACKAGE_NAME version $PACKAGE_VERSION
  Prefix................: $prefix
  Sysconfdir............: `eval echo $sysconfdir`
  Localstatedir.........: `eval echo $localstatedir`
  C Compiler............: $CC $CFLAGS $CPPFLAGS $LDFLAGS $LIBS

Optional features:
  RSRR (experimental)...: $enable_rsrr

------------- Compiler version --------------
$($CC --version || true)
---------------------------------------------

Check the above options and compile with:
 ${MAKE-make}

EOF
