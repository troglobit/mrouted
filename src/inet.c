/*
 * The mrouted program is covered by the license in the accompanying file
 * named "LICENSE".  Use of the mrouted program represents acceptance of
 * the terms and conditions listed in that file.
 *
 * The mrouted program is COPYRIGHT 1989 by The Board of Trustees of
 * Leland Stanford Junior University.
 */

#include <arpa/inet.h>
#include <netdb.h>

#include "defs.h"

/*
 * Exported variables.
 */
char s1[MAX_INET_BUF_LEN];		/* buffers to hold the string representations  */
char s2[MAX_INET_BUF_LEN];		/* of IP addresses, to be passed to inet_fmt() */
char s3[MAX_INET_BUF_LEN];		/* or inet_fmts().                             */
char s4[MAX_INET_BUF_LEN];


/*
 * Verify that a given IP address is credible as a host address.
 * (Without a mask, cannot detect addresses of the form {subnet,0} or
 * {subnet,-1}.)
 */
int inet_valid_host(uint32_t naddr)
{
    uint32_t addr;

    addr = ntohl(naddr);

    return (!(IN_MULTICAST(addr) ||
	      IN_BADCLASS (addr) ||
	      (addr & 0xff000000) == 0));
}

/*
 * Verify that a given netmask is plausible;
 * make sure that it is a series of 1's followed by
 * a series of 0's with no discontiguous 1's.
 */
int inet_valid_mask(uint32_t mask)
{
    if (~(((mask & -mask) - 1) | mask) != 0) {
	/* Mask is not contiguous */
	return FALSE;
    }

    return TRUE;
}

/*
 * Verify that a given subnet number and mask pair are credible.
 *
 * With CIDR, almost any subnet and mask are credible.  mrouted still
 * can't handle aggregated class A's, so we still check that, but
 * otherwise the only requirements are that the subnet address is
 * within the [ABC] range and that the host bits of the subnet
 * are all 0.
 */
int inet_valid_subnet(uint32_t nsubnet, uint32_t nmask)
{
    uint32_t subnet, mask;

    subnet = ntohl(nsubnet);
    mask   = ntohl(nmask);

    if ((subnet & mask) != subnet) return FALSE;

    if (subnet == 0)
	return mask == 0;

    if (IN_CLASSA(subnet)) {
	if (mask < 0xff000000 ||
	    (subnet & 0xff000000) == 0x7f000000 ||
	    (subnet & 0xff000000) == 0x00000000) return FALSE;
    }
    else if (IN_CLASSD(subnet) || IN_BADCLASS(subnet)) {
	/* Above Class C address space */
	return FALSE;
    }
    if (subnet & ~mask) {
	/* Host bits are set in the subnet */
	return FALSE;
    }
    if (!inet_valid_mask(mask)) {
	/* Netmask is not contiguous */
	return FALSE;
    }

    return TRUE;
}


/*
 * Convert an IP address to name
 */
char *inet_name(uint32_t addr, int numeric)
{
    static char host[NI_MAXHOST];
    struct sockaddr_in sin;
    struct sockaddr *sa;
    struct in_addr in;
    int rc;

    if (addr == 0)
	return "local";

    if (numeric) {
	in.s_addr = addr;
	return inet_ntoa(in);
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = addr;
    sa = (struct sockaddr *)&sin;

    rc = getnameinfo(sa, sizeof(sin), host, sizeof(host), NULL, 0, 0);
    if (rc)
	return NULL;

    return host;
}


/*
 * Convert an IP address in uint32_t (network) format into a printable string.
 */
char *inet_fmt(uint32_t addr, char *s, size_t len)
{
    uint8_t *a;

    a = (uint8_t *)&addr;
    snprintf(s, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);

    return s;
}


/*
 * Convert an IP subnet number in uint32_t (network) format into a printable
 * string including the netmask as a number of bits.
 */
char *inet_fmts(uint32_t addr, uint32_t mask, char *s, size_t len)
{
    uint8_t *a, *m;
    int bits;

    if ((addr == 0) && (mask == 0)) {
	snprintf(s, len, "default");
	return s;
    }
    a = (uint8_t *)&addr;
    m = (uint8_t *)&mask;
    bits = 33 - ffs(ntohl(mask));

    if      (m[3] != 0) snprintf(s, len, "%u.%u.%u.%u/%d", a[0], a[1], a[2], a[3],
						bits);
    else if (m[2] != 0) snprintf(s, len, "%u.%u.%u/%d",    a[0], a[1], a[2], bits);
    else if (m[1] != 0) snprintf(s, len, "%u.%u/%d",       a[0], a[1], bits);
    else                snprintf(s, len, "%u/%d",          a[0], bits);

    return s;
}

/*
 * Convert the printable string representation of an IP address into the
 * uint32_t (network) format.  Return 0xffffffff on error.  (To detect the
 * legal address with that value, you must explicitly compare the string
 * with "255.255.255.255".)
 */
uint32_t inet_parse(char *s, int n)
{
    uint32_t a = 0;
    uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i;
    char c;

    i = sscanf(s, "%u.%u.%u.%u%c", &a0, &a1, &a2, &a3, &c);
    if (i < n || i > 4 || a0 > 255 || a1 > 255 || a2 > 255 || a3 > 255)
	return 0xffffffff;

    ((uint8_t *)&a)[0] = a0;
    ((uint8_t *)&a)[1] = a1;
    ((uint8_t *)&a)[2] = a2;
    ((uint8_t *)&a)[3] = a3;

    return a;
}


/*
 * inet_cksum extracted from:
 *			P I N G . C
 *
 * Author -
 *	Mike Muuss
 *	U. S. Army Ballistic Research Laboratory
 *	December, 1983
 * Modified at Uc Berkeley
 *
 * (ping.c) Status -
 *	Public Domain.  Distribution Unlimited.
 *
 *			I N _ C K S U M
 *
 * Checksum routine for Internet Protocol family headers (C Version)
 *
 */
int inet_cksum(uint16_t *addr, uint32_t len)
{
	int nleft = (int)len;
	uint16_t *w = addr;
	uint16_t answer = 0;
	int32_t sum = 0;

	/*
	 *  Our algorithm is simple, using a 32 bit accumulator (sum),
	 *  we add sequential 16 bit words to it, and at the end, fold
	 *  back all the carry bits from the top 16 bits into the lower
	 *  16 bits.
	 */
	while (nleft > 1)  {
		sum += *w++;
		nleft -= 2;
	}

	/* mop up an odd byte, if necessary */
	if (nleft == 1) {
		*(uint8_t *) (&answer) = *(uint8_t *)w ;
		sum += answer;
	}

	/*
	 * add back carry outs from top 16 bits to low 16 bits
	 */
	sum = (sum >> 16) + (sum & 0xffff);	/* add hi 16 to low 16 */
	sum += (sum >> 16);			/* add carry */
	answer = ~sum;				/* truncate to 16 bits */

	return answer;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "ellemtel"
 *  c-basic-offset: 4
 * End:
 */
