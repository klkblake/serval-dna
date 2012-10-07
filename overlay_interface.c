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

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

int overlay_ready=0;
int overlay_interface_count=0;
overlay_interface overlay_interfaces[OVERLAY_MAX_INTERFACES];
int overlay_last_interface_number=-1;

struct interface_rules {
  char *namespec;
  unsigned long long speed_in_bits;
  int port;
  char type;
  char excludeP;
  struct interface_rules *next;
};

struct interface_rules *interface_filter=NULL;

struct profile_total interface_poll_stats;
struct profile_total dummy_poll_stats;

struct sched_ent sock_any;
struct sockaddr_in sock_any_addr;
struct profile_total sock_any_stats;

struct sched_ent next_packet;
struct profile_total send_packet;

static int overlay_tick_interface(int i, time_ms_t now);
static void overlay_interface_poll(struct sched_ent *alarm);
static void		logServalPacket(int level, struct __sourceloc where, const char *message, const unsigned char *packet, size_t len);
static long long	parse_quantity(char *q);

unsigned char magic_header[]={/* Magic */ 'O',0x10,
  /* Version */ 0x00,0x01};


static int overlay_interface_type(char *s)
{
  if (!strcasecmp(s,"ethernet")) return OVERLAY_INTERFACE_ETHERNET;
  if (!strcasecmp(s,"wifi")) return OVERLAY_INTERFACE_WIFI;
  if (!strcasecmp(s,"other")) return OVERLAY_INTERFACE_UNKNOWN;
  if (!strcasecmp(s,"catear")) return OVERLAY_INTERFACE_PACKETRADIO;
  return WHY("Invalid interface type -- consider using 'wifi','ethernet' or 'other'");
}

int overlay_interface_arg(char *arg)
{
  /* Parse an interface argument, of the form:

     <+|->[interfacename][=type]

     +interface tells DNA to sit on that interface
     -interface tells DNA to not sit on that interface
     +/- without an interface tells DNA to sit on all interfaces.

     The first match rules, so -en0+ tells DNA to use all interfaces, excepting en0

     The optional =type specifier tells DNA how to handle the interface in terms of
     bandwidth:distance relationship for calculating tick times etc.

     The special type =custom allows full specification:
     
     XXX - Settle the custom specification now that we have changed the interface
     management.
  */

  char sign[80]="+";
  char interface_name[80]="";
  char speed[80]="1m";
  char typestring[80]="wifi";
  int port=PORT_DNA;
  int type=OVERLAY_INTERFACE_UNKNOWN;
  int n=0;

  /* Too long */
  if (strlen(arg)>79) return WHY("interface specification was >79 characters");

  struct interface_rules *r=calloc(sizeof(struct interface_rules),1);
  if (!r) return WHY("calloc(struct interface rules),1) failed");


  if (sscanf(arg,"%[+-]%n%[^=:,]%n=%[^:]%n:%d%n:%[^:]%n",
	     sign,&n,interface_name,&n,typestring,&n,&port,&n,speed,&n)>=1)
    {
      if (n<strlen(arg)) { free(r); return WHY("Extra junk at end of interface specification"); }

      if (strlen(sign)>1) { free(r); return WHY("Sign must be + or -"); }
      switch(sign[0])
	{
	case '+': break;
	case '-': r->excludeP=1; break;
	default: 
	  free(r);
	  return WHY("Invalid interface list item: Must begin with + or -");
	}

      long long speed_in_bits=parse_quantity(speed);
      if (speed_in_bits<=1) {
	free(r);
	return WHY("Interfaces must be capable of at least 1 bit per second");
      }
      if (n<strlen(arg)) return WHY("Extra stuff at end of interface specification");

      type=overlay_interface_type(typestring);
      if (type<0) { free(r); return WHY("Invalid interface type in specification"); }

      /* Okay, register the interface preference */
      r->namespec=strdup(interface_name);
      r->speed_in_bits=speed_in_bits;
      r->port=port;
      r->type=type;
      
      r->next=interface_filter;
      interface_filter=r;

      return 0;
    }
  else { free(r); return WHY("Bad interface specification"); }
}

int overlay_interface_args(const char *arg)
{
  /* Parse series of comma-separated interface definitions from a single argument
   */
  int i=0;
  char interface[80];
  int len=0;

  for(i=0;arg[i];i++)
    {
      if (arg[i]==','||arg[i]=='\n') {
	interface[len]=0;
	if (overlay_interface_arg(interface)) return WHY("Could not add interface");
	len=0;
      } else {
	if (len<79) {
	  interface[len++]=arg[i];
	  interface[len]=0;
	} else 
	  return WHY("Interface definition is too long (each must be <80 characters)");
      }
    }
  if (len) if (overlay_interface_arg(interface)) return WHY("Could not add final interface");
  return 0;     
}

static void
overlay_interface_close(overlay_interface *interface){
  if (interface->fileP){
    INFOF("Interface %s is down", interface->name);
  }else{
    INFOF("Interface %s addr %s is down", interface->name, inet_ntoa(interface->broadcast_address.sin_addr));
  }
  unschedule(&interface->alarm);
  unwatch(&interface->alarm);
  close(interface->alarm.poll.fd);
  interface->alarm.poll.fd=-1;
  interface->state=INTERFACE_STATE_DOWN;
}

// create a socket with options common to all our UDP sockets
static int
overlay_bind_socket(const struct sockaddr *addr, size_t addr_size, char *interface_name){
  int fd;
  int reuseP = 1;
  int broadcastP = 1;
  
  fd = socket(PF_INET,SOCK_DGRAM,0);
  if (fd < 0) {
    WHY_perror("Error creating socket");
    return -1;
  } 
  
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseP, sizeof(reuseP)) < 0) {
    WHY_perror("setsockopt(SO_REUSEADR)");
    goto error;
  }
  
  #ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseP, sizeof(reuseP)) < 0) {
      WHY_perror("setsockopt(SO_REUSEPORT)");
      goto error;
    }
  #endif
  
  if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcastP, sizeof(broadcastP)) < 0) {
    WHY_perror("setsockopt(SO_BROADCAST)");
    goto error;
  }
  
  /* Automatically close socket on calls to exec().
   This makes life easier when we restart with an exec after receiving
   a bad signal. */
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, NULL) | 
#ifdef FD_CLOEXEC
						FD_CLOEXEC
#else
						O_CLOEXEC
#endif
	);
  
#ifdef SO_BINDTODEVICE
  /*
   Limit incoming and outgoing packets to this interface, no matter what the routing table says.
   This should allow for a device with multiple interfaces on the same subnet.
   Don't abort if this fails, I believe it requires root, just log it.
   */
  if (interface_name && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name, strlen(interface_name)+1) < 0) {
    WHY_perror("setsockopt(SO_BINDTODEVICE)");
  }
#endif

  if (bind(fd, addr, addr_size)) {
    WHY_perror("Bind failed");
    goto error;
  }
  
  return fd;
  
error:
  close(fd);
  return -1;
}

overlay_interface * overlay_interface_find(struct in_addr addr){
  int i;
  for (i=0;i<OVERLAY_MAX_INTERFACES;i++){
    if (overlay_interfaces[i].state!=INTERFACE_STATE_UP)
      continue;
    if ((overlay_interfaces[i].netmask.s_addr & addr.s_addr) == (overlay_interfaces[i].netmask.s_addr & overlay_interfaces[i].address.sin_addr.s_addr)){
      return &overlay_interfaces[i];
    }
  }
  
  return NULL;
}

overlay_interface * overlay_interface_find_name(const char *name){
  int i;
  for (i=0;i<OVERLAY_MAX_INTERFACES;i++){
    if (overlay_interfaces[i].state!=INTERFACE_STATE_UP)
      continue;
    if (strcasecmp((*name=='>'?name+1:name), 
		   (*overlay_interfaces[i].name=='>'?overlay_interfaces[i].name+1:overlay_interfaces[i].name)
		   )==0){
      return &overlay_interfaces[i];
    }
  }
  return NULL;
}

// OSX doesn't recieve broadcast packets on sockets bound to an interface's address
// So we have to bind a socket to INADDR_ANY to receive these packets.
static void
overlay_interface_read_any(struct sched_ent *alarm){
  if (alarm->poll.revents & POLLIN) {
    int plen=0;
    int recvttl=1;
    unsigned char packet[16384];
    overlay_interface *interface=NULL;
    struct sockaddr src_addr;
    socklen_t addrlen = sizeof(src_addr);
    
    /* Read only one UDP packet per call to share resources more fairly, and also
     enable stats to accurately count packets received */
    plen = recvwithttl(alarm->poll.fd, packet, sizeof(packet), &recvttl, &src_addr, &addrlen);
    if (plen == -1) {
      WHY_perror("recvwithttl(c)");
      unwatch(alarm);
      close(alarm->poll.fd);
      return;
    }
    
    struct in_addr src = ((struct sockaddr_in *)&src_addr)->sin_addr;
    
    /* Try to identify the real interface that the packet arrived on */
    interface = overlay_interface_find(src);
    
    /* Drop the packet if we don't find a match */
    if (!interface){
      if (debug&DEBUG_OVERLAYINTERFACES)
	DEBUGF("Could not find matching interface for packet received from %s", inet_ntoa(src));
      return;
    }
    
    /* We have a frame from this interface */
    if (debug&DEBUG_PACKETRX)
      DEBUG_packet_visualise("Read from real interface", packet,plen);
    if (debug&DEBUG_OVERLAYINTERFACES) 
      DEBUGF("Received %d bytes from %s on interface %s (ANY)",plen, 
	     inet_ntoa(((struct sockaddr_in *)&src_addr)->sin_addr),
	     interface->name);
    if (packetOk(interface,packet,plen,NULL,recvttl,&src_addr,addrlen,1)) {
      WHY("Malformed packet");
    }
  }
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    INFO("Closing broadcast socket due to error");
    unwatch(alarm);
    close(alarm->poll.fd);
    alarm->poll.fd=-1;
  }  
}

// bind a socket to INADDR_ANY:port
// for now, we don't have a graceful close for this interface but it should go away when the process dies
static int overlay_interface_init_any(int port){
  struct sockaddr_in addr;
  
  if (sock_any.poll.fd>0){
    // Check the port number matches
    if (sock_any_addr.sin_port != htons(port))
      return WHYF("Unable to listen to broadcast packets for ports %d & %d", port, ntohs(sock_any_addr.sin_port));
    
    return 0;
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  sock_any.poll.fd = overlay_bind_socket((const struct sockaddr *)&addr, sizeof(addr), NULL);
  if (sock_any.poll.fd<0)
    return -1;
  
  sock_any_addr = addr;
  
  sock_any.poll.events=POLLIN;
  sock_any.function = overlay_interface_read_any;
  
  sock_any_stats.name="overlay_interface_read_any";
  sock_any.stats=&sock_any_stats;
  watch(&sock_any);
  return 0;
}

static int
overlay_interface_init_socket(int interface_index)
{
  overlay_interface *const interface = &overlay_interfaces[interface_index];
  interface->fileP = 0;

  /*
   On linux you can bind to the broadcast address to receive broadcast packets per interface [or subnet],
   but then you can't receive unicast packets on the same socket.
   
   On osx, you can only receive broadcast packets if you bind to INADDR_ANY.
   
   So the most portable way to do this is to bind to each interface's IP address for sending broadcasts 
   and receiving unicasts, and bind a separate socket to INADDR_ANY just for receiving broadcast packets.
   
   Sending packets from INADDR_ANY would probably work, but gives us less control over which interfaces are sending packets.
   But there may be some platforms that need some other combination for everything to work.
   */
  
  overlay_interface_init_any(interface->port);
  
  const struct sockaddr *addr = (const struct sockaddr *)&interface->address;
  
  interface->alarm.poll.fd = overlay_bind_socket(addr, sizeof(interface->broadcast_address), interface->name);
  if (interface->alarm.poll.fd<0){
    interface->state=INTERFACE_STATE_DOWN;
    return -1;
  }
  
  if (debug & (DEBUG_PACKETRX | DEBUG_IO)){
    char srctxt[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, (const void *)&interface->broadcast_address.sin_addr, srctxt, INET_ADDRSTRLEN))
      DEBUGF("Bound to %s:%d", srctxt, ntohs(interface->broadcast_address.sin_port));
  }

  interface->alarm.poll.events=POLLIN;
  interface->alarm.function = overlay_interface_poll;
  
  interface_poll_stats.name="overlay_interface_poll";
  interface->alarm.stats=&interface_poll_stats;
  watch(&interface->alarm);

  if (interface->tick_ms>0){
    // run the first tick asap
    interface->alarm.alarm=gettime_ms();
    interface->alarm.deadline=interface->alarm.alarm+10;
    schedule(&interface->alarm);
  }
  
  interface->state=INTERFACE_STATE_UP;
  
  INFOF("Interface %s addr %s, is up",interface->name, inet_ntoa(interface->broadcast_address.sin_addr));
  
  // mark our sid to be sent in full
  if (my_subscriber)
    my_subscriber->send_full = 1;
  directory_registration();
  
  return 0;
}

static int
overlay_interface_init(char *name, struct in_addr src_addr, struct in_addr netmask,
		       struct in_addr broadcast,
		       int speed_in_bits, int port, int type)
{
  /* Too many interfaces */
  if (overlay_interface_count>=OVERLAY_MAX_INTERFACES) return WHY("Too many interfaces -- Increase OVERLAY_MAX_INTERFACES");

  overlay_interface *const interface = &overlay_interfaces[overlay_interface_count];

  strcpy(interface->name,name);

  /* Pick a reasonable default MTU.
     This will ultimately get tuned by the bandwidth and other properties of the interface */
  interface->mtu=1200;
  interface->state=INTERFACE_STATE_DOWN;
  interface->bits_per_second=speed_in_bits;
  interface->port=port;
  interface->type=type;
  interface->last_tick_ms= -1; // not ticked yet
  interface->alarm.poll.fd=0;
  
  // how often do we announce ourselves on this interface?
  switch (type) {
  case OVERLAY_INTERFACE_PACKETRADIO:
    interface->tick_ms = confValueGetInt64Range("mdp.packetradio.tick_ms", 15000LL, 1LL, 3600000LL);
    break;
  case OVERLAY_INTERFACE_ETHERNET:
    interface->tick_ms = confValueGetInt64Range("mdp.ethernet.tick_ms", 500LL, 1LL, 3600000LL);
    break;
  case OVERLAY_INTERFACE_WIFI:
    interface->tick_ms = confValueGetInt64Range("mdp.wifi.tick_ms", 500LL, 1LL, 3600000LL);
    break;
  case OVERLAY_INTERFACE_UNKNOWN:
    interface->tick_ms = confValueGetInt64Range("mdp.unknown.tick_ms", 500LL, 1LL, 3600000LL);
    break;
  default:
    return WHYF("Unsupported interface type %d", type);
  }

  // allow for a per interface override of tick interval
  {
    char option_name[64];
    snprintf(option_name, sizeof(option_name), "mdp.%s.tick_ms", (*name=='>'?name+1:name));
    interface->tick_ms = confValueGetInt64Range(option_name, interface->tick_ms, 1LL, 3600000LL);
  }
  
  // disable announcements and other broadcasts if tick_ms=0. 
  if (interface->tick_ms>0)
    interface->send_broadcasts=1;
  else{
    interface->send_broadcasts=0;
    INFOF("Interface %s is running tickless", name);
  }
  
  if (name[0]=='>') {
    interface->fileP=1;
    char dummyfile[1024];
    if (name[1]=='/') {
      /* Absolute path */
      snprintf(dummyfile, sizeof(dummyfile), "%s", &name[1]);
    } else {
      const char *interface_folder = confValueGet("interface.folder", serval_instancepath());
      snprintf(dummyfile, sizeof(dummyfile), "%s/%s", interface_folder, &name[1]);
    }
    
    if ((interface->alarm.poll.fd = open(dummyfile,O_APPEND|O_RDWR)) < 1) {
      return WHYF("could not open dummy interface file %s for append", dummyfile);
    }

    /* Seek to end of file as initial reading point */
    interface->recv_offset = lseek(interface->alarm.poll.fd,0,SEEK_END);
    /* XXX later add pretend location information so that we can decide which "packets" to receive
       based on closeness */    
    
    // schedule an alarm for this interface
    interface->alarm.function=overlay_dummy_poll;
    interface->alarm.alarm=gettime_ms()+10;
    interface->alarm.deadline=interface->alarm.alarm;
    dummy_poll_stats.name="overlay_dummy_poll";
    interface->alarm.stats=&dummy_poll_stats;
    schedule(&interface->alarm);
    
    interface->state=INTERFACE_STATE_UP;
    INFOF("Dummy interface %s is up",interface->name);
    
    // mark our sid to be sent in full
    if (my_subscriber)
      my_subscriber->send_full = 1;
    
    directory_registration();
    
  } else {
    
    interface->netmask = netmask;
    
    interface->address.sin_addr = src_addr;
    interface->address.sin_family = AF_INET;
    interface->address.sin_port = htons(interface->port);
    
    interface->broadcast_address.sin_addr = broadcast;
    interface->broadcast_address.sin_family = AF_INET;
    interface->broadcast_address.sin_port = htons(interface->port);
    
    if (overlay_interface_init_socket(overlay_interface_count))
      return WHY("overlay_interface_init_socket() failed");    
  }

  overlay_interface_count++;
  return 0;
}

static void overlay_interface_poll(struct sched_ent *alarm)
{
  struct overlay_interface *interface = (overlay_interface *)alarm;

  if (alarm->poll.revents==0){
    
    if (interface->state==INTERFACE_STATE_UP && interface->tick_ms>0){
      // tick the interface
      time_ms_t now = gettime_ms();
      int i = (interface - overlay_interfaces);
      overlay_tick_interface(i, now);
      alarm->alarm=now+interface->tick_ms;
      alarm->deadline=alarm->alarm+interface->tick_ms/2;
      schedule(alarm);
    }
    
    return;
  }
  
  if (alarm->poll.revents & POLLIN) {
    int plen=0;
    unsigned char packet[16384];

    struct sockaddr src_addr;
    socklen_t addrlen = sizeof(src_addr);

    
    /* Read only one UDP packet per call to share resources more fairly, and also
       enable stats to accurately count packets received */
    int recvttl=1;
    plen = recvwithttl(alarm->poll.fd,packet, sizeof(packet), &recvttl, &src_addr, &addrlen);
    if (plen == -1) {
      WHY_perror("recvwithttl(c)");
      overlay_interface_close(interface);
      return;
    }
    
    /* We have a frame from this interface */
    if (debug&DEBUG_PACKETRX)
      DEBUG_packet_visualise("Read from real interface", packet,plen);
    if (debug&DEBUG_OVERLAYINTERFACES) 
      DEBUGF("Received %d bytes from %s on interface %s",plen, 
	     inet_ntoa(((struct sockaddr_in *)&src_addr)->sin_addr),
	     interface->name);
    if (packetOk(interface,packet,plen,NULL,recvttl,&src_addr,addrlen,1)) {
      WHY("Malformed packet");
      // Do we really want to attempt to parse it again?
      //DEBUG_packet_visualise("Malformed packet", packet,plen);
    }
  }
  
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    overlay_interface_close(interface);
  }  
}

void overlay_dummy_poll(struct sched_ent *alarm)
{
  overlay_interface *interface = (overlay_interface *)alarm;
  /* Grab packets, unpackage and dispatch frames to consumers */
  /* XXX Okay, so how are we managing out-of-process consumers?
     They need some way to register their interest in listening to a port.
  */
  unsigned char packet[2048];
  int plen=0;
  struct sockaddr src_addr;
  size_t addrlen = sizeof(src_addr);
  unsigned char transaction_id[8];
  time_ms_t now = gettime_ms();

  /* Read from dummy interface file */
  long long length=lseek(alarm->poll.fd,0,SEEK_END);
  
  int new_packets = (length - interface->recv_offset) / sizeof packet;
  if (new_packets > 20)
    WARNF("Getting behind, there are %d unread packets", new_packets);
  
  if (interface->recv_offset >= length) {
    /* if there's no input, while we want to check for more soon,
	we need to allow all other low priority alarms to fire first,
	otherwise we'll dominate the scheduler without accomplishing anything */
    alarm->alarm = gettime_ms() + 5;
    if (interface->last_tick_ms != -1 && alarm->alarm > interface->last_tick_ms + interface->tick_ms)
      alarm->alarm = interface->last_tick_ms + interface->tick_ms;
    alarm->deadline = alarm->alarm + 10000;
  } else {
    if (lseek(alarm->poll.fd,interface->recv_offset,SEEK_SET) == -1)
      WHY_perror("lseek");
    else {
      if (debug&DEBUG_OVERLAYINTERFACES)
	DEBUGF("Read interface %s (size=%lld) at offset=%d",interface->name, length, interface->recv_offset);
      ssize_t nread = read(alarm->poll.fd, packet, sizeof packet);
      if (nread == -1)
	WHY_perror("read");
      else {
	if (nread == sizeof packet) {
	  interface->recv_offset += nread;
	  plen = packet[110] + (packet[111] << 8);
	  if (plen > nread - 128)
	    plen = -1;
	  if (debug&DEBUG_PACKETRX)
	    DEBUG_packet_visualise("Read from dummy interface", &packet[128], plen);
	  bzero(&transaction_id[0],8);
	  bzero(&src_addr,sizeof(src_addr));
	  if (plen >= 4) {
	    if (packet[0] == 0x01 && packet[1] == 0 && packet[2] == 0 && packet[3] == 0) {
	      if (packetOk(interface,&packet[128],plen,transaction_id, -1 /* fake TTL */, &src_addr,addrlen,1) == -1)
		WARN("Unsupported packet from dummy interface");
	    } else {
	      WARNF("Unsupported packet version from dummy interface: %02x %02x %02x %02x", packet[0], packet[1], packet[2], packet[3]);
	    }
	  } else {
	    WARNF("Invalid packet from dummy interface: plen=%lld", (long long) plen);
	  }
	}
	else
	  WARNF("Read %lld bytes from dummy interface", nread);
      }
    }
    /* keep reading new packets as fast as possible, 
	but don't completely prevent other high priority alarms */
    if (interface->recv_offset >= length)
      alarm->alarm = gettime_ms() + 5;
    else
      alarm->alarm = gettime_ms();
    alarm->deadline = alarm->alarm + 100;
  }
  
  // only tick the interface if we've caught up reading all the packets
  if (interface->recv_offset >= length &&
      interface->tick_ms>0 && 
      (interface->last_tick_ms == -1 || now >= interface->last_tick_ms + interface->tick_ms)) {
    // tick the interface
    int i = (interface - overlay_interfaces);
    overlay_tick_interface(i, now);
  }
  
  schedule(alarm);

  return ;
}

int
overlay_broadcast_ensemble(int interface_number,
			   struct sockaddr_in *recipientaddr,
			   unsigned char *bytes,int len)
{
  if (debug&DEBUG_PACKETTX)
    {
      DEBUGF("Sending this packet via interface #%d",interface_number);
      DEBUG_packet_visualise(NULL,bytes,len);
    }

  overlay_interface *interface = &overlay_interfaces[interface_number];

  if (interface->state!=INTERFACE_STATE_UP){
    return WHYF("Cannot send to interface %s as it is down", interface->name);
  }

  if (interface->fileP)
    {
      char buf[2048];
      bzero(&buf[0],128);
      /* Version information */
      buf[0]=1; buf[1]=0; 
      buf[2]=0; buf[3]=0;
      /* PID of creator */
      buf[4]=getpid()&0xff; buf[5]=getpid()>>8;

      /* TODO make a structure for all this stuff */
      /* bytes 4-5  = half-power beam height (uint16) */
      /* bytes 6-7  = half-power beam width (uint16) */
      /* bytes 8-11 = range in metres, centre beam (uint32) */
      /* bytes 16-47 = sender */
      /* bytes 48-79 = next hop */
      /* bytes 80-83 = latitude (uint32) */
      /* bytes 84-87 = longitude (uint32) */
      /* bytes 88-89 = X/Z direction (uint16) */
      /* bytes 90-91 = Y direction (uint16) */
      /* bytes 92-93 = speed in metres per second (uint16) */
      /* bytes 94-97 = TX frequency in Hz, uncorrected for doppler (which must be done at the receiving end to take into account
         relative motion) */
      /* bytes 98-109 = coding method (use for doppler response etc) null terminated string */
      /* bytes 110-111 = length of packet body in bytes */
      /* bytes 112-127 reserved for future use */

      if (len>2048-128) {
	WARN("Truncating long packet to fit within 1920 byte limit for dummy interface");
	len=2048-128;
      }

      /* Record length of packet */
      buf[110]=len&0xff;
      buf[111]=(len>>8)&0xff;

      bzero(&buf[128+len],2048-(128+len));
      bcopy(bytes,&buf[128],len);
      /* This lseek() is unneccessary because the dummy file is opened in O_APPEND mode.  It's
	 only purpose is to find out the offset to print in the DEBUG statement.  It is vulnerable
	 to a race condition with other processes appending to the same file. */
      off_t fsize = lseek(interface->alarm.poll.fd, (off_t) 0, SEEK_END);
      if (fsize == -1)
	return WHY_perror("lseek");
      if (debug&DEBUG_OVERLAYINTERFACES)
	DEBUGF("Write to interface %s at offset=%d", interface->name, fsize);
      ssize_t nwrite = write(interface->alarm.poll.fd, buf, 2048);
      if (nwrite == -1)
	return WHY_perror("write");
      if (nwrite != 2048)
	return WHYF("only wrote %lld of %lld bytes", nwrite, 2048);
      return 0;
    }
  else
    {
      if (debug&DEBUG_OVERLAYINTERFACES) 
	DEBUGF("Sending %d byte overlay frame on %s to %s",len,interface->name,inet_ntoa(recipientaddr->sin_addr));
      if(sendto(interface->alarm.poll.fd, 
		bytes, len, 0, (struct sockaddr *)recipientaddr, sizeof(struct sockaddr_in)) != len){
	WHY_perror("sendto(c)");
	overlay_interface_close(interface);
	return -1;
      }
      return 0;
    }
}

/* This function is called to return old non-overlay requests back out the
   interface they came in. */
int overlay_sendto(struct sockaddr_in *recipientaddr,unsigned char *bytes,int len)
{
  if (debug&DEBUG_PACKETTX) DEBUGF("Sending %d bytes",len);
  if (overlay_broadcast_ensemble(overlay_last_interface_number,recipientaddr,bytes,len) == -1) 
    return -1;
  return len;
}

/* Register the interface, or update the existing interface registration */
int
overlay_interface_register(char *name,
			   struct in_addr addr,
			   struct in_addr mask) {
  struct interface_rules	*r, *me;
  int				i;
  struct in_addr broadcast = {.s_addr = addr.s_addr | ~mask.s_addr};
  
  if (debug & DEBUG_OVERLAYINTERFACES) {
    // note, inet_ntop doesn't seem to behave on android
    DEBUGF("%s address: %s", name, inet_ntoa(addr));
    DEBUGF("%s broadcast address: %s", name, inet_ntoa(broadcast));
  }
  
  /* See if the interface is listed in the filter */
  me = NULL;
  r = interface_filter;
  while(r) {
    if (!strcasecmp(name, r->namespec))
      me = r;

    r = r->next;
  }

  if (me == NULL || me->excludeP) {
    if (debug & DEBUG_OVERLAYINTERFACES)
      DEBUGF("Interface %s is not interesting.",name);
    return 0;
  }

  int found_interface= -1;
  
  /* Search in the exist list of interfaces */
  for(i = 0; i < overlay_interface_count; i++){
    int broadcast_match = 0;
    int name_match =0;
    
    if ((overlay_interfaces[i].broadcast_address.sin_addr.s_addr & 0xffffffff)
	== (broadcast.s_addr & 0xffffffff)){
      broadcast_match = 1;
    }
    
    name_match = !strcasecmp(overlay_interfaces[i].name, name);
    
    // if we find an exact match we can stop searching
    if (name_match && broadcast_match){
      // mark this interface as still alive
      if (overlay_interfaces[i].state==INTERFACE_STATE_DETECTING)
	overlay_interfaces[i].state=INTERFACE_STATE_UP;
      
      // try to bring the interface back up again even if the address has changed
      if (overlay_interfaces[i].state==INTERFACE_STATE_DOWN){
	overlay_interfaces[i].address.sin_addr = addr;
	overlay_interface_init_socket(i);
      }
      
      // we already know about this interface, and it's up so stop looking immediately
      return 0;
    }
    
    // remember this slot to bring the interface back up again, even if the address has changed
    if (name_match && overlay_interfaces[i].state==INTERFACE_STATE_DOWN)
      found_interface=i;
  }
  
  if (found_interface>=0){
    // try to reactivate the existing interface
    overlay_interfaces[found_interface].address.sin_addr = addr;
    overlay_interfaces[found_interface].broadcast_address.sin_addr = broadcast;
    overlay_interfaces[found_interface].netmask = mask;
    return overlay_interface_init_socket(found_interface);
  }
  
  /* New interface, so register it */
  if (overlay_interface_init(name, addr, mask, broadcast, me->speed_in_bits, me->port, me->type))
    return WHYF("Could not initialise newly seen interface %s", name);
  else
    if (debug & DEBUG_OVERLAYINTERFACES) DEBUGF("Registered interface %s", name);

  return 0;
}
  
void overlay_interface_discover(struct sched_ent *alarm){
  int				i;
  struct interface_rules	*r;
  struct in_addr		dummyaddr;
  int detect_real_interfaces = 0;
  
  /* Mark all UP interfaces as DETECTING, so we can tell which interfaces are new, and which are dead */
  for (i = 0; i < overlay_interface_count; i++)
    if (overlay_interfaces[i].state==INTERFACE_STATE_UP)
      overlay_interfaces[i].state=INTERFACE_STATE_DETECTING;

  /* Check through for any virtual dummy interfaces */
  for (r = interface_filter; r != NULL; r = r->next) {
    if (r->namespec[0] != '>'){
      detect_real_interfaces = 1;
      continue;
    }
    
    for (i = 0; i < overlay_interface_count; i++)
      if (!strcasecmp(overlay_interfaces[i].name,r->namespec)){
	if (overlay_interfaces[i].state==INTERFACE_STATE_DETECTING)
	  overlay_interfaces[i].state=INTERFACE_STATE_UP;
	break;
      }

    if (i >= overlay_interface_count){
      /* New interface, so register it */      
      overlay_interface_init(r->namespec,dummyaddr,dummyaddr,dummyaddr,1000000,PORT_DNA,OVERLAY_INTERFACE_WIFI);
    }
  }

  /* Look for real interfaces */
  if (detect_real_interfaces){
    int no_route = 1;
    
#ifdef HAVE_IFADDRS_H
    if (no_route != 0)
      no_route = doifaddrs();
#endif

#ifdef SIOCGIFCONF
    if (no_route != 0)
      no_route = lsif();
#endif

#ifdef linux
    if (no_route != 0)
      no_route = scrapeProcNetRoute();
#endif

    if (no_route != 0) {
      FATAL("Unable to get any interface information");
    }
  }
  
  // detect if any interfaces have gone away and need to be closed
  for(i = 0; i < overlay_interface_count; i++)
    if (overlay_interfaces[i].state==INTERFACE_STATE_DETECTING)
      overlay_interface_close(&overlay_interfaces[i]);
  
  alarm->alarm = gettime_ms()+5000;
  alarm->deadline = alarm->alarm + 10000;
  schedule(alarm);
  return;
}

/* remove and free a payload from the queue */
static struct overlay_frame *
overlay_queue_remove(overlay_txqueue *queue, struct overlay_frame *frame){
  struct overlay_frame *prev = frame->prev;
  struct overlay_frame *next = frame->next;
  if (prev)
    prev->next = next;
  else if(frame == queue->first)
    queue->first = next;
    
  if (next)
    next->prev = prev;
  else if(frame == queue->last)
    queue->last = prev;
  
  queue->length--;
  
  op_free(frame);
  
  return next;
}

#if 0 /* unused */
static int
overlay_queue_dump(overlay_txqueue *q)
{
  strbuf b = strbuf_alloca(8192);
  struct overlay_frame *f;
  strbuf_sprintf(b,"overlay_txqueue @ 0x%p\n",q);
  strbuf_sprintf(b,"  length=%d\n",q->length);
  strbuf_sprintf(b,"  maxLenght=%d\n",q->maxLength);
  strbuf_sprintf(b,"  latencyTarget=%d milli-seconds\n",q->latencyTarget);
  strbuf_sprintf(b,"  first=%p\n",q->first);
  f=q->first;
  while(f) {
    strbuf_sprintf(b,"    %p: ->next=%p, ->prev=%p\n",
	    f,f->next,f->prev);
    if (f==f->next) {
      strbuf_sprintf(b,"        LOOP!\n"); break;
    }
    f=f->next;
  }
  strbuf_sprintf(b,"  last=%p\n",q->last);
  f=q->last;
  while(f) {
    strbuf_sprintf(b,"    %p: ->next=%p, ->prev=%p\n",
	    f,f->next,f->prev);
    if (f==f->prev) {
      strbuf_sprintf(b,"        LOOP!\n"); break;
    }
    f=f->prev;
  }
  DEBUG(strbuf_str(b));
  return 0;
}
#endif // 0

void
overlay_init_packet(struct outgoing_packet *packet, overlay_interface *interface, int tick){
  packet->interface = interface;
  packet->i = (interface - overlay_interfaces);
  packet->dest=interface->broadcast_address;
  packet->buffer=ob_new();
  ob_limitsize(packet->buffer, packet->interface->mtu);
  ob_append_bytes(packet->buffer,magic_header,4);
  
  overlay_address_clear();
  
  if (tick){
    /* 1. Send announcement about ourselves, including one SID that we host if we host more than one SID
     (the first SID we host becomes our own identity, saving a little bit of data here).
     */
    overlay_add_selfannouncement(packet->i, packet->buffer);
  }else{
    // add a badly formatted dummy self announce payload to tell people we sent this.
    ob_append_byte(packet->buffer, OF_TYPE_SELFANNOUNCE);
    ob_append_byte(packet->buffer, 1);
    ob_append_rfs(packet->buffer, SID_SIZE + 2);
    
    /* from me, to me, via me 
     (it's shorter than an actual broadcast, 
     and receivers wont try to process it 
     since its not going to have a payload body anyway) */
    overlay_address_append_self(interface, packet->buffer);
    overlay_address_set_sender(my_subscriber);
    ob_append_byte(packet->buffer, OA_CODE_PREVIOUS);
    ob_append_byte(packet->buffer, OA_CODE_PREVIOUS);
    
    ob_patch_rfs(packet->buffer, COMPUTE_RFS_LENGTH);
  }
}

// update the alarm time and return 1 if changed
static int
overlay_calc_queue_time(overlay_txqueue *queue, struct overlay_frame *frame){
  int ret=0;
  time_ms_t send_time;

  // ignore packet if the destination is currently unreachable
  if (frame->destination && subscriber_is_reachable(frame->destination)==REACHABLE_NONE)
    return 0;
  
  // when is the next packet from this queue due?
  send_time=queue->first->enqueued_at + queue->transmit_delay;
  if (next_packet.alarm==0 || send_time < next_packet.alarm){
    next_packet.alarm=send_time;
    ret = 1;
  }
  
  // how long can we wait if the server is busy?
  send_time += queue->grace_period;
  if (next_packet.deadline==0 || send_time < next_packet.deadline){
    next_packet.deadline=send_time;
    ret = 1;
  }
  if (!next_packet.function){
    next_packet.function=overlay_send_packet;
    send_packet.name="overlay_send_packet";
    next_packet.stats=&send_packet;
  }
  return ret;
}

static void
overlay_stuff_packet(struct outgoing_packet *packet, overlay_txqueue *queue, time_ms_t now){
  struct overlay_frame *frame = queue->first;
  
  // TODO stop when the packet is nearly full?
  
  while(frame){
    if (frame->enqueued_at + queue->latencyTarget < now){
      DEBUGF("Dropping frame type %x for %s due to expiry timeout", 
	     frame->type, frame->destination?alloca_tohex_sid(frame->destination->sid):"All");
      frame = overlay_queue_remove(queue, frame);
      continue;
    }
    /* Note, once we queue a broadcast packet we are committed to sending it out every interface, 
     even if we hear it from somewhere else in the mean time
     */
    
    struct subscriber *next_hop = frame->destination;
    
    if (next_hop){
      switch(subscriber_is_reachable(next_hop)){
	case REACHABLE_NONE:
	  goto skip;
	  
	case REACHABLE_INDIRECT:
	  next_hop=next_hop->next_hop;
	  frame->sendBroadcast=0;
	  break;
	  
	case REACHABLE_DEFAULT_ROUTE:
	  next_hop=directory_service;
	  frame->sendBroadcast=0;
	  break;
	  
	case REACHABLE_DIRECT:
	case REACHABLE_UNICAST:
	  frame->sendBroadcast=0;
	  break;
	  
	case REACHABLE_BROADCAST:
	  if (!frame->sendBroadcast){
	    if (frame->ttl>2)
	      frame->ttl=2;
	    frame->sendBroadcast=1;
	    if (is_all_matching(frame->broadcast_id.id, BROADCAST_LEN, 0)){
	      overlay_broadcast_generate_address(&frame->broadcast_id);
	      // mark it as already seen so we don't immediately retransmit it
	      overlay_broadcast_drop_check(&frame->broadcast_id);
	    }
	    int i;
	    for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
	      frame->broadcast_sent_via[i]=0;
	  }
	  break;
      }
    }
    
    if (!packet->buffer){
      // use the interface of the first payload we find
      if (frame->sendBroadcast){
	// find an interface that we haven't broadcast on yet
	int i;
	for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
	{
	  if (overlay_interfaces[i].state==INTERFACE_STATE_UP
	      && !frame->broadcast_sent_via[i]){
	    overlay_init_packet(packet, &overlay_interfaces[i], 0);
	    break;
	  }
	}
	
	if (!packet->buffer){
	  // oh dear, why is this broadcast still in the queue?
	  frame = overlay_queue_remove(queue, frame);
	  continue;
	}
      }else{
	overlay_init_packet(packet, next_hop->interface, 0);
	if (next_hop->reachable==REACHABLE_UNICAST){
	  packet->dest = next_hop->address;
	  packet->unicast=1;
	}
      }
      
    }else{
      // make sure this payload can be sent via this interface
      if (frame->sendBroadcast){
	if (frame->broadcast_sent_via[packet->i]){
	  goto skip;
	}
      }else{
	if(packet->interface != next_hop->interface)
	  goto skip;
	if (next_hop->reachable==REACHABLE_DIRECT && packet->unicast)
	  goto skip;
	if (next_hop->reachable==REACHABLE_UNICAST && 
	    ((!packet->unicast) ||
	    packet->dest.sin_addr.s_addr != next_hop->address.sin_addr.s_addr))
	  goto skip;
      }
    }
    
    if (debug&DEBUG_OVERLAYFRAMES){
      DEBUGF("Sending payload type %x len %d for %s via %s", frame->type, frame->payload->position,
	     frame->destination?alloca_tohex_sid(frame->destination->sid):"All",
	     frame->sendBroadcast?alloca_tohex(frame->broadcast_id.id, BROADCAST_LEN):alloca_tohex_sid(next_hop->sid));
    }
    
    if (overlay_frame_append_payload(packet->interface, frame, next_hop, packet->buffer))
      // payload was not queued
      goto skip;
    
    // mark the payload as sent
    int keep_payload = 0;
    
    if (frame->sendBroadcast){
      int i;
      frame->broadcast_sent_via[packet->i]=1;
      
      // check if there is still a broadcast to be sent      
      for(i=0;i<OVERLAY_MAX_INTERFACES;i++)
      {
	if (overlay_interfaces[i].state==INTERFACE_STATE_UP)
	  if (!frame->broadcast_sent_via[i]){
	    keep_payload=1;
	    break;
	  }
      }
    }else{
      frame->send_copies --;
      if (frame->send_copies>0)
	keep_payload=1;
    }
    
    if (!keep_payload){
      frame = overlay_queue_remove(queue, frame);
      continue;
    }
    
  skip:
    // if we can't send the payload now, check when we should try
    overlay_calc_queue_time(queue, frame);
    frame = frame->next;
  }
}

// fill a packet from our outgoing queues and send it
static int
overlay_fill_send_packet(struct outgoing_packet *packet, time_ms_t now) {
  int i;
  IN();
  // while we're looking at queues, work out when to schedule another packet
  unschedule(&next_packet);
  next_packet.alarm=0;
  next_packet.deadline=0;
  
  for (i=0;i<OQ_MAX;i++){
    overlay_txqueue *queue=&overlay_tx[i];
    
    overlay_stuff_packet(packet, queue, now);
  }
  
  if (next_packet.alarm)
    schedule(&next_packet);
  
  if(packet->buffer){
    // send the packet
    if (packet->buffer->position>=HEADERFIELDS_LEN){
      // stuff rhizome announcements at the last moment
      if (rhizome_enabled() && rhizome_http_server_running()){
	overlay_rhizome_add_advertisements(packet->i,packet->buffer);
      }
      
      if (debug&DEBUG_PACKETCONSTRUCTION)
	dump("assembled packet",&packet->buffer->bytes[0],packet->buffer->position);
      
      overlay_broadcast_ensemble(packet->i, &packet->dest, packet->buffer->bytes, packet->buffer->position);
    }
    ob_free(packet->buffer);
    overlay_address_clear();
    RETURN(1);
  }
  RETURN(0);
}

// when the queue timer elapses, send a packet
void overlay_send_packet(struct sched_ent *alarm){
  struct outgoing_packet packet;
  bzero(&packet, sizeof(struct outgoing_packet));
  
  overlay_fill_send_packet(&packet, gettime_ms());
}

// update time for next alarm and reschedule
void overlay_update_queue_schedule(overlay_txqueue *queue, struct overlay_frame *frame){
  if (overlay_calc_queue_time(queue, frame)){
    unschedule(&next_packet);
    schedule(&next_packet);
  }
}

static int
overlay_tick_interface(int i, time_ms_t now) {
  struct outgoing_packet packet;
  IN();
  
  /* An interface with no speed budget is for listening only, so doesn't get ticked */
  if (overlay_interfaces[i].bits_per_second<1
      || overlay_interfaces[i].state!=INTERFACE_STATE_UP) {
    RETURN(0);
  }

  if (debug&DEBUG_OVERLAYINTERFACES) DEBUGF("Ticking interface #%d",i);
  
  // initialise the packet buffer
  bzero(&packet, sizeof(struct outgoing_packet));
  overlay_init_packet(&packet, &overlay_interfaces[i], 1);
  
  /* Add advertisements for ROUTES */
  overlay_route_add_advertisements(packet.interface, packet.buffer);
  
  /* Stuff more payloads from queues and send it */
  overlay_fill_send_packet(&packet, now);
  RETURN(0);
}

static long long
parse_quantity(char *q) {
  int m;
  char units[80];

  if (strlen(q)>=80) return WHY("quantity string >=80 characters");

  if (sscanf(q,"%d%s",&m,units)==2)
    {
      if (units[1]) return WHY("Units should be single character");
      switch(units[0])
	{
	case 'k': return m*1000LL;
	case 'K': return m*1024LL;
	case 'm': return m*1000LL*1000LL;
	case 'M': return m*1024LL*1024LL;
	case 'g': return m*1000LL*1000LL*1000LL;
	case 'G': return m*1024LL*1024LL*1024LL;
	default:
	  return WHY("Illegal unit: should be k,K,m,M,g, or G.");
	}
    }
  if (sscanf(q,"%d",&m)==1)
    {
      return m;
    }
  else
    {
      return WHY("Could not parse quantity");
    }
}

static void
logServalPacket(int level, struct __sourceloc where, const char *message, const unsigned char *packet, size_t len) {
  struct mallocbuf mb = STRUCT_MALLOCBUF_NULL;
  if (serval_packetvisualise(XPRINTF_MALLOCBUF(&mb), message, packet, len) == -1)
    WHY("serval_packetvisualise() failed");
  else if (mb.buffer == NULL)
    WHYF("serval_packetvisualise() output buffer missing, message=%s packet=%p len=%lu", alloca_toprint(-1, message, strlen(message)), packet, len);
  else
    logString(level, where, mb.buffer);
  if (mb.buffer)
    free(mb.buffer);
}
