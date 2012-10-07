/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <time.h>
#include "serval.h"
#include "strbuf.h"
#include "overlay_buffer.h"
#include "overlay_packet.h"

/*
  Many phones have a broadcast reception filter. This makes peer discovery 
  difficult for when we use AP/client mode, e.g., on un-rooted phones, because
  the peers never find each other.

  This work around reads the ARP table periodically and pokes (potentially a
  subset of) that list.  That will (hopefully) allow the client reachability code
  to realise the presence of the node, and record unicast reachability for them.

  Rhizome advertisement will not work, so we will actually create a rhizome
  advertisement packet so that bundles still get advertised.
*/

void overlay_poke_arp_peers()
{
  static struct sched_ent alarm;

#define MAX_ARP_PEERS 256
  struct in_addr peers[MAX_ARP_PEERS];
  int peer_count=0;

  /* Read list of peers from ARP table */
  readArpTable(peers,&peer_count,MAX_ARP_PEERS);

  DEBUGF("Discovered %d peers by ARP.",peer_count);

  bzero(&alarm,sizeof(alarm));
  alarm.alarm =  gettime_ms()+ 1000;
  alarm.deadline = gettime_ms()+ 1000;
  alarm.function = overlay_poke_arp_peers;
  unschedule(&alarm);
  schedule(&alarm);

  return;
}
