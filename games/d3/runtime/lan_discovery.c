#include "lan_discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const uint8_t magic[8]={'M','N','G','L','D','N',0,1};
static void put64(uint8_t *out,uint64_t n){for(int i=7;i>=0;i--){out[i]=(uint8_t)n;n>>=8;}}
static uint64_t get64(const uint8_t *in){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|in[i];return n;}
static int nonzero(const uint8_t *p,size_t n){uint8_t value=0;for(size_t i=0;i<n;i++)value|=p[i];return value!=0;}
static int ssid_valid(const MngLdnSsid *s){
 if(s->length<1||s->length>32||s->value[s->length])return 0;
 for(unsigned i=0;i<s->length;i++)if((unsigned char)s->value[i]<0x20||(unsigned char)s->value[i]>0x7e)return 0;
 return 1;
}
int mng_ldn_network_valid(const MngLdnNetwork *n){
 if(!n||!ssid_valid(&n->common.ssid)||n->max_nodes<1||n->max_nodes>8||
    n->node_count<1||n->node_count>(unsigned)n->max_nodes||n->advertise_size>384||
    n->accept_policy>3||n->security>3)return 0;
 unsigned count=0;
 for(unsigned i=0;i<8;i++){
  const MngLdnNode *node=&n->nodes[i];if(node->connected>1)return 0;
  if(node->connected){
   if(i>=(unsigned)n->max_nodes||node->id!=(int)i||!memchr(node->name,0,sizeof node->name)||node->version<0)return 0;
   count++;
  }
 }
 return count==n->node_count&&n->nodes[0].connected;
}
int mng_ldn_filter_match(const MngLdnNetwork *n,const MngLdnFilter *f){
 if(!mng_ldn_network_valid(n))return 0;if(!f)return 1;
 unsigned flags=f->flags;if(flags&~63u)return 0;
 if((flags&1)&&n->network_id.intent.communication_id!=f->network_id.intent.communication_id)return 0;
 if((flags&2)&&memcmp(n->network_id.session,f->network_id.session,16))return 0;
 if((flags&4)&&n->common.type!=f->network_type)return 0;
 if((flags&8)&&memcmp(n->common.bssid,f->bssid,6))return 0;
 if((flags&16)&&(!ssid_valid(&f->ssid)||n->common.ssid.length!=f->ssid.length||memcmp(n->common.ssid.value,f->ssid.value,f->ssid.length)))return 0;
 if((flags&32)&&n->network_id.intent.scene!=f->network_id.intent.scene)return 0;
 return 1;
}
static void frame(uint8_t *out,uint8_t kind,uint16_t size,uint64_t title,const uint8_t nonce[16]){
 memset(out,0,40);memcpy(out,magic,8);out[8]=kind;out[10]=(uint8_t)(size>>8);out[11]=(uint8_t)size;
 put64(out+12,title);memcpy(out+20,nonce,16);
}
static int valid_frame(const uint8_t *in,size_t size,unsigned kind,uint64_t title){
 return in&&size>=40&&size<=MNG_LAN_PACKET_MAX&&!memcmp(in,magic,8)&&in[8]==kind&&in[9]==0&&
  (((unsigned)in[10]<<8)|in[11])==size&&get64(in+12)==title&&nonzero(in+20,16)&&
  (kind==3 ? (in[36] || in[37]) && in[38]==3 && in[39]==0 : !nonzero(in+36,4));
}
size_t mng_lan_query_encode(uint8_t *out,uint64_t title,const uint8_t nonce[16]){
 if(!out||!nonce||!nonzero(nonce,16)||!title)return 0;frame(out,1,40,title,nonce);return 40;
}
size_t mng_lan_advert_encode(uint8_t *out,uint64_t title,const uint8_t nonce[16],const MngLanRoom *room){
 if(!out||!nonce||!nonzero(nonce,16)||!title||!room||!nonzero(room->room,16)||!mng_ldn_network_valid(&room->network))return 0;
 if(room->control_port ? room->control_version!=3 || memcmp(room->room,room->network.network_id.session,16) : room->control_version!=0)return 0;
 frame(out,room->control_port?3:2,MNG_LAN_PACKET_MAX,title,nonce);
 if(room->control_port){out[36]=(uint8_t)(room->control_port>>8);out[37]=(uint8_t)room->control_port;out[38]=room->control_version;}memcpy(out+40,room->room,16);put64(out+56,room->revision);
 memcpy(out+64,&room->network,sizeof room->network);return MNG_LAN_PACKET_MAX;
}
int mng_lan_query_decode(const void *data,size_t size,uint64_t title,uint8_t nonce[16]){
 if(!nonce||size!=40||!valid_frame(data,size,1,title))return 0;memcpy(nonce,(const uint8_t *)data+20,16);return 1;
}
int mng_lan_advert_decode(const void *data,size_t size,uint64_t title,const uint8_t nonce[16],MngLanRoom *room){
 const uint8_t *in=data;
 if(!room||!nonce||size!=MNG_LAN_PACKET_MAX||(!valid_frame(in,size,2,title)&&!valid_frame(in,size,3,title))||memcmp(in+20,nonce,16)||!nonzero(in+40,16))return 0;
 MngLanRoom tmp={0};memcpy(tmp.room,in+40,16);tmp.revision=get64(in+56);memcpy(&tmp.network,in+64,sizeof tmp.network);
 if(!mng_ldn_network_valid(&tmp.network))return 0;
 if(in[8]==3){
  if(memcmp(tmp.room,tmp.network.network_id.session,16))return 0;
  tmp.control_port=(uint16_t)((unsigned)in[36]<<8|in[37]);tmp.control_version=in[38];
 }
 *room=tmp;return 1;
}
static uint64_t now_ms(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;}
static int socket_open(void){
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -1;
 if(fcntl(fd,F_SETFD,FD_CLOEXEC)||fcntl(fd,F_SETFL,O_NONBLOCK)){int e=errno;close(fd);errno=e;return -1;}return fd;
}
static int fail_close(int fd){int e=errno;close(fd);errno=e;return -1;}
static int multicast_options(int fd,uint32_t interface_be){
 unsigned char ttl=1,loop=1;struct in_addr address={interface_be};
 return setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof ttl)||
  setsockopt(fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof loop)||
  setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&address,sizeof address);
}
int mng_lan_listener_open(uint32_t interface_be,uint16_t port,int multicast){
 int fd=socket_open();if(fd<0)return -1;
 int yes=1;
 if(multicast&&(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes)||setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof yes)))return fail_close(fd);
 struct sockaddr_in address={0};address.sin_len=sizeof address;address.sin_family=AF_INET;address.sin_port=htons(port);
 address.sin_addr.s_addr=multicast?INADDR_ANY:interface_be;
 if(bind(fd,(struct sockaddr *)&address,sizeof address))return fail_close(fd);
 if(multicast){
  struct ip_mreq membership={0};inet_pton(AF_INET,MNG_LAN_DISCOVERY_GROUP,&membership.imr_multiaddr);membership.imr_interface.s_addr=interface_be;
  if(multicast_options(fd,interface_be)||setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&membership,sizeof membership))return fail_close(fd);
 }
 return fd;
}
static int unicast_source(const struct sockaddr_in *source){
 uint32_t ip=ntohl(source->sin_addr.s_addr);
 return source->sin_family==AF_INET&&source->sin_port&&ip&&ip!=0xffffffffu&&(ip>>24)<224;
}
static ssize_t receive(int fd,uint8_t *bytes,struct sockaddr_in *source,int timeout){
 struct pollfd p={fd,POLLIN,0};int result=poll(&p,1,timeout);
 if(result<=0)return result==0?0:-1;
 struct iovec iov={bytes,MNG_LAN_PACKET_MAX};struct msghdr msg={0};memset(source,0,sizeof *source);
 msg.msg_name=source;msg.msg_namelen=sizeof *source;msg.msg_iov=&iov;msg.msg_iovlen=1;
 ssize_t n=recvmsg(fd,&msg,0);
 if(n<0)return -1;
 if((msg.msg_flags&MSG_TRUNC)||msg.msg_namelen!=sizeof *source||!unicast_source(source))return 0;
 return n;
}
int mng_lan_serve_once(int fd,uint64_t title,const MngLanRoom *room,int timeout_ms){
 if(timeout_ms<0||timeout_ms>2000||!room||!mng_ldn_network_valid(&room->network)){errno=EINVAL;return -1;}
 uint8_t bytes[MNG_LAN_PACKET_MAX],nonce[16];struct sockaddr_in source;
 ssize_t n=receive(fd,bytes,&source,timeout_ms);if(n<=0)return (int)n;
 if(!mng_lan_query_decode(bytes,(size_t)n,title,nonce))return 0;
 size_t size=mng_lan_advert_encode(bytes,title,nonce,room);if(!size){errno=EINVAL;return -1;}
 return sendto(fd,bytes,size,0,(struct sockaddr *)&source,sizeof source)==(ssize_t)size?1:-1;
}
int mng_lan_scan(const struct sockaddr_in *destination,uint32_t interface_be,uint64_t title,
                 const MngLdnFilter *filter,MngLanRoom *out,size_t capacity,int timeout_ms){
 if(!destination||destination->sin_family!=AF_INET||!destination->sin_port||!title||!out||!capacity||capacity>MNG_LAN_ROOMS_MAX||timeout_ms<1||timeout_ms>2000){errno=EINVAL;return -1;}
 int fd=socket_open();if(fd<0)return -1;
 struct sockaddr_in local={0};local.sin_len=sizeof local;local.sin_family=AF_INET;local.sin_addr.s_addr=interface_be;
 if(bind(fd,(struct sockaddr *)&local,sizeof local)||multicast_options(fd,interface_be))return fail_close(fd);
 uint8_t nonce[16],bytes[MNG_LAN_PACKET_MAX];arc4random_buf(nonce,sizeof nonce);nonce[0]|=1;
 size_t size=mng_lan_query_encode(bytes,title,nonce);
 if(sendto(fd,bytes,size,0,(const struct sockaddr *)destination,sizeof *destination)!=(ssize_t)size)return fail_close(fd);
 uint64_t deadline=now_ms()+(unsigned)timeout_ms;size_t count=0;unsigned received=0;
 while(now_ms()<deadline&&received<256){
  uint64_t now=now_ms();if(now>=deadline)break;
  struct sockaddr_in source;ssize_t n=receive(fd,bytes,&source,(int)(deadline-now));
  if(n<0){if(errno==EINTR||errno==EAGAIN)continue;return fail_close(fd);}
  received++;if(!n)continue;
  MngLanRoom room;
  if(source.sin_port!=destination->sin_port||!mng_lan_advert_decode(bytes,(size_t)n,title,nonce,&room)||!mng_ldn_filter_match(&room.network,filter))continue;
  room.source=source;size_t i;
  for(i=0;i<count;i++)if(!memcmp(out[i].room,room.room,16)&&out[i].source.sin_addr.s_addr==source.sin_addr.s_addr)break;
  if(i<count){if(room.revision>out[i].revision)out[i]=room;}
  else if(count<capacity)out[count++]=room;
 }
 close(fd);return (int)count;
}

int mng_lan_room_endpoint(const MngLanRoom *room,struct sockaddr_in *out){
 if(!room||!out||!room->control_port||room->control_version!=3||
    !unicast_source(&room->source)||!mng_ldn_network_valid(&room->network)||
    memcmp(room->room,room->network.network_id.session,16)){errno=EINVAL;return -1;}
 struct sockaddr_in endpoint={0};endpoint.sin_len=sizeof endpoint;endpoint.sin_family=AF_INET;
 endpoint.sin_addr=room->source.sin_addr;endpoint.sin_port=htons(room->control_port);
 *out=endpoint;return 0;
}
