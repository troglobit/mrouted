README
======
[![Build Status](https://travis-ci.org/troglobit/mrouted.png?branch=master)](https://travis-ci.org/troglobit/mrouted)[![Coverity Scan Status](https://scan.coverity.com/projects/3320/badge.svg)](https://scan.coverity.com/projects/3320)

mrouted is a 3-clause BSD licensed implementation of the DVMRP multicast
routing protocol.  It can run on any UNIX based system, from embedded
Linux systems to workstations, turning them into multicast routers with
tunnel support, which can be used to cross non-multicast-aware routers.

DVMRP is a distance vector based protocol, derived from RIP, suitable
for closely located multicast users in smaller networks.  It simply
floods all multicast streams to all routers, i.e. implicit join.  This
is also known as "flood and prune" since you can opt out from groups you
do not want. For a detailed explanation of the protocol, consult
[RFC 1075](http://tools.ietf.org/html/rfc1075).


History
-------

The mrouted routing daemon was developed by David Waitzman, Craig
Partridge, Steve Deering, Ajit Thyagarajan, Bill Fenner, David Thaler
and Daniel Zappala.  With contributions by many others.

The last release by Mr. Fenner was 3.9-beta3 on April 26 1999 and
mrouted has been in "beta" status since then.  Several prominent UNIX
operating systems, such as AIX, Solaris, HP-UX, BSD/OS, NetBSD, FreeBSD,
OpenBSD as well as most GNU/Linux based distributions have used that
beta as a de facto stable release, with (mostly) minor patches for
system adaptations.  Over time however many dropped support, but Debian
and OpenBSD kept it under their wings.

In March 2003 [OpenBSD](http://www.openbsd.org/), led by the fearless
Theo de Raadt, managed to convince Stanford to release mrouted under a
fully free license, the
[3-clause BSD license](http://www.openbsd.org/cgi-bin/cvsweb/src/usr.sbin/mrouted/LICENSE).
Unfortunately, in February 2005
[Debian dropped mrouted](http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=288112)
as an "obsolete protocol".

For a long time the OpenBSD team remained the sole guardian of this
project.  In 2010 [Joachim Nilsson](http://troglobit.com) revived
mrouted on [GitHub](https://github.com/troglobit/mrouted).  The 3.9.x
stable series represent the first releases in over a decade.  Patches
from all over the Internet, including OpenBSD, have been merged.


Bugs
----

The basic functionality has been tested thoroughly over the years, but
that does not mean mrouted is bug free.  Please report any oddities,
feature requests, patches or pull requests in the github issue tracker
at:

  http://github.com/troglobit/mrouted/issues

mrouted contain "hacks" to recognize Cisco IOS's pseudo-DVMRP
implementation to yield all "DVMRP" versions of 10 and higher to IOS.
Previous pre-release versions of mrouted 3.9.0 made the assumption that
the IOS bug that causes it to use the IOS version number instead of the
DVMRP version number would be fixed by the time 12.0 was out.


Configuring
-----------

mrouted reads its configuration file from `/etc/mrouted.conf`.  You can
override the default by specifying an alternate file when invoking
mrouted.

    mrouted -f /path/file.conf

mrouted can be reconfigured at runtime if you change the configuration
file, simply send the process a `SIGHUP` to activate new changes to the
file.  The PID is saved automatically to the file `/var/run/mrouted.pid`
for your convenience.

By default, mrouted configures itself to act as a multicast router on
all multicast capable interfaces, excluding the loopback interface that
has the `IFF_MULTICAST` flag set.  Therefore, you do not need to
explicitly configure mrouted, unless you need to setup tunnel links,
change the default operating parameters, or disable multicast routing
over a specific physical interface.

See the man page for further details.


Running
-------

mrouted must run as root.

For the native mrouted tunnel to work in Linux based systems, you need
to have the "ipip" kernel module loaded or as built-in.

    modprobe ipip

Several signals are supported, for querying status, or simply for
reloading the configuration.  See the man page for details.


RSRR
----
Routing Support for Resource Reservations (RSRR) is required for running
RSVP and was contributed by Daniel Zappala <daniel@isi.edu>.

**Note:** This has not been tested for many years ...

To enable RSRR support, uncomment the three lines starting with RSRR
near the top of the Makefile and "make clean; make".  Or use the
prebuilt binary, mrouted.rsrr .

RSRR allows RSVP to query mrouted for its routing entry for a particular
source-group pair.  Using the routing entry and the `IP_MULTICAST_VIF`
socket call, RSVP can forward distinct control messages out each
outgoing interface.  This version of mrouted supports RSRR messages
using a Unix datagram socket.

RSRR currently includes two pairs of query-reply messages.  RSVP sends
an Initial Query when it starts.  Mrouted responds with an Initial Reply
that includes the set of vifs it is using, flagging those that are
administratively disabled.  When RSVP needs the routing entry for a
source-group pair, it sends a Route Query.  Mrouted responds with a
Route Reply that includes the incoming vif and outgoing vifs for the
source-group pair.

RSVP may request route change notification by setting the notification
bit in the Route Query.  If mrouted can provide route change
notification for the source-group pair, it sets the notification bit in
its Route Reply.  When the routing entry for the source-group pair
changes, mrouted sends an unsolicited Route Reply containing the new
routing information.  The initial release of mrouted 3.5 did not support
route change notification and always returned a Route Reply with the
notification bit cleared.  This release of mrouted provides route change
notification when possible.
