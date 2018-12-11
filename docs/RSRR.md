RSRR
----

Routing Support for Resource Reservations (RSRR) is required for running
RSVP and was contributed by Daniel Zappala.

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
routing information.

