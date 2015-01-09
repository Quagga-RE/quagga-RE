/* BGP nexthop scan
   Copyright (C) 2000 Kunihiro Ishiguro

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the Free
Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

#include <zebra.h>

#include "command.h"
#include "thread.h"
#include "prefix.h"
#include "zclient.h"
#include "stream.h"
#include "network.h"
#include "log.h"
#include "memory.h"
#include "table.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_table.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_damp.h"
#include "zebra/rib.h"
#include "zebra/zserv.h"	/* For ZEBRA_SERV_PATH. */

struct bgp_nexthop_cache *zlookup_query (struct in_addr);
#ifdef HAVE_IPV6
struct bgp_nexthop_cache *zlookup_query_ipv6 (struct in6_addr *);
#endif /* HAVE_IPV6 */
static int zlookup_write_packet (const char *, int *, const u_char *, const int);

/* Only one BGP scan thread are activated at the same time. */
static struct thread *bgp_scan_thread = NULL;

/* BGP import thread */
static struct thread *bgp_import_thread = NULL;

/* BGP scan interval. */
static int bgp_scan_interval;

/* BGP import interval. */
static int bgp_import_interval;

/* Route table for next-hop lookup cache. */
static struct bgp_table *bgp_nexthop_cache_table[AFI_MAX];
static struct bgp_table *cache1_table[AFI_MAX];
static struct bgp_table *cache2_table[AFI_MAX];

/* Route table for connected route. */
static struct bgp_table *bgp_connected_table[AFI_MAX];

/* BGP nexthop lookup query client. */
struct zclient *zlookup = NULL;

/* Add nexthop to the end of the list.  */
static void
bnc_nexthop_add (struct bgp_nexthop_cache *bnc, struct nexthop *nexthop)
{
  struct nexthop *last;

  for (last = bnc->nexthop; last && last->next; last = last->next)
    ;
  if (last)
    last->next = nexthop;
  else
    bnc->nexthop = nexthop;
  nexthop->prev = last;
}

static void
bnc_nexthop_free (struct bgp_nexthop_cache *bnc)
{
  struct nexthop *nexthop;
  struct nexthop *next = NULL;

  for (nexthop = bnc->nexthop; nexthop; nexthop = next)
    {
      next = nexthop->next;
      XFREE (MTYPE_NEXTHOP, nexthop);
    }
}

static struct bgp_nexthop_cache *
bnc_new (void)
{
  return XCALLOC (MTYPE_BGP_NEXTHOP_CACHE, sizeof (struct bgp_nexthop_cache));
}

static void
bnc_free (struct bgp_nexthop_cache *bnc)
{
  bnc_nexthop_free (bnc);
  XFREE (MTYPE_BGP_NEXTHOP_CACHE, bnc);
}

static int
bgp_nexthop_same (struct nexthop *next1, struct nexthop *next2)
{
  if (next1->type != next2->type)
    return 0;

  switch (next1->type)
    {
    case ZEBRA_NEXTHOP_IPV4:
      if (! IPV4_ADDR_SAME (&next1->gate.ipv4, &next2->gate.ipv4))
	return 0;
      break;
    case ZEBRA_NEXTHOP_IFINDEX:
    case ZEBRA_NEXTHOP_IFNAME:
      if (next1->ifindex != next2->ifindex)
	return 0;
      break;
#ifdef HAVE_IPV6
    case ZEBRA_NEXTHOP_IPV6:
      if (! IPV6_ADDR_SAME (&next1->gate.ipv6, &next2->gate.ipv6))
	return 0;
      break;
    case ZEBRA_NEXTHOP_IPV6_IFINDEX:
    case ZEBRA_NEXTHOP_IPV6_IFNAME:
      if (! IPV6_ADDR_SAME (&next1->gate.ipv6, &next2->gate.ipv6))
	return 0;
      if (next1->ifindex != next2->ifindex)
	return 0;
      break;
#endif /* HAVE_IPV6 */
    default:
      /* do nothing */
      break;
    }
  return 1;
}

static int
bgp_nexthop_cache_different (struct bgp_nexthop_cache *bnc1,
			   struct bgp_nexthop_cache *bnc2)
{
  int i;
  struct nexthop *next1, *next2;

  if (bnc1->nexthop_num != bnc2->nexthop_num)
    return 1;

  next1 = bnc1->nexthop;
  next2 = bnc2->nexthop;

  for (i = 0; i < bnc1->nexthop_num; i++)
    {
      if (! bgp_nexthop_same (next1, next2))
	return 1;

      next1 = next1->next;
      next2 = next2->next;
    }
  return 0;
}

/* If nexthop exists on connected network return 1. */
int
bgp_nexthop_onlink (afi_t afi, struct attr *attr)
{
  struct bgp_node *rn;

  /* Lookup the address is onlink or not. */
  if (afi == AFI_IP)
    {
      rn = bgp_node_match_ipv4 (bgp_connected_table[AFI_IP], &attr->nexthop);
      if (rn)
	{
	  bgp_unlock_node (rn);
	  return 1;
	}
    }
#ifdef HAVE_IPV6
  else if (afi == AFI_IP6)
    {
      if (attr->extra->mp_nexthop_len == 32)
	return 1;
      else if (attr->extra->mp_nexthop_len == 16)
	{
	  if (IN6_IS_ADDR_LINKLOCAL (&attr->extra->mp_nexthop_global))
	    return 1;

	  rn = bgp_node_match_ipv6 (bgp_connected_table[AFI_IP6],
				      &attr->extra->mp_nexthop_global);
	  if (rn)
	    {
	      bgp_unlock_node (rn);
	      return 1;
	    }
	}
    }
#endif /* HAVE_IPV6 */
  return 0;
}

static void
bnct_init (const afi_t afi)
{
  cache1_table[afi] = bgp_nexthop_cache_table[afi] = bgp_table_init (afi, SAFI_UNICAST);
  cache2_table[afi] = bgp_table_init (afi, SAFI_UNICAST);
}

static struct bgp_table *
bnct_active (const afi_t afi)
{
  return bgp_nexthop_cache_table[afi];
}

static struct bgp_table *
bnct_inactive (const afi_t afi)
{
  return bgp_nexthop_cache_table[afi] == cache1_table[afi] ? cache2_table[afi] : cache1_table[afi];
}

static void
bnct_swap (const afi_t afi)
{
  bgp_nexthop_cache_table[afi] = bnct_inactive (afi);
}

static void
bnct_finish (const afi_t afi)
{
  bgp_table_unlock (cache1_table[afi]);
  bgp_table_unlock (cache2_table[afi]);
  cache1_table[afi] = cache2_table[afi] = NULL;
}

#ifdef HAVE_IPV6
/* Check specified next-hop is reachable or not. */
static int
bgp_nexthop_lookup_ipv6 (struct peer *peer, struct bgp_info *ri, int *changed,
			 int *metricchanged)
{
  struct bgp_node *rn;
  struct prefix p;
  struct bgp_nexthop_cache *bnc;
  struct attr *attr;

  /* Only check IPv6 global address only nexthop. */
  attr = ri->attr;

  if (attr->extra->mp_nexthop_len != 16 
      || IN6_IS_ADDR_LINKLOCAL (&attr->extra->mp_nexthop_global))
    return 1;

  memset (&p, 0, sizeof (struct prefix));
  p.family = AF_INET6;
  p.prefixlen = IPV6_MAX_BITLEN;
  p.u.prefix6 = attr->extra->mp_nexthop_global;

  /* IBGP or ebgp-multihop */
  rn = bgp_node_get (bnct_active (AFI_IP6), &p);

  if (rn->info)
    {
      bnc = rn->info;
      bgp_unlock_node (rn);
    }
  else
    {
      if (NULL == (bnc = zlookup_query_ipv6 (&attr->extra->mp_nexthop_global)))
	bnc = bnc_new ();
      else
	{
	  if (changed)
	    {
	      struct bgp_node *oldrn;

	      oldrn = bgp_node_lookup (bnct_inactive (AFI_IP6), &p);
	      if (oldrn)
		{
		  struct bgp_nexthop_cache *oldbnc = oldrn->info;

		  bnc->changed = bgp_nexthop_cache_different (bnc, oldbnc);

		  if (bnc->metric != oldbnc->metric)
		    bnc->metricchanged = 1;

                  bgp_unlock_node (oldrn);
		}
	    }
	}
      rn->info = bnc;
    }

  if (changed)
    *changed = bnc->changed;

  if (metricchanged)
    *metricchanged = bnc->metricchanged;

  if (bnc->valid && bnc->metric)
    (bgp_info_extra_get (ri))->igpmetric = bnc->metric;
  else if (ri->extra)
    ri->extra->igpmetric = 0;

  return bnc->valid;
}
#endif /* HAVE_IPV6 */

/* Check specified next-hop is reachable or not. */
int
bgp_nexthop_lookup (afi_t afi, struct peer *peer, struct bgp_info *ri,
		    int *changed, int *metricchanged)
{
  struct bgp_node *rn;
  struct prefix p;
  struct bgp_nexthop_cache *bnc;
  struct in_addr addr;

#ifdef HAVE_IPV6
  if (afi == AFI_IP6)
    return bgp_nexthop_lookup_ipv6 (peer, ri, changed, metricchanged);
#endif /* HAVE_IPV6 */

  addr = ri->attr->nexthop;

  memset (&p, 0, sizeof (struct prefix));
  p.family = AF_INET;
  p.prefixlen = IPV4_MAX_BITLEN;
  p.u.prefix4 = addr;

  /* IBGP or ebgp-multihop */
  rn = bgp_node_get (bnct_active (AFI_IP), &p);

  if (rn->info)
    {
      bnc = rn->info;
      bgp_unlock_node (rn);
    }
  else
    {
      if (NULL == (bnc = zlookup_query (addr)))
	bnc = bnc_new ();
      else
	{
	  if (changed)
	    {
	      struct bgp_node *oldrn;

	      oldrn = bgp_node_lookup (bnct_inactive (AFI_IP), &p);
	      if (oldrn)
		{
		  struct bgp_nexthop_cache *oldbnc = oldrn->info;

		  bnc->changed = bgp_nexthop_cache_different (bnc, oldbnc);

		  if (bnc->metric != oldbnc->metric)
		    bnc->metricchanged = 1;

                  bgp_unlock_node (oldrn);
		}
	    }
	}
      rn->info = bnc;
    }

  if (changed)
    *changed = bnc->changed;

  if (metricchanged)
    *metricchanged = bnc->metricchanged;

  if (bnc->valid && bnc->metric)
    (bgp_info_extra_get(ri))->igpmetric = bnc->metric;
  else if (ri->extra)
    ri->extra->igpmetric = 0;

  return bnc->valid;
}

/* Reset and free all BGP nexthop cache. */
static void
bgp_nexthop_cache_reset (struct bgp_table *table)
{
  struct bgp_node *rn;
  struct bgp_nexthop_cache *bnc;

  for (rn = bgp_table_top (table); rn; rn = bgp_route_next (rn))
    if ((bnc = rn->info) != NULL)
      {
	bnc_free (bnc);
	rn->info = NULL;
	bgp_unlock_node (rn);
      }
}

/* Translate the contents of a series of received ZEBRA_BGP_IPV4_RGATE_VERIFY
 * messages into the provided route_table structure.
 */
static u_char
recv_verified_desync_prefixes (struct route_table *pfxlist)
{
  struct stream *s = zlookup->ibuf;
  uint16_t length;
  u_char marker;
  u_char version;
  u_char morefollows;
  unsigned numpfx;

  stream_reset (s);

  stream_read (s, zlookup->sock, 2); /* nbytes */
  length = stream_getw (s);

  stream_read (s, zlookup->sock, length - 2); /* nbytes */
  marker = stream_getc (s);
  version = stream_getc (s);

  if (version != ZSERV_VERSION || marker != ZEBRA_HEADER_MARKER)
    {
      zlog_err ("%s: socket %d version mismatch, marker %d, version %d",
               __func__, zlookup->sock, marker, version);
      return 0;
    }
  assert (stream_getw (s) == ZEBRA_BGP_IPV4_RGATE_VERIFY);

  morefollows = stream_getc (s);
  numpfx = stream_getw (s);
  if (BGP_DEBUG (nexthop, NEXTHOP))
    zlog_debug ("%s: receiving %s%u IPv4 prefixes", __func__, morefollows ? "" : "last ", numpfx);
  while (numpfx--)
    {
      struct prefix p;
      struct route_node *rn;

      p.family = AF_INET;
      p.u.prefix4.s_addr = stream_get_ipv4 (s);
      p.prefixlen = stream_getc (s);
      rn = route_node_get (pfxlist, &p);
      if (rn->lock > 1)
        {
          zlog_warn ("%s: duplicate prefix", __func__);
          while (rn->lock > 1)
            route_unlock_node (rn);
        }
      rn->info = pfxlist; /* not used to access data, but must be non-NULL */
    }
  return morefollows;
}

/* Translate the provided array of nexthops into a series of transmitted
 * ZEBRA_BGP_IPV4_RGATE_VERIFY messages.
 */
static int
send_rgates (const struct nexthop nhbuf[], const unsigned numnh, const u_char morefollow)
{
  unsigned i;
  struct stream *s = zlookup->obuf;

  stream_reset (s);
  zclient_create_header (s, ZEBRA_BGP_IPV4_RGATE_VERIFY);
  stream_putc (s, morefollow);
  stream_putw (s, numnh);
  for (i = 0; i < numnh; i++)
    {
      stream_put_ipv4 (s, nhbuf[i].gate.ipv4.s_addr);
      stream_put_ipv4 (s, nhbuf[i].rgate.ipv4.s_addr);
    }
  if (BGP_DEBUG (nexthop, NEXTHOP))
    zlog_debug ("%s: sent %u IPv4 nexthops to verify", __func__, numnh);
  stream_putw_at (s, 0, stream_get_endp (s));
  return zlookup_write_packet (__func__, &zlookup->sock, stream_get_data (s), stream_get_endp (s));
}

/* Allocated stream size without the common message header and fixed fields,
 * divided by number of bytes per data item.
 */
#define VERIFIED_NEXTHOPS_PER_MSG ((ZEBRA_MAX_PACKET_SIZ - ZEBRA_HEADER_SIZE - 1 - 2) / 8)

/* Feed the given BNCT copy to zserv and store the nexthop verification results
 * (prefixes) received from zebra into the provided route_table.
 */
static void
verify_ipv4_rgates (struct bgp_table *nhtable, struct route_table *pfxlist)
{
  struct bgp_node *rn;
  struct bgp_nexthop_cache *bnc;
  struct nexthop buffered[VERIFIED_NEXTHOPS_PER_MSG];
  struct nexthop *nexthop;
  unsigned numbuffered = 0;

  if (zlookup->sock < 0)
    return;

  for (rn = bgp_table_top (nhtable); rn; rn = bgp_route_next (rn))
    if ((bnc = rn->info) != NULL && bnc->valid)
      for (nexthop = bnc->nexthop; nexthop; nexthop = nexthop->next)
        if (nexthop->type == NEXTHOP_TYPE_IPV4)
          {
            IPV4_ADDR_COPY (&buffered[numbuffered].gate.ipv4, &rn->p.u.prefix4);
            IPV4_ADDR_COPY (&buffered[numbuffered].rgate.ipv4, &nexthop->gate.ipv4);
            if (++numbuffered == VERIFIED_NEXTHOPS_PER_MSG)
              {
                if (send_rgates (buffered, numbuffered, 1) <= 0)
                  return;
                numbuffered = 0;
              }
            break; /* only the first IGP nexthop of a BGP nexthop matters */
          }

  if (send_rgates (buffered, numbuffered, 0) <= 0)
    return;
  while (recv_verified_desync_prefixes (pfxlist));
}

static void
bgp_scan (afi_t afi, safi_t safi)
{
  struct bgp_node *rn;
  struct bgp *bgp;
  struct bgp_info *bi;
  struct bgp_info *next;
  struct peer *peer;
  struct listnode *node, *nnode;
  struct route_table *desyncpfxs;
  struct route_node *dprn;
  int valid;
  int current;
  int changed;
  int metricchanged;

  bnct_swap (afi);

  /* Get default bgp. */
  bgp = bgp_get_default ();
  if (bgp == NULL)
    return;

  if (BGP_DEBUG (events, EVENTS))
    zlog_debug ("scanning IPv%s Unicast routing tables", afi == AFI_IP ? "4" : "6");

  /* Maximum prefix check */
  for (ALL_LIST_ELEMENTS (bgp->peer, node, nnode, peer))
    {
      if (peer->status != Established)
	continue;

      if (peer->afc[afi][SAFI_UNICAST])
	bgp_maximum_prefix_overflow (peer, afi, SAFI_UNICAST, 1);
      if (peer->afc[afi][SAFI_MULTICAST])
	bgp_maximum_prefix_overflow (peer, afi, SAFI_MULTICAST, 1);
      if (peer->afc[afi][SAFI_MPLS_VPN])
	bgp_maximum_prefix_overflow (peer, afi, SAFI_MPLS_VPN, 1);
    }

  if (afi == AFI_IP)
    {
      desyncpfxs = route_table_init();
      verify_ipv4_rgates (bnct_inactive (afi), desyncpfxs);
    }

  for (rn = bgp_table_top (bgp->rib[afi][SAFI_UNICAST]); rn;
       rn = bgp_route_next (rn))
    {
      for (bi = rn->info; bi; bi = next)
	{
	  next = bi->next;

	  if (bi->type == ZEBRA_ROUTE_BGP && bi->sub_type == BGP_ROUTE_NORMAL)
	    {
	      changed = 0;
	      metricchanged = 0;

              if (afi == AFI_IP)
                if ((dprn = route_node_match (desyncpfxs, &rn->p)))
                  {
                    /* The current prefix failed zebra nexthop verification,
                     * further checks can be omitted.
                     */
                    route_unlock_node (dprn);
                    if (BGP_DEBUG (nexthop, NEXTHOP))
                      {
                        char buf[INET_ADDRSTRLEN];
                        inet_ntop (AF_INET, &rn->p.u.prefix4, buf, INET_ADDRSTRLEN);
                        zlog_debug ("%s: rgate out of sync for %s/%u", __func__, buf, rn->p.prefixlen);
                      }
                    /* Setting this flag will eventually lead to the old BGP RIB
                     * entry of the prefix withdrawn at zebra side of the socket
                     * and reinstalled using freshly resolved IGP gateway.
                     */
                    SET_FLAG (bi->flags, BGP_INFO_IGP_CHANGED);
                    goto nextprefix;
                  }

	      if (peer_sort (bi->peer) == BGP_PEER_EBGP && bi->peer->ttl == 1)
		valid = bgp_nexthop_onlink (afi, bi->attr);
	      else
		valid = bgp_nexthop_lookup (afi, bi->peer, bi,
					    &changed, &metricchanged);

	      current = CHECK_FLAG (bi->flags, BGP_INFO_VALID) ? 1 : 0;

	      if (changed)
		SET_FLAG (bi->flags, BGP_INFO_IGP_CHANGED);
	      else
		UNSET_FLAG (bi->flags, BGP_INFO_IGP_CHANGED);

	      if (valid != current)
		{
		  if (CHECK_FLAG (bi->flags, BGP_INFO_VALID))
		    {
		      bgp_aggregate_decrement (bgp, &rn->p, bi,
					       afi, SAFI_UNICAST);
		      bgp_info_unset_flag (rn, bi, BGP_INFO_VALID);
		    }
		  else
		    {
		      bgp_info_set_flag (rn, bi, BGP_INFO_VALID);
		      bgp_aggregate_increment (bgp, &rn->p, bi,
					       afi, SAFI_UNICAST);
		    }
		}

              if (CHECK_FLAG (bgp->af_flags[afi][SAFI_UNICAST],
		  BGP_CONFIG_DAMPENING)
                  &&  bi->extra && bi->extra->damp_info )
                if (bgp_damp_scan (bi, afi, SAFI_UNICAST))
		  bgp_aggregate_increment (bgp, &rn->p, bi,
					   afi, SAFI_UNICAST);
nextprefix: ;
	    }
	}
      bgp_process (bgp, rn, afi, SAFI_UNICAST);
    }

  bgp_nexthop_cache_reset (bnct_inactive (afi));
  if (afi == AFI_IP)
    {
      for (dprn = route_top (desyncpfxs); dprn; dprn = route_next (dprn))
        dprn->info = NULL;
      route_table_finish (desyncpfxs);
    }
}

/* BGP scan thread.  This thread check nexthop reachability. */
static int
bgp_scan_timer (struct thread *t)
{
  bgp_scan_thread =
    thread_add_timer (master, bgp_scan_timer, NULL, bgp_scan_interval);

  if (BGP_DEBUG (events, EVENTS))
    zlog_debug ("Performing BGP general scanning");

  bgp_scan (AFI_IP, SAFI_UNICAST);

#ifdef HAVE_IPV6
  bgp_scan (AFI_IP6, SAFI_UNICAST);
#endif /* HAVE_IPV6 */

  return 0;
}

struct bgp_connected_ref
{
  unsigned int refcnt;
};

void
bgp_connected_add (struct connected *ifc)
{
  struct prefix p;
  struct prefix *addr;
  struct interface *ifp;
  struct bgp_node *rn;
  struct bgp_connected_ref *bc;

  ifp = ifc->ifp;

  if (! ifp)
    return;

  if (if_is_loopback (ifp))
    return;

  addr = ifc->address;

  if (addr->family == AF_INET)
    {
      PREFIX_COPY_IPV4(&p, CONNECTED_PREFIX(ifc));
      apply_mask_ipv4 ((struct prefix_ipv4 *) &p);

      if (prefix_ipv4_any ((struct prefix_ipv4 *) &p))
	return;

      rn = bgp_node_get (bgp_connected_table[AFI_IP], (struct prefix *) &p);
      if (rn->info)
	{
	  bc = rn->info;
	  bc->refcnt++;
	}
      else
	{
	  bc = XCALLOC (MTYPE_BGP_CONN, sizeof (struct bgp_connected_ref));
	  bc->refcnt = 1;
	  rn->info = bc;
	}
    }
#ifdef HAVE_IPV6
  else if (addr->family == AF_INET6)
    {
      PREFIX_COPY_IPV6(&p, CONNECTED_PREFIX(ifc));
      apply_mask_ipv6 ((struct prefix_ipv6 *) &p);

      if (IN6_IS_ADDR_UNSPECIFIED (&p.u.prefix6))
	return;

      if (IN6_IS_ADDR_LINKLOCAL (&p.u.prefix6))
	return;

      rn = bgp_node_get (bgp_connected_table[AFI_IP6], (struct prefix *) &p);
      if (rn->info)
	{
	  bc = rn->info;
	  bc->refcnt++;
	}
      else
	{
	  bc = XCALLOC (MTYPE_BGP_CONN, sizeof (struct bgp_connected_ref));
	  bc->refcnt = 1;
	  rn->info = bc;
	}
    }
#endif /* HAVE_IPV6 */
}

void
bgp_connected_delete (struct connected *ifc)
{
  struct prefix p;
  struct prefix *addr;
  struct interface *ifp;
  struct bgp_node *rn;
  struct bgp_connected_ref *bc;

  ifp = ifc->ifp;

  if (if_is_loopback (ifp))
    return;

  addr = ifc->address;

  if (addr->family == AF_INET)
    {
      PREFIX_COPY_IPV4(&p, CONNECTED_PREFIX(ifc));
      apply_mask_ipv4 ((struct prefix_ipv4 *) &p);

      if (prefix_ipv4_any ((struct prefix_ipv4 *) &p))
	return;

      rn = bgp_node_lookup (bgp_connected_table[AFI_IP], &p);
      if (! rn)
	return;

      bc = rn->info;
      bc->refcnt--;
      if (bc->refcnt == 0)
	{
	  XFREE (MTYPE_BGP_CONN, bc);
	  rn->info = NULL;
	}
      bgp_unlock_node (rn);
      bgp_unlock_node (rn);
    }
#ifdef HAVE_IPV6
  else if (addr->family == AF_INET6)
    {
      PREFIX_COPY_IPV6(&p, CONNECTED_PREFIX(ifc));
      apply_mask_ipv6 ((struct prefix_ipv6 *) &p);

      if (IN6_IS_ADDR_UNSPECIFIED (&p.u.prefix6))
	return;

      if (IN6_IS_ADDR_LINKLOCAL (&p.u.prefix6))
	return;

      rn = bgp_node_lookup (bgp_connected_table[AFI_IP6], (struct prefix *) &p);
      if (! rn)
	return;

      bc = rn->info;
      bc->refcnt--;
      if (bc->refcnt == 0)
	{
	  XFREE (MTYPE_BGP_CONN, bc);
	  rn->info = NULL;
	}
      bgp_unlock_node (rn);
      bgp_unlock_node (rn);
    }
#endif /* HAVE_IPV6 */
}

int
bgp_nexthop_self (afi_t afi, struct attr *attr)
{
  struct listnode *node;
  struct listnode *node2;
  struct interface *ifp;
  struct connected *ifc;
  struct prefix *p;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    {
      for (ALL_LIST_ELEMENTS_RO (ifp->connected, node2, ifc))
	{
	  p = ifc->address;

	  if (p && p->family == AF_INET 
	      && IPV4_ADDR_SAME (&p->u.prefix4, &attr->nexthop))
	    return 1;
	}
    }
  return 0;
}

static struct bgp_nexthop_cache *
zlookup_read (void)
{
  struct stream *s;
  uint16_t length;
  u_char marker;
  u_char version;
  uint16_t command;
  int nbytes;
  struct in_addr raddr;
  uint32_t metric;
  int i;
  u_char nexthop_num;
  struct nexthop *nexthop;
  struct bgp_nexthop_cache *bnc;

  s = zlookup->ibuf;
  stream_reset (s);

  nbytes = stream_read (s, zlookup->sock, 2);
  length = stream_getw (s);

  nbytes = stream_read (s, zlookup->sock, length - 2);
  marker = stream_getc (s);
  version = stream_getc (s);
  
  if (version != ZSERV_VERSION || marker != ZEBRA_HEADER_MARKER)
    {
      zlog_err("%s: socket %d version mismatch, marker %d, version %d",
               __func__, zlookup->sock, marker, version);
      return NULL;
    }
    
  command = stream_getw (s);
  
  raddr.s_addr = stream_get_ipv4 (s);
  metric = stream_getl (s);
  nexthop_num = stream_getc (s);

  if (nexthop_num)
    {
      bnc = bnc_new ();
      bnc->valid = 1;
      bnc->metric = metric;
      bnc->nexthop_num = nexthop_num;

      for (i = 0; i < nexthop_num; i++)
	{
	  nexthop = XCALLOC (MTYPE_NEXTHOP, sizeof (struct nexthop));
	  nexthop->type = stream_getc (s);
	  switch (nexthop->type)
	    {
	    case ZEBRA_NEXTHOP_IPV4:
	      nexthop->gate.ipv4.s_addr = stream_get_ipv4 (s);
	      break;
	    case ZEBRA_NEXTHOP_IFINDEX:
	    case ZEBRA_NEXTHOP_IFNAME:
	      nexthop->ifindex = stream_getl (s);
	      break;
            default:
              /* do nothing */
              break;
	    }
	  bnc_nexthop_add (bnc, nexthop);
	}
    }
  else
    return NULL;

  return bnc;
}

static int
zlookup_write_packet (const char *caller, int *socket, const u_char *data, const int nbytes)
{
  int ret;

  if ((ret = writen (*socket, data, nbytes)) <= 0)
    {
      zlog_err ("writing zlookup packet failed in %s, %s", caller,
                ret == 0 ? "connection closed" : "write error");
      close (*socket);
      *socket = -1;
    }
  return ret;
}

struct bgp_nexthop_cache *
zlookup_query (struct in_addr addr)
{
  struct stream *s;

  /* Check socket. */
  if (zlookup->sock < 0)
    return NULL;

  s = zlookup->obuf;
  stream_reset (s);
  zclient_create_header (s, ZEBRA_IPV4_NEXTHOP_LOOKUP);
  stream_put_in_addr (s, &addr);
  
  stream_putw_at (s, 0, stream_get_endp (s));
  
  return zlookup_write_packet (__func__, &zlookup->sock, s->data, stream_get_endp (s)) <= 0 ?
    NULL : zlookup_read ();
}

#ifdef HAVE_IPV6
static struct bgp_nexthop_cache *
zlookup_read_ipv6 (void)
{
  struct stream *s;
  uint16_t length;
  u_char version, marker;
  uint16_t  command;
  int nbytes;
  struct in6_addr raddr;
  uint32_t metric;
  int i;
  u_char nexthop_num;
  struct nexthop *nexthop;
  struct bgp_nexthop_cache *bnc;

  s = zlookup->ibuf;
  stream_reset (s);

  nbytes = stream_read (s, zlookup->sock, 2);
  length = stream_getw (s);

  nbytes = stream_read (s, zlookup->sock, length - 2);
  marker = stream_getc (s);
  version = stream_getc (s);
  
  if (version != ZSERV_VERSION || marker != ZEBRA_HEADER_MARKER)
    {
      zlog_err("%s: socket %d version mismatch, marker %d, version %d",
               __func__, zlookup->sock, marker, version);
      return NULL;
    }
    
  command = stream_getw (s);
  
  stream_get (&raddr, s, 16);

  metric = stream_getl (s);
  nexthop_num = stream_getc (s);

  if (nexthop_num)
    {
      bnc = bnc_new ();
      bnc->valid = 1;
      bnc->metric = metric;
      bnc->nexthop_num = nexthop_num;

      for (i = 0; i < nexthop_num; i++)
	{
	  nexthop = XCALLOC (MTYPE_NEXTHOP, sizeof (struct nexthop));
	  nexthop->type = stream_getc (s);
	  switch (nexthop->type)
	    {
	    case ZEBRA_NEXTHOP_IPV6:
	      stream_get (&nexthop->gate.ipv6, s, 16);
	      break;
	    case ZEBRA_NEXTHOP_IPV6_IFINDEX:
	    case ZEBRA_NEXTHOP_IPV6_IFNAME:
	      stream_get (&nexthop->gate.ipv6, s, 16);
	      nexthop->ifindex = stream_getl (s);
	      break;
	    case ZEBRA_NEXTHOP_IFINDEX:
	    case ZEBRA_NEXTHOP_IFNAME:
	      nexthop->ifindex = stream_getl (s);
	      break;
	    default:
	      /* do nothing */
	      break;
	    }
	  bnc_nexthop_add (bnc, nexthop);
	}
    }
  else
    return NULL;

  return bnc;
}

struct bgp_nexthop_cache *
zlookup_query_ipv6 (struct in6_addr *addr)
{
  struct stream *s;

  /* Check socket. */
  if (zlookup->sock < 0)
    return NULL;

  s = zlookup->obuf;
  stream_reset (s);
  zclient_create_header (s, ZEBRA_IPV6_NEXTHOP_LOOKUP);
  stream_put (s, addr, 16);
  stream_putw_at (s, 0, stream_get_endp (s));

  return zlookup_write_packet (__func__, &zlookup->sock, s->data, stream_get_endp (s)) <= 0 ?
    NULL : zlookup_read_ipv6();
}
#endif /* HAVE_IPV6 */

static int
bgp_import_check (struct prefix *p, u_int32_t *igpmetric,
                  struct in_addr *igpnexthop)
{
  struct stream *s;
  u_int16_t length, command;
  u_char version, marker;
  int nbytes;
  struct in_addr addr;
  struct in_addr nexthop;
  u_int32_t metric = 0;
  u_char nexthop_num;
  u_char nexthop_type;

  /* If lookup connection is not available return valid. */
  if (zlookup->sock < 0)
    {
      if (igpmetric)
	*igpmetric = 0;
      return 1;
    }

  /* Send query to the lookup connection */
  s = zlookup->obuf;
  stream_reset (s);
  zclient_create_header (s, ZEBRA_IPV4_IMPORT_LOOKUP);
  
  stream_putc (s, p->prefixlen);
  stream_put_in_addr (s, &p->u.prefix4);
  
  stream_putw_at (s, 0, stream_get_endp (s));
  
  if (zlookup_write_packet (__func__, &zlookup->sock, s->data, stream_get_endp (s)) <= 0)
    return 1;

  /* Get result. */
  stream_reset (s);

  /* Fetch length. */
  nbytes = stream_read (s, zlookup->sock, 2);
  length = stream_getw (s);

  /* Fetch whole data. */
  nbytes = stream_read (s, zlookup->sock, length - 2);
  marker = stream_getc (s);
  version = stream_getc (s);

  if (version != ZSERV_VERSION || marker != ZEBRA_HEADER_MARKER)
    {
      zlog_err("%s: socket %d version mismatch, marker %d, version %d",
               __func__, zlookup->sock, marker, version);
      return 0;
    }
    
  command = stream_getw (s);
  
  addr.s_addr = stream_get_ipv4 (s);
  metric = stream_getl (s);
  nexthop_num = stream_getc (s);

  /* Set IGP metric value. */
  if (igpmetric)
    *igpmetric = metric;

  /* If there is nexthop then this is active route. */
  if (nexthop_num)
    {
      nexthop.s_addr = 0;
      nexthop_type = stream_getc (s);
      if (nexthop_type == ZEBRA_NEXTHOP_IPV4)
	{
	  nexthop.s_addr = stream_get_ipv4 (s);
	  if (igpnexthop)
	    *igpnexthop = nexthop;
	}
      else
	*igpnexthop = nexthop;

      return 1;
    }
  else
    return 0;
}

/* Scan all configured BGP route then check the route exists in IGP or
   not. */
static int
bgp_import (struct thread *t)
{
  struct bgp *bgp;
  struct bgp_node *rn;
  struct bgp_static *bgp_static;
  struct listnode *node, *nnode;
  int valid;
  u_int32_t metric;
  struct in_addr nexthop;
  afi_t afi;
  safi_t safi;

  bgp_import_thread = 
    thread_add_timer (master, bgp_import, NULL, bgp_import_interval);

  if (BGP_DEBUG (events, EVENTS))
    zlog_debug ("Import timer expired.");

  for (ALL_LIST_ELEMENTS (bm->bgp, node, nnode, bgp))
    {
      for (afi = AFI_IP; afi < AFI_MAX; afi++)
	for (safi = SAFI_UNICAST; safi < SAFI_MPLS_VPN; safi++)
	  for (rn = bgp_table_top (bgp->route[afi][safi]); rn;
	       rn = bgp_route_next (rn))
	    if ((bgp_static = rn->info) != NULL)
	      {
		if (bgp_static->backdoor)
		  continue;

		valid = bgp_static->valid;
		metric = bgp_static->igpmetric;
		nexthop = bgp_static->igpnexthop;

		if (bgp_flag_check (bgp, BGP_FLAG_IMPORT_CHECK)
		    && afi == AFI_IP && safi == SAFI_UNICAST)
		  bgp_static->valid = bgp_import_check (&rn->p, &bgp_static->igpmetric,
						        &bgp_static->igpnexthop);
		else
		  {
		    bgp_static->valid = 1;
		    bgp_static->igpmetric = 0;
		    bgp_static->igpnexthop.s_addr = 0;
		  }

		if (bgp_static->valid != valid)
		  {
		    if (bgp_static->valid)
		      bgp_static_update (bgp, &rn->p, bgp_static, afi, safi);
		    else
		      bgp_static_withdraw (bgp, &rn->p, afi, safi);
		  }
		else if (bgp_static->valid)
		  {
		    if (bgp_static->igpmetric != metric
			|| bgp_static->igpnexthop.s_addr != nexthop.s_addr
			|| bgp_static->rmap.name)
		      bgp_static_update (bgp, &rn->p, bgp_static, afi, safi);
		  }
	      }
    }
  return 0;
}

/* Connect to zebra for nexthop lookup. */
static int
zlookup_connect (struct thread *t)
{
  struct zclient *zlookup;

  zlookup = THREAD_ARG (t);
  zlookup->t_connect = NULL;

  if (zlookup->sock != -1)
    return 0;

  if (zclient_socket_connect (zlookup) < 0)
    return -1;

  return 0;
}

/* Check specified multiaccess next-hop. */
int
bgp_multiaccess_check_v4 (struct in_addr nexthop, char *peer)
{
  struct bgp_node *rn1;
  struct bgp_node *rn2;
  struct prefix p1;
  struct prefix p2;
  struct in_addr addr;
  int ret;

  ret = inet_aton (peer, &addr);
  if (! ret)
    return 0;

  memset (&p1, 0, sizeof (struct prefix));
  p1.family = AF_INET;
  p1.prefixlen = IPV4_MAX_BITLEN;
  p1.u.prefix4 = nexthop;
  memset (&p2, 0, sizeof (struct prefix));
  p2.family = AF_INET;
  p2.prefixlen = IPV4_MAX_BITLEN;
  p2.u.prefix4 = addr;

  /* If bgp scan is not enabled, return invalid. */
  if (zlookup->sock < 0)
    return 0;

  rn1 = bgp_node_match (bgp_connected_table[AFI_IP], &p1);
  if (! rn1)
    return 0;
  bgp_unlock_node (rn1);
  
  rn2 = bgp_node_match (bgp_connected_table[AFI_IP], &p2);
  if (! rn2)
    return 0;
  bgp_unlock_node (rn2);

  /* This is safe, even with above unlocks, since we are just
     comparing pointers to the objects, not the objects themselves. */
  if (rn1 == rn2)
    return 1;

  return 0;
}

DEFUN (bgp_scan_time,
       bgp_scan_time_cmd,
       "bgp scan-time <5-60>",
       "BGP specific commands\n"
       "Configure background scanner interval\n"
       "Scanner interval (seconds)\n")
{
  bgp_scan_interval = atoi (argv[0]);

  if (bgp_scan_thread)
    {
      thread_cancel (bgp_scan_thread);
      bgp_scan_thread = 
	thread_add_timer (master, bgp_scan_timer, NULL, bgp_scan_interval);
    }

  return CMD_SUCCESS;
}

DEFUN (no_bgp_scan_time,
       no_bgp_scan_time_cmd,
       "no bgp scan-time",
       NO_STR
       "BGP specific commands\n"
       "Configure background scanner interval\n")
{
  bgp_scan_interval = BGP_SCAN_INTERVAL_DEFAULT;

  if (bgp_scan_thread)
    {
      thread_cancel (bgp_scan_thread);
      bgp_scan_thread = 
	thread_add_timer (master, bgp_scan_timer, NULL, bgp_scan_interval);
    }

  return CMD_SUCCESS;
}

ALIAS (no_bgp_scan_time,
       no_bgp_scan_time_val_cmd,
       "no bgp scan-time <5-60>",
       NO_STR
       "BGP specific commands\n"
       "Configure background scanner interval\n"
       "Scanner interval (seconds)\n")

static int
show_ip_bgp_scan_tables (struct vty *vty, const char detail)
{
  struct bgp_node *rn;
  struct bgp_nexthop_cache *bnc;
  struct nexthop *nexthop;
  char buf[INET6_ADDRSTRLEN];

  if (bgp_scan_thread)
    vty_out (vty, "BGP scan is running%s", VTY_NEWLINE);
  else
    vty_out (vty, "BGP scan is not running%s", VTY_NEWLINE);
  vty_out (vty, "BGP scan interval is %d%s", bgp_scan_interval, VTY_NEWLINE);

  vty_out (vty, "Current BGP nexthop cache:%s", VTY_NEWLINE);
  for (rn = bgp_table_top (bnct_active (AFI_IP)); rn; rn = bgp_route_next (rn))
    if ((bnc = rn->info) != NULL)
      {
	if (bnc->valid)
	{
	  vty_out (vty, " %s valid [IGP metric %d]%s",
		   inet_ntop (AF_INET, &rn->p.u.prefix4, buf, INET6_ADDRSTRLEN), bnc->metric, VTY_NEWLINE);
	  if (detail)
	    for (nexthop = bnc->nexthop; nexthop; nexthop = nexthop->next)
	      switch (nexthop->type)
	      {
	      case NEXTHOP_TYPE_IPV4:
		vty_out (vty, "  gate %s%s", inet_ntop (AF_INET, &nexthop->gate.ipv4, buf, INET6_ADDRSTRLEN), VTY_NEWLINE);
		break;
	      case NEXTHOP_TYPE_IFINDEX:
		vty_out (vty, "  ifidx %u%s", nexthop->ifindex, VTY_NEWLINE);
		break;
	      default:
		vty_out (vty, "  invalid nexthop type %u%s", nexthop->type, VTY_NEWLINE);
	      }
	}
	else
	  vty_out (vty, " %s invalid%s",
		   inet_ntop (AF_INET, &rn->p.u.prefix4, buf, INET6_ADDRSTRLEN), VTY_NEWLINE);
      }

#ifdef HAVE_IPV6
  {
    for (rn = bgp_table_top (bnct_active (AFI_IP6));
         rn; 
         rn = bgp_route_next (rn))
      if ((bnc = rn->info) != NULL)
	{
	  if (bnc->valid)
	  {
	    vty_out (vty, " %s valid [IGP metric %d]%s",
		     inet_ntop (AF_INET6, &rn->p.u.prefix6, buf, INET6_ADDRSTRLEN),
		     bnc->metric, VTY_NEWLINE);
	    if (detail)
	      for (nexthop = bnc->nexthop; nexthop; nexthop = nexthop->next)
		switch (nexthop->type)
		{
		case NEXTHOP_TYPE_IPV6:
		  vty_out (vty, "  gate %s%s", inet_ntop (AF_INET6, &nexthop->gate.ipv6, buf, INET6_ADDRSTRLEN), VTY_NEWLINE);
		  break;
		case NEXTHOP_TYPE_IFINDEX:
		  vty_out (vty, "  ifidx %u%s", nexthop->ifindex, VTY_NEWLINE);
		  break;
		default:
		  vty_out (vty, "  invalid nexthop type %u%s", nexthop->type, VTY_NEWLINE);
		}
	  }
	  else
	    vty_out (vty, " %s invalid%s",
		     inet_ntop (AF_INET6, &rn->p.u.prefix6, buf, INET6_ADDRSTRLEN),
		     VTY_NEWLINE);
	}
  }
#endif /* HAVE_IPV6 */

  vty_out (vty, "BGP connected route:%s", VTY_NEWLINE);
  for (rn = bgp_table_top (bgp_connected_table[AFI_IP]); 
       rn; 
       rn = bgp_route_next (rn))
    if (rn->info != NULL)
      vty_out (vty, " %s/%d%s", inet_ntoa (rn->p.u.prefix4), rn->p.prefixlen,
	       VTY_NEWLINE);

#ifdef HAVE_IPV6
  {
    for (rn = bgp_table_top (bgp_connected_table[AFI_IP6]); 
         rn; 
         rn = bgp_route_next (rn))
      if (rn->info != NULL)
	vty_out (vty, " %s/%d%s",
		 inet_ntop (AF_INET6, &rn->p.u.prefix6, buf, INET6_ADDRSTRLEN),
		 rn->p.prefixlen,
		 VTY_NEWLINE);
  }
#endif /* HAVE_IPV6 */

  return CMD_SUCCESS;
}

DEFUN (show_ip_bgp_scan,
       show_ip_bgp_scan_cmd,
       "show ip bgp scan",
       SHOW_STR
       IP_STR
       BGP_STR
       "BGP scan status\n")
{
  return show_ip_bgp_scan_tables (vty, 0);
}

DEFUN (show_ip_bgp_scan_detail,
       show_ip_bgp_scan_detail_cmd,
       "show ip bgp scan detail",
       SHOW_STR
       IP_STR
       BGP_STR
       "BGP scan status\n"
       "More detailed output\n")
{
  return show_ip_bgp_scan_tables (vty, 1);
}

int
bgp_config_write_scan_time (struct vty *vty)
{
  if (bgp_scan_interval != BGP_SCAN_INTERVAL_DEFAULT)
    vty_out (vty, " bgp scan-time %d%s", bgp_scan_interval, VTY_NEWLINE);
  return CMD_SUCCESS;
}

void
bgp_scan_init (void)
{
  zlookup = zclient_new ();
  zlookup->sock = -1;
  zlookup->t_connect = thread_add_event (master, zlookup_connect, zlookup, 0);

  bgp_scan_interval = BGP_SCAN_INTERVAL_DEFAULT;
  bgp_import_interval = BGP_IMPORT_INTERVAL_DEFAULT;

  bnct_init (AFI_IP);
  bgp_connected_table[AFI_IP] = bgp_table_init (AFI_IP, SAFI_UNICAST);

#ifdef HAVE_IPV6
  bnct_init (AFI_IP6);
  bgp_connected_table[AFI_IP6] = bgp_table_init (AFI_IP6, SAFI_UNICAST);
#endif /* HAVE_IPV6 */

  /* Make BGP scan thread. */
  bgp_scan_thread = thread_add_timer (master, bgp_scan_timer, 
                                      NULL, bgp_scan_interval);
  /* Make BGP import there. */
  bgp_import_thread = thread_add_timer (master, bgp_import, NULL, 0);

  install_element (BGP_NODE, &bgp_scan_time_cmd);
  install_element (BGP_NODE, &no_bgp_scan_time_cmd);
  install_element (BGP_NODE, &no_bgp_scan_time_val_cmd);
  install_element (VIEW_NODE, &show_ip_bgp_scan_cmd);
  install_element (VIEW_NODE, &show_ip_bgp_scan_detail_cmd);
  install_element (RESTRICTED_NODE, &show_ip_bgp_scan_cmd);
  install_element (ENABLE_NODE, &show_ip_bgp_scan_cmd);
  install_element (ENABLE_NODE, &show_ip_bgp_scan_detail_cmd);
}

void
bgp_scan_finish (void)
{
  /* Only the current one needs to be reset. */
  bgp_nexthop_cache_reset (bnct_active (AFI_IP));
  bnct_finish (AFI_IP);
  bgp_table_unlock (bgp_connected_table[AFI_IP]);
  bgp_connected_table[AFI_IP] = NULL;

#ifdef HAVE_IPV6
  /* Only the current one needs to be reset. */
  bgp_nexthop_cache_reset (bnct_active (AFI_IP6));
  bnct_finish (AFI_IP6);
  bgp_table_unlock (bgp_connected_table[AFI_IP6]);
  bgp_connected_table[AFI_IP6] = NULL;
#endif /* HAVE_IPV6 */
}
