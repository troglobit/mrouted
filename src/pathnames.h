/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */
#ifndef MROUTED_PATHNAMES_H_
#define MROUTED_PATHNAMES_H_

#include <paths.h>

#define _PATH_MROUTED_CONF	SYSCONFDIR   "/%s.conf"
#define _PATH_MROUTED_GENID	PRESERVEDIR  "/%s.genid"
#define _PATH_MROUTED_RUNDIR    RUNSTATEDIR
#define _PATH_MROUTED_SOCK	RUNSTATEDIR  "/%s.sock"

#endif /* MROUTED_PATHNAMES_H_ */
