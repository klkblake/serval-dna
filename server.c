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

#include <sys/socket.h>
#include <netinet/in.h>

#include "serval.h"

unsigned char *hlr=NULL;
int hlr_size=0;

FILE *i_f=NULL;

struct in_addr client_addr;
int client_port;

int getBackingStore(char *s,int size);
int createServerSocket();
int simpleServerMode();

int recvwithttl(int sock,unsigned char *buffer,int bufferlen,int *ttl,
		struct sockaddr *recvaddr,unsigned int *recvaddrlen)
{
  struct msghdr msg;
  struct iovec iov[1];
  
  iov[0].iov_base=buffer;
  iov[0].iov_len=bufferlen;
  bzero(&msg,sizeof(msg));
  msg.msg_name = recvaddr;
  msg.msg_namelen = *recvaddrlen;
  msg.msg_iov = &iov[0];
  msg.msg_iovlen = 1;
  // setting the following makes the data end up in the wrong place
  //  msg.msg_iov->iov_base=iov_buffer;
  // msg.msg_iov->iov_len=sizeof(iov_buffer);

  struct cmsghdr cmsgcmsg[16];
  msg.msg_control = &cmsgcmsg[0];
  msg.msg_controllen = sizeof(struct cmsghdr)*16;
  msg.msg_flags = 0;

  fcntl(sock,F_SETFL, O_NONBLOCK);

  int len = recvmsg(sock,&msg,0);

  if (debug&DEBUG_PACKETXFER)
    fprintf(stderr,"recvmsg returned %d bytes (flags=%d,msg_controllen=%d)\n",
	    len,msg.msg_flags,msg.msg_controllen);
  
  struct cmsghdr *cmsg;
  if (len>0)
    {
      for (cmsg = CMSG_FIRSTHDR(&msg); 
	   cmsg != NULL; 
	   cmsg = CMSG_NXTHDR(&msg,cmsg)) {
	
	if ((cmsg->cmsg_level == IPPROTO_IP) && 
	    ((cmsg->cmsg_type == IP_RECVTTL) ||(cmsg->cmsg_type == IP_TTL))
	    &&(cmsg->cmsg_len) ){
	  if (debug&DEBUG_PACKETXFER)
	    fprintf(stderr,"  TTL (%p) data location resolves to %p\n",
		    ttl,CMSG_DATA(cmsg));
	  if (CMSG_DATA(cmsg)) {
	    *ttl = *(unsigned char *) CMSG_DATA(cmsg);
	    if (debug&DEBUG_PACKETXFER)
	      fprintf(stderr,"  TTL of packet is %d\n",*ttl);
	  } 
	} else {
	  if (debug&DEBUG_PACKETXFER)
	    fprintf(stderr,"I didn't expect to see level=%02x, type=%02x\n",
		    cmsg->cmsg_level,cmsg->cmsg_type);
	}	 
      }
  }
  *recvaddrlen=msg.msg_namelen;

  return len;
}


int server(char *backing_file,int size,int foregroundMode)
{
  
  /* Get backing store for HLR */
  getBackingStore(backing_file,size);

  if (overlayMode)
    {
      /* Now find and initialise all the suitable network interfaces, i.e., 
	 those running IPv4.
	 Packet radio dongles will get discovered later as the interfaces get probed.

	 This will setup the sockets for the server to communicate on each interface.
	 
	 XXX - Problems may persist where the same address is used on multiple interfaces,
	 but otherwise hopefully it will allow us to bridge multiple networks.
      */
      overlay_interface_discover();
    }
  else
    {
      /* Create a simple socket for listening on if we are not in overlay mesh mode. */
      createServerSocket();     
    }

  /* Detach from the console */
  if (!foregroundMode) daemon(0,0);

  if (!overlayMode) simpleServerMode();
  else overlayServerMode();

  return 0;
}

int getBackingStore(char *backing_file,int size)
{
 if (!backing_file)
    {
      /* transitory storage of HLR data, so just malloc() the memory */
      hlr=calloc(size,1);
      if (!hlr) exit(setReason("Failed to calloc() HLR database."));
      if (debug&DEBUG_HLR) fprintf(stderr,"Allocated %d byte temporary HLR store\n",size);
    }
  else
    {
      unsigned char zero[8192];
      FILE *f=fopen(backing_file,"r+");
      if (!f) f=fopen(backing_file,"w+");
      if (!f) exit(setReason("Could not open backing file."));
      bzero(&zero[0],8192);
      fseek(f,0,SEEK_END);
      errno=0;
      while(ftell(f)<size)
	{
	  int r;
	  fseek(f,0,SEEK_END);
	  if ((r=fwrite(zero,8192,1,f))!=1)
	    {
	      perror("fwrite");
	      exit(setReason("Could not enlarge backing file to requested size (short write)"));
	    }
	  fseek(f,0,SEEK_END);
	}
      
      if (errno) perror("fseek");
      if (fwrite("",1,1,f)!=1)
	{
	  fprintf(stderr,"Failed to set backing file size.\n");
	  perror("fwrite");
	}
      hlr=(unsigned char *)mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,fileno(f),0);
      if (hlr==MAP_FAILED) {
	perror("mmap");
	exit(setReason("Memory mapping of HLR backing file failed."));
      }
      if (debug&DEBUG_HLR) fprintf(stderr,"Allocated %d byte HLR store backed by file `%s'\n",
			 size,backing_file);
    }
  hlr_size=size;

  seedHlr();

  return 0;
}

int processRequest(unsigned char *packet,int len,
		   struct sockaddr *sender,int sender_len,
		   unsigned char *transaction_id,int recvttl, char *did,char *sid)
{
  /* Find HLR entry by DID or SID, unless creating */
  int ofs,rofs=0;
  int records_searched=0;
  
  int prev_pofs=0;
  int pofs=OFS_PAYLOAD;

  while(pofs<len)
    {
      if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"  processRequest: len=%d, pofs=%d, pofs_prev=%d\n",len,pofs,prev_pofs);
      /* Avoid infinite loops */
      if (pofs<=prev_pofs) break;
      prev_pofs=pofs;

      if (packet[pofs]==ACTION_CREATEHLR)
	{
	  /* Creating an HLR requires an initial DID number and definately no SID -
	     you can't choose a SID. */
	  if (debug&DEBUG_HLR) fprintf(stderr,"Creating a new HLR record. did='%s', sid='%s'\n",did,sid);
	  if (!did[0]) return respondSimple(NULL,ACTION_DECLINED,NULL,0,transaction_id,recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
	  if (sid[0])  return respondSimple(sid,ACTION_DECLINED,NULL,0,transaction_id,recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
	  if (debug&DEBUG_HLR) fprintf(stderr,"Verified that create request supplies DID but not SID\n");
	  
	  {
	    char sid[128];
	    /* make HLR with new random SID and initial DID */
	    if (!createHlr(did,sid))
	      return respondSimple(sid,ACTION_OKAY,NULL,0,transaction_id,
				   recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
	    else
	      return respondSimple(NULL,ACTION_DECLINED,NULL,0,transaction_id,
				   recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
	  }
	  pofs+=1;
	  pofs+=1+SID_SIZE;
	}
      else
	{
	  if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"Looking at action code 0x%02x @ packet offset 0x%x\n",
			       packet[pofs],pofs);
	  switch(packet[pofs])
	    {
	    case ACTION_PAD: /* Skip padding */
	      pofs++;
	      pofs+=1+packet[pofs];
	      break;
	    case ACTION_EOT:  /* EOT */
	      pofs=len;
	      break;
	    case ACTION_STATS:
	      /* short16 variable id,
		 int32 value */
	      {
		pofs++;
		short field=packet[pofs+1]+(packet[pofs]<<8);
		int value=packet[pofs+5]+(packet[pofs+4]<<8)+(packet[pofs+3]<<16)+(packet[pofs+2]<<24);
		pofs+=6;
		if (instrumentation_file)
		  {
		    if (!i_f) { if (strcmp(instrumentation_file,"-")) i_f=fopen(instrumentation_file,"a"); else i_f=stdout; }
		    if (i_f) fprintf(i_f,"%ld:%08x:%d:%d\n",time(0),*(unsigned int *)&sender->sa_data[0],field,value);
		    if (i_f) fflush(i_f);
		  }
	      }
	      break;
	    case ACTION_DIGITALTELEGRAM:
	      if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"In ACTION_DIGITALTELEGRAM\n");
	      {
		// Check transaction cache, to see if message has already been delivered.  If not, then process it.
		int delivered = isTransactionInCache(transaction_id);
		if (!delivered) {
		  // Unpack SMS message.
		  char emitterPhoneNumber[256];
		  char message[256];
		  pofs++;
		  /* char messageType = packet[pofs]; */
		  pofs++;
		  char emitterPhoneNumberLen = packet[pofs];
		  pofs++;
		  char messageLen = packet[pofs];
		  pofs++;
		  strncpy(emitterPhoneNumber, (const char*)packet+pofs, emitterPhoneNumberLen);
		  emitterPhoneNumber[(unsigned int)emitterPhoneNumberLen]=0;
		  
		  pofs+=emitterPhoneNumberLen;
		  strncpy(message, (const char*)packet+pofs, messageLen); 
		  message[(unsigned int)messageLen]=0;
		  
		  pofs+=messageLen;
		
		  // Check if I'm the recipient
		  ofs=0;
		  if (findHlr(hlr, &ofs, sid, did)){
		    // Deliver SMS to android.
		    char amCommand[576]; // 64 char + 2*256(max) char = 576
		    sprintf(amCommand, "am broadcast -a org.servalproject.DT -e number \"%s\"  -e content \"%s\"", emitterPhoneNumber, message);
		    if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"Delivering DT message via intent: %s\n",amCommand);
		    runCommand(amCommand);
		    delivered = 1;
		    // Record in cache to prevent re-delivering the same message if a duplicate is received.
		    insertTransactionInCache(transaction_id);
		  }
		}
		if (delivered) {
		  respondSimple(hlrSid(hlr, ofs), ACTION_OKAY, NULL, 0, transaction_id, recvttl,sender, CRYPT_CIPHERED|CRYPT_SIGNED);
		}
	      }
	      break;
	    case ACTION_SET:
	      ofs=0;
	      if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"Looking for hlr entries with sid='%s' / did='%s'\n",sid,did);

	      if ((!sid)||(!sid[0])) {
		setReason("You can only set variables by SID");
		return respondSimple(NULL,ACTION_ERROR,
				     (unsigned char *)"SET requires authentication by SID",
				     0,transaction_id,recvttl,
				     sender,CRYPT_CIPHERED|CRYPT_SIGNED);
	      }

	      while(findHlr(hlr,&ofs,sid,did))
		{
		  int itemId,instance,start_offset,bytes,flags;
		  unsigned char value[9000],oldvalue[65536];
		  int oldr,oldl;
		  
		  if (debug&DEBUG_HLR) fprintf(stderr,"findHlr found a match for writing at 0x%x\n",ofs);
		  if (debug&DEBUG_HLR) hlrDump(hlr,ofs);
		  
		  /* XXX consider taking action on this HLR
		     (check PIN first depending on the action requested) */
	      
		  /* XXX Doesn't verify PIN authentication */
		  
		  /* Get write request */
		    
		  pofs++; rofs=pofs;
		  if (extractRequest(packet,&pofs,len,
				     &itemId,&instance,value,
				     &start_offset,&bytes,&flags))
		    {
		      setReason("Could not extract ACTION_SET request");
		      return 
			respondSimple(NULL,ACTION_ERROR,
				      (unsigned char *)"Mal-formed SET request",
				      0,transaction_id,recvttl,
				      sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		    }
		  
		  /* Get the stored value */
		  oldl=65536;
		  oldr=hlrGetVariable(hlr,ofs,itemId,instance,oldvalue,&oldl);
		  if (oldr) {
		    if (flags==SET_NOREPLACE) {
		      oldl=0;
		    } else {
		      setReason("Tried to SET_NOCREATE/SET_REPLACE a non-existing value");
		      return 
			  respondSimple(NULL,ACTION_ERROR,
					(unsigned char *)"Cannot SET NOCREATE/REPLACE a value that does not exist",
					0,transaction_id,recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		    }
		  } else {
		    if (flags==SET_NOREPLACE) {
		      setReason("Tried to SET_NOREPLACE an existing value");
		      if (debug&DEBUG_DNAREQUESTS) dump("Existing value (in SET_NOREPLACE flagged request)",oldvalue,oldl);
		      return 
			respondSimple(NULL,ACTION_ERROR,
				      (unsigned char *)"Cannot SET NOREPLACE; a value exists",
				      0,transaction_id,recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		      }
		  }
		  /* Replace the changed portion of the stored value */
		  if ((start_offset+bytes)>oldl) {
		    bzero(&oldvalue[oldl],start_offset+bytes-oldl);
		    oldl=start_offset+bytes;
		  }
		  bcopy(&value[0],&oldvalue[start_offset],bytes);
		    
		  /* Write new value back */
		  if (hlrSetVariable(hlr,ofs,itemId,instance,oldvalue,oldl))
		    {
		      setReason("Failed to write variable");
		      return 
			respondSimple(NULL,ACTION_ERROR,
				      (unsigned char *)"Failed to SET variable",
				      0,transaction_id,recvttl,
				      sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		    }
		  if (debug&DEBUG_HLR) { fprintf(stderr,"HLR after writing:\n"); hlrDump(hlr,ofs); }
		  
		  /* Reply that we wrote the fragment */
		  respondSimple(sid,ACTION_WROTE,&packet[rofs],6,
				transaction_id,recvttl,
				sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		  /* Advance to next record and keep searching */
		  if (nextHlr(hlr,&ofs)) break;
		}
	      break;
	    case ACTION_GET:
	      {
		/* Limit transfer size to MAX_DATA_BYTES, plus an allowance for variable packing. */
		unsigned char data[MAX_DATA_BYTES+16];
		int dlen=0;
		int sendDone=0;

		if (debug&DEBUG_HLR) dump("Request bytes",&packet[pofs],8);

		pofs++;
		int var_id=packet[pofs];
		int instance=-1;
		if (var_id&0x80) instance=packet[++pofs];
		pofs++;
		int offset=(packet[pofs]<<8)+packet[pofs+1]; pofs+=2;
		char *hlr_sid=NULL;

		pofs+=2;

		if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"Processing ACTION_GET (var_id=%02x, instance=%02x, pofs=0x%x, len=%d)\n",var_id,instance,pofs,len);

		ofs=0;
		if (debug&DEBUG_HLR) fprintf(stderr,"Looking for hlr entries with sid='%s' / did='%s'\n",sid?sid:"null",did?did:"null");

		while(1)
		  {
		    struct hlrentry_handle *h;

		    // if an empty did was passed in, get results from all hlr records
		    if (*sid || *did){
 		      if (!findHlr(hlr,&ofs,sid,did)) break;
		      if (debug&DEBUG_HLR) fprintf(stderr,"findHlr found a match @ 0x%x\n",ofs);
		    }
		    if (debug&DEBUG_HLR) hlrDump(hlr,ofs);
  		  
		    /* XXX consider taking action on this HLR
		       (check PIN first depending on the action requested) */

		    /* Form a reply packet containing the requested data */
  		  
		    if (instance==0xff) instance=-1;
  		  
		    /* Step through HLR to find any matching instances of the requested variable */
		    h=openhlrentry(hlr,ofs);
		    if (debug&DEBUG_HLR) fprintf(stderr,"openhlrentry(hlr,%d) returned %p\n",ofs,h);
		    while(h)
		      {
			/* Is this the variable? */
			if (debug&DEBUG_HLR) fprintf(stderr,"  considering var_id=%02x, instance=%02x\n",
					     h->var_id,h->var_instance);
			if (h->var_id==var_id)
			  {
			    if (h->var_instance==instance||instance==-1)
			      {
				if (debug&DEBUG_HLR) fprintf(stderr,"Sending matching variable value instance (instance #%d), value offset %d.\n",
						     h->var_instance,offset);
  			      
				// only send each value when the *next* record is found, that way we can easily stamp the last response with DONE
				if (sendDone>0)
				  respondSimple(hlr_sid,ACTION_DATA,data,dlen,
						transaction_id,recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);

				dlen=0;
	    
				if (packageVariableSegment(data,&dlen,h,offset,MAX_DATA_BYTES+16))
				  return setReason("packageVariableSegment() failed.");
				hlr_sid=hlrSid(hlr,ofs);

				sendDone++;
			      }
			    else
			      if (debug&DEBUG_HLR) fprintf(stderr,"Ignoring variable instance %d (not %d)\n",
						   h->var_instance,instance);
			  }
			else
			  if (debug&DEBUG_HLR) fprintf(stderr,"Ignoring variable ID %d (not %d)\n",
					       h->var_id,var_id);
			h=hlrentrygetent(h);
		      }
  		  
		    /* Advance to next record and keep searching */
		    if (nextHlr(hlr,&ofs)) break;
		  }
		  if (sendDone)
		    {
		      data[dlen++]=ACTION_DONE;
		      data[dlen++]=sendDone&0xff;
		      respondSimple(hlr_sid,ACTION_DATA,data,dlen,transaction_id,
				    recvttl,sender,CRYPT_CIPHERED|CRYPT_SIGNED);
		    }
		  if (gatewayspec&&(var_id==VAR_LOCATIONS)&&did&&strlen(did))
		    {
		      /* We are a gateway, so offer connection via the gateway as well */
		      unsigned char data[MAX_DATA_BYTES+16];
		      int dlen=0;
		      struct hlrentry_handle fake;
		      unsigned char uri[1024];

		      /* We use asterisk to provide the gateway service,
			 so we need to create a temporary extension in extensions.conf,
			 ask asterisk to re-read extensions.conf, and then make sure it has
			 a functional SIP gateway.
		      */
		      if (!asteriskObtainGateway(sid,did,(char *)uri))
			{
			  
			  fake.value_len=strlen((char *)uri);
			  fake.var_id=var_id;
			  fake.value=uri;
			  
			  if (packageVariableSegment(data,&dlen,&fake,offset,MAX_DATA_BYTES+16))
			    return setReason("packageVariableSegment() of gateway URI failed.");
			  
			  respondSimple(hlrSid(hlr,0),ACTION_DATA,data,dlen,
					transaction_id,recvttl,sender,
					CRYPT_CIPHERED|CRYPT_SIGNED);
			}
		      else
			{
			  /* Should we indicate the gateway is not available? */
			}
		    }
	      
	      }
	      break;
	    default:
	      setReason("Asked to perform unsupported action");
	      if (debug&DEBUG_PACKETFORMATS) fprintf(stderr,"Asked to perform unsipported action at Packet offset = 0x%x\n",pofs);
	      if (debug&DEBUG_PACKETFORMATS) dump("Packet",packet,len);
	      return WHY("Asked to perform unsupported action.");
	    }	   
	}
    }
  
  if (debug&DEBUG_HLR) fprintf(stderr,"Searched %d HLR entries.\n",records_searched);

  return 0;
}

int respondSimple(char *sid,int action,unsigned char *action_text,int action_len,
		  unsigned char *transaction_id,int recvttl,
		  struct sockaddr *recvaddr,int cryptoFlags)
{
  unsigned char packet[8000];
  int pl=0;
  int *packet_len=&pl;
  int packet_maxlen=8000;
  int i;

  /* XXX Complain about invalid crypto flags.
     XXX We don't do anything with the crypto flags right now
     XXX Other packet sending routines need this as well. */
  if (!cryptoFlags) return WHY("Crypto-flags not set.");

  /* ACTION_ERROR is associated with an error message.
     For syntactic simplicity, we do not require the respondSimple() call to provide
     the length of the error message. */
  if (action==ACTION_ERROR) {
    action_len=strlen((char *)action_text);
    /* Make sure the error text isn't too long.
       IF it is, trim it, as we still need to communicate the error */
    if (action_len>255) action_len=255;
  }

  /* Prepare the request packet */
  if (packetMakeHeader(packet,8000,packet_len,transaction_id,cryptoFlags)) 
    return WHY("packetMakeHeader() failed.");
  if (sid&&sid[0]) 
    { if (packetSetSid(packet,8000,packet_len,sid)) 
	return setReason("invalid SID in reply"); }
  else 
    { if (packetSetDid(packet,8000,packet_len,"")) 
	return setReason("Could not set empty DID in reply"); }  

  CHECK_PACKET_LEN(1+1+action_len);
  packet[(*packet_len)++]=action;
  if (action==ACTION_ERROR) packet[(*packet_len)++]=action_len;
  for(i=0;i<action_len;i++) packet[(*packet_len)++]=action_text[i];

  if (debug&DEBUG_DNARESPONSES) dump("Simple response octets",action_text,action_len);

  if (packetFinalise(packet,8000,recvttl,packet_len,cryptoFlags))
    return WHY("packetFinalise() failed.");

  if (debug&DEBUG_DNARESPONSES) fprintf(stderr,"Sending response of %d bytes.\n",*packet_len);

  if (packetSendRequest(REQ_REPLY,packet,*packet_len,NONBATCH,transaction_id,recvaddr,NULL)) 
    return WHY("packetSendRequest() failed.");
  
  return 0;
}

int createServerSocket() 
{
  struct sockaddr_in bind_addr;
  
  sock=socket(PF_INET,SOCK_DGRAM,0);
  if (sock<0) {
    fprintf(stderr,"Could not create UDP socket.\n");
    perror("socket");
    exit(-3);
  }
  
  /* Automatically close socket on calls to exec().
     This makes life easier when we restart with an exec after receiving
     a bad signal. */
  fcntl(sock, F_SETFL,
	fcntl(sock, F_GETFL, NULL)|O_CLOEXEC);

  int TRUE=1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &TRUE, sizeof(TRUE));

  errno=0;
  if(setsockopt(sock, IPPROTO_IP, IP_RECVTTL, &TRUE,sizeof(TRUE))<0)
    perror("setsockopt(IP_RECVTTL)");  

  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons( PORT_DNA );
  bind_addr.sin_addr.s_addr = htonl( INADDR_ANY );
  if(bind(sock,(struct sockaddr *)&bind_addr,sizeof(bind_addr))) {
    fprintf(stderr,"MP HLR server could not bind to UDP port %d\n", PORT_DNA);
    perror("bind");
    exit(-3);
  }
  return 0;
}

extern int sigIoFlag;
extern int rhizome_server_socket;
int simpleServerMode()
{
  while(1) {
    struct sockaddr recvaddr;
    socklen_t recvaddrlen=sizeof(recvaddr);
    struct pollfd fds[128];
    int fdcount;
    int len;
    int r;

    bzero((void *)&recvaddr,sizeof(recvaddr));

    /* Get rhizome server started BEFORE populating fd list so that
       the server's listen socket is in the list for poll() */
    if (rhizome_datastore_path) rhizome_server_poll();

    /* Get list of file descripters to watch */
    fds[0].fd=sock; fds[0].events=POLLIN;
    fdcount=1;
    rhizome_server_get_fds(fds,&fdcount,128);
    if (debug&DEBUG_IO) {
      printf("poll()ing file descriptors:");
      { int i;
	for(i=0;i<fdcount;i++) { printf(" %d",fds[i].fd); } }
      printf("\n");
    }
    
    /* Wait patiently for packets to arrive. */
    if (rhizome_datastore_path) rhizome_server_poll();
    while ((r=poll(fds,fdcount,100000))<1) {
      if (sigIoFlag) { sigIoFlag=0; break; }
      sleep(0);
    }
    if (rhizome_datastore_path) rhizome_server_poll();

    unsigned char buffer[16384];
    int ttl=-1; // unknown

    if (fds[0].revents&POLLIN) {
      
      len=recvwithttl(sock,buffer,sizeof(buffer),&ttl,&recvaddr,&recvaddrlen);


      client_port=((struct sockaddr_in*)&recvaddr)->sin_port;
      client_addr=((struct sockaddr_in*)&recvaddr)->sin_addr;
      
      if (debug&DEBUG_DNAREQUESTS) fprintf(stderr,"Received packet from %s:%d (len=%d).\n",inet_ntoa(client_addr),client_port,len);
      if (debug&DEBUG_PACKETXFER) dump("recvaddr",(unsigned char *)&recvaddr,recvaddrlen);
      if (debug&DEBUG_PACKETXFER) dump("packet",(unsigned char *)buffer,len);
      if (dropPacketP(len)) {
	if (debug&DEBUG_SIMULATION) fprintf(stderr,"Simulation mode: Dropped packet due to simulated link parameters.\n");
	continue;
      }
      /* Simple server mode doesn't really use interface numbers, so lie and say interface -1 */
      if (packetOk(-1,buffer,len,NULL,ttl,&recvaddr,recvaddrlen,1)) { 
	if (debug&DEBUG_PACKETFORMATS) setReason("Ignoring invalid packet");
      }
      if (debug&DEBUG_PACKETXFER) fprintf(stderr,"Finished processing packet, waiting for next one.\n");
    }
  }
  return 0;
}

#ifdef DEBUG_MEM_ABUSE
unsigned char groundzero[65536];
int memabuseInitP=0;

int memabuseInit()
{
  if (memabuseInitP) {
    fprintf(stderr,"WARNING: memabuseInit() called more than once.\n");
    return memabuseCheck();
  }

  unsigned char *zero=(unsigned char *)0;
  int i;
  for(i=0;i<65536;i++) {
    groundzero[i]=zero[i];
    printf("%04x\n",i);
  }
  memabuseInitP=1;
  return 0;
}

int _memabuseCheck(const char *func,const char *file,const int line)
{
  unsigned char *zero=(unsigned char *)0;
  int firstAddr=-1;
  int lastAddr=-1;
  int i;
  for(i=0;i<65536;i++) if (groundzero[i]!=zero[i]) {
      lastAddr=i;
      if (firstAddr==-1) firstAddr=i;
    }
  
  if (lastAddr>0) {
    fprintf(stderr,"WARNING: Memory corruption in first 64KB of RAM detected.\n");
    fprintf(stderr,"         Changed bytes exist in range 0x%04x - 0x%04x\n",firstAddr,lastAddr);
    dump("Changed memory content",&zero[firstAddr],lastAddr-firstAddr+1);
    dump("Initial memory content",&groundzero[firstAddr],lastAddr-firstAddr+1);
    sleep(1);
  } else {
    fprintf(stderr,"All's well at %s() %s:%d\n",func,file,line);
  }
  
  return 0;
}
#endif
