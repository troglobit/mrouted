# config.mk                                                     -*-Makefile-*-
# This is the mrouted build configuration file.  See the below description for
# details on each build option.

# -D_GNU_SOURCE : Use GNU extensions, where possible
# -D_BSD_SOURCE : Use functions derived from 4.3 BSD Unix rather than POSIX.1
DEFS = -D_BSD_SOURCE -D_GNU_SOURCE

##
# Compilation flags for different platforms. 
# Uncomment only one of them. Default: Linux

# If the multicast header files are not in the standard place on your system,
# define MCAST_INCLUDE to be an appropriate `-I' options for the C compiler.
#MCAST_INCLUDE=	-I/sys
MCAST_INCLUDE = -Iinclude

## FreeBSD	-D__FreeBSD__ is defined by the OS
## FreeBSD-3.x, FreeBSD-4.x
#INCLUDES     = -Iinclude/freebsd
#DEFS        +=
## FreeBSD-2.x
#INCLUDES     = -Iinclude/freebsd2
#DEFS        +=

## NetBSD	-DNetBSD is defined by the OS
#INCLUDES     = -Iinclude/netbsd
#DEFS        +=

## OpenBSD	-DOpenBSD is defined by the OS
#INCLUDES     = -Iinclude/openbsd
#DEFS        +=

## BSDI		-D__bsdi__ is defined by the OS
#INCLUDES     =
#DEFS        +=

## SunOS, OSF1, gcc
#INCLUDES     = -Iinclude/sunos-gcc
#DEFS        += -DSunOS=43

## SunOS, OSF1, cc
#INCLUDES     = -Iinclude/sunos-cc
#DEFS        += -DSunOS=43

## IRIX
#INCLUDES     =
#DEFS        += -D_BSD_SIGNALS -DIRIX

## Solaris 2.5, gcc
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=55
## Solaris 2.5, cc
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=55
## Solaris 2.6
#INCLUDES     =
#DEFS        += -DSYSV -DSUNOS5 -DSunOS=56
## Solaris 2.x
#LIB2         = -L/usr/ucblib -lucb -L/usr/lib -lsocket -lnsl

## Linux v2.2, or later
INCLUDES      = -Iinclude/linux
DEFS         += -DLinux -DIOCTL_OK_ON_RAW_SOCKET
