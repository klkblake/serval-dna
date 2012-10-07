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
#include <netinet/in.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include "serval.h"

struct reachable_peer {
  unsigned char addr_len;
  unsigned char addr[32];
  unsigned char tq_avg;
};

#ifndef RTF_UP
/* Keep this in sync with /usr/src/linux/include/linux/route.h */
#define RTF_UP          0x0001	/* route usable                 */
#define RTF_GATEWAY     0x0002	/* destination is a gateway     */
#define RTF_HOST        0x0004	/* host entry (net otherwise)   */
#define RTF_REINSTATE   0x0008	/* reinstate route after tmout  */
#define RTF_DYNAMIC     0x0010	/* created dyn. (by redirect)   */
#define RTF_MODIFIED    0x0020	/* modified dyn. (by redirect)  */
#define RTF_MTU         0x0040	/* specific MTU for this route  */
#ifndef RTF_MSS
#define RTF_MSS         RTF_MTU	/* Compatibility :-(            */
#endif
#define RTF_WINDOW      0x0080	/* per route window clamping    */
#define RTF_IRTT        0x0100	/* Initial round trip time      */
#define RTF_REJECT      0x0200	/* Reject route                 */
#endif

int readRoutingTable(struct in_addr peers[],int *peer_count,int peer_max){
  char devname[64];
  unsigned long d, g, m;
  int flgs, ref, use, metric, mtu, win, ir;
  
  if (debug&DEBUG_PEERS) DEBUG("Reading routing table");
  
  FILE *fp = fopen("/proc/net/route","r");
  if (!fp) return -1;
  
  if (debug&DEBUG_PEERS) DEBUG("Skipping line");
  if (fscanf(fp, "%*[^\n]\n") < 0)
    goto ERROR;
  
  while(1){
    int r;
    if (debug&DEBUG_PEERS) DEBUG("Reading next route");
    r = fscanf(fp, "%63s%lx%lx%X%d%d%d%lx%d%d%d\n",
	       devname, &d, &g, &flgs, &ref, &use, &metric, &m,
	       &mtu, &win, &ir);
    
    if (r != 11) {
      if ((r < 0) && feof(fp)) { /* EOF with no (nonspace) chars read. */
	if (debug&DEBUG_PEERS) DEBUG("eof");
	break;
      }
    ERROR:
      fclose(fp);
      return WHY("Unable to parse routing table");
    }
    
    if (!(flgs & RTF_UP)) { /* Skip interfaces that are down. */
      if (debug&DEBUG_PEERS) DEBUGF("Skipping down interface %s",devname);
      continue;
    }
    
    if (m!=0xFFFFFFFF){
      /* Netmask indicates a network, so calculate broadcast address */
      d=(d&m)|(0xffffffff^m);
      if (debug&DEBUG_PEERS) DEBUGF("Adding broadcast address %08lx",d);
    }
    
    if (*peer_count<peer_max)	peers[(*peer_count)++]
.s_addr=d;
    if (debug&DEBUG_PEERS) DEBUGF("Found peer %08lx from routing table",d);
  }
  fclose(fp);
  return 0;
}

int readArpTable(struct in_addr peers[],int *peer_count,int peer_max){
  unsigned long d;
  int q1,q2,q3,q4;

  if (debug&DEBUG_PEERS) DEBUG("Reading ARP table");
  
  FILE *fp = fopen("/proc/net/arp","r");
  if (!fp) return -1;
  
  if (debug&DEBUG_PEERS) DEBUG("Skipping line");
  if (fscanf(fp, "%*[^\n]\n") < 0)
    goto ERROR;
  
  while(1){
    int r;
    r = fscanf(fp, "%d.%d.%d.%d%*[^\n]\n",
	       &q1,&q2,&q3,&q4);
    if (debug&DEBUG_PEERS) DEBUGF("Reading next arp entry (r=%d, %d.%d.%d.%d)",r,q1,q2,q3,q4);
    
    d = (q1&0xff)
      +((q2&0xff)<<8)
      +((q3&0xff)<<16)
      +((q4&0xff)<<24);

    if (r != 4) {
      if ((r < 0) && feof(fp)) { /* EOF with no (nonspace) chars read. */
	if (debug&DEBUG_PEERS) DEBUG("eof");
	break;
      }
    ERROR:
      fclose(fp);
      return WHY("Unable to parse arp table");
    }
        
    if (*peer_count<peer_max)	peers[(*peer_count)++].s_addr=d;
    if (debug&DEBUG_PEERS) DEBUGF("Found peer %08lx from ARP table",d);
  }
  fclose(fp);
  return 0;
}

int readBatmanPeerFile(char *file_path,struct in_addr peers[],int *peer_count,int peer_max)
{
  /* Shiny new code to read the flat file containing peer list */
  FILE *f;
  unsigned int offset=0;
  unsigned int timestamp=0;
  struct reachable_peer p;

  f=fopen(file_path,"r");
  if (!f) {
    WHY_perror("fopen");
    return WHYF("Failed to open peer list file `%s'",file_path);
  }

  if (fread(&offset,sizeof(offset),1,f)!=1) {
    WHY_perror("fread");
    fclose(f);
    return WHYF("Failed to read peer list offset from `%s'",file_path);
  }
  offset=ntohl(offset);

  if (fseek(f,offset,SEEK_SET)) {
    WHY_perror("fseek");
    fclose(f);
    return WHYF("Failed to seek to peer list offset 0x%x in `%s'",offset,file_path);
  }
  
  if (fread(&timestamp,sizeof(timestamp),1,f)!=1) { 
    WHY_perror("fread");
    fclose(f);
    return WHYF("Failed to read peer list timestamp from `%s'",file_path);
  }
  timestamp=ntohl(timestamp);  

  if (timestamp<(time(0)-3)) {
    if (debug&DEBUG_PEERS)
      DEBUGF("Ignoring stale BATMAN peer list (%d seconds old)",(int)(time(0)-timestamp));
    fclose(f);
    return -1;
  }

  while(fread(&p,sizeof(p),1,f)==1) {
    struct in_addr i;
    if (!p.addr_len) break;
    union { char c[4]; uint32_t ui32; } *u = (void*)&p.addr[0];
    i.s_addr = u->ui32;
    if (*peer_count<peer_max)	peers[(*peer_count)++]=i;
    if (debug&DEBUG_PEERS) DEBUGF("Found BATMAN peer '%s'",inet_ntoa(i));
  }

  if (fclose(f) == EOF)
    WHY_perror("fclose");
  return 0;
}

int getBatmanPeerList(char *socket_path,struct in_addr peers[],int *peer_count,int peer_max)
{
#ifdef WIN32
	return -1;
#else
  int sock;
  struct sockaddr_un socket_address;
  unsigned char buf[16384]; /* big enough for a gigabit jumbo frame or loopback godzilla-gram */
  int ofs=0;
  int bytes=0;
  struct pollfd fds;
  int notDone=1;
  int res;
  int tries=0;

 askagain:

  /* Make socket */
  sock=socket(PF_UNIX,SOCK_STREAM,0);
  memset(&socket_address,0,sizeof(struct sockaddr_un));
  socket_address.sun_family=PF_UNIX;
  if (strlen(socket_path)>256) return WHY("BATMAN socket path too long");
  strcpy(socket_address.sun_path,socket_path);

  /* Connect the socket */
  if (connect(sock,(struct sockaddr*)&socket_address,sizeof(socket_address))<0)
    return WHY("connect() to BATMAN socket failed.");

  char cmd[30];
  snprintf(cmd, sizeof cmd, "d:%c", 1);  
  cmd[sizeof cmd - 1] = '\0';
  if (write(sock,cmd,30) != 30) {
    close(sock);
    return WHY("write() command failed to BATMAN socket.");
  }

  fds.fd=sock;
  fds.events=POLLIN;

 getmore:

  while(notDone)
    {      
      switch (poll(&fds,1,1500)) {
      case 1: /* Excellent - we have a response */ break;
      case 0: if (debug&DEBUG_PEERS) DEBUGF("BATMAN did not respond to peer enquiry");
	close(sock);
	if (tries++<=3) goto askagain;
	return WHY("No response from BATMAN.");
      default: /* some sort of error, e.g., lost connection */
	close(sock);
	return WHY("poll() of BATMAN socket failed.");
	}
      
      res=read(sock,&buf[bytes],16383-bytes); close(sock);
      ofs=0;
      if (res<1) {
	if (bytes)
	  {
	    /* Got a partial response, then a dead line.
	       Should probably ask again unless we have tried too many times.
	    */
	    if (debug&DEBUG_PEERS) DEBUGF("Trying again after cold drop");
	    close(sock);
	    bytes=0;
	    if (tries++<=3) goto askagain;
	    else return WHY("failed to read() from BATMAN socket (too many tries).");
	  }
	return WHY("failed to read() from BATMAN socket.");
      }
      if (!res) return 0;
      if (debug&DEBUG_PEERS) DEBUGF("BATMAN has responded with %d bytes",res);
      
      if (debug&DEBUG_PEERS) dump("BATMAN says",&buf[bytes],res);
      
      if (res<80 /*||buf[bytes]!='B' -- turns out we can't rely on this, either */
	  ||buf[bytes+res-1]!=0x0a||buf[bytes+res-4]!='E')
	{
	  /* Jolly BATMAN on Android sometimes sends us bung packets from time to
	     time.  Sometimes it is fragmenting, other times it is just plain
	     odd.
	     If this happens, we should just ask again.
	     We should also try to reassemble fragments.
	     
	     Sometimes we get cold-drops and resumes, too.  Yay.
	  */
	  if (buf[bytes+res-4]!='E') {
	    /* no end marker, so try adding record to the end. */
	    if (debug&DEBUG_PEERS) DEBUGF("Data has no end marker, accumulating");
	    bytes+=res;
	    goto getmore;
	  }
	  close(sock);
	  goto askagain;
	}
      bytes+=res;
      
      while(ofs<bytes)
	{	  
	  if(debug&DEBUG_PEERS) DEBUGF("New line @ %d",ofs);
	  /* Check for IP address of peers */
	  if (isdigit(buf[ofs]))
	    {
	      int i;
	      for(i=0;ofs+i<bytes;i++)
		if (buf[i+ofs]==' ') { 
		  buf[i+ofs]=0;
		  if (*peer_count<peer_max) peers[(*peer_count)++].s_addr=inet_addr((char *)&buf[ofs]);
		  if (debug&DEBUG_PEERS) DEBUGF("Found BATMAN peer '%s'",&buf[ofs]);
		  buf[ofs+i]=' ';
		  break; 
		}	    
	    }
	  /* Check for end of transmission */
	  if (buf[ofs]=='E') { notDone=0; }
	  
	  /* Skip to next line */
	  while(buf[ofs]>=' '&&(ofs<bytes)) ofs++;
	  while(buf[ofs]<' '&&(ofs<bytes)) ofs++;
	}
      bytes=0;
    }
  return 0;
#endif
}
