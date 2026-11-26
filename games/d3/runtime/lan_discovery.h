/* LAN discovery used by opt-in guest scanning and isolated transport tests.
   No constructor, external service, save access, or online-account emulation.
   Discovery advertisements are untrusted; a successful scan NEVER admits a peer
   to a room. Join authentication and game-data transport are separate work. */
#pragma once
#include "ldn_abi.h"
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#define MNG_LAN_DISCOVERY_PORT 37771
#define MNG_LAN_DISCOVERY_GROUP "239.255.77.71"
#define MNG_LAN_PACKET_MAX 1216
#define MNG_LAN_ROOMS_MAX 16

typedef struct {
 uint8_t room[16]; uint64_t revision; MngLdnNetwork network;
 struct sockaddr_in source;
 /* Untrusted admission hint, numeric host-order port. 0 = discovery-only.
    Version3 refers to the authenticated admission wire, NOT proof of membership.
    Source IPv4 comes from recvmsg, never a caller-controlled advertised IP. */
 uint16_t control_port;
 uint8_t control_version;
} MngLanRoom;
/* Packet framing uses big endian integers; the opaque guest structure is the
   little-endian ARM64 LDN ABI. Reserved/header fields must be zero. */
size_t mng_lan_query_encode(uint8_t out[MNG_LAN_PACKET_MAX],uint64_t title,const uint8_t nonce[16]);
size_t mng_lan_advert_encode(uint8_t out[MNG_LAN_PACKET_MAX],uint64_t title,const uint8_t nonce[16],const MngLanRoom *room);
int mng_lan_query_decode(const void *data,size_t size,uint64_t title,uint8_t nonce[16]);
int mng_lan_advert_decode(const void *data,size_t size,uint64_t title,const uint8_t nonce[16],MngLanRoom *room);
int mng_ldn_network_valid(const MngLdnNetwork *network);
int mng_ldn_filter_match(const MngLdnNetwork *network,const MngLdnFilter *filter);
/* Host listener: explicit interface address; 0 means route-selected interface.
   port=0 allows an OS-assigned test port. multicast=0 is loopback/unicast testing. */
int mng_lan_listener_open(uint32_t interface_be,uint16_t port,int multicast);
/* Wait at most timeout_ms (0..2000). At most one response, sent only to a valid
   query from a unicast IPv4 source. Caller owns pacing/rate limits. */
int mng_lan_serve_once(int fd,uint64_t title,const MngLanRoom *room,int timeout_ms);
/* Send one query, collect matching nonce/title replies, deduplicate by room ID
   and source address. Deadline is monotonic and bounded to 2000 ms. */
int mng_lan_scan(const struct sockaddr_in *destination,uint32_t interface_be,uint64_t title,
                 const MngLdnFilter *filter,MngLanRoom *out,size_t capacity,int timeout_ms);

/* Convert only a supported, internally consistent endpoint advertisement.
   Leaves output untouched on error. Still requires credential-based admission. */
int mng_lan_room_endpoint(const MngLanRoom *room, struct sockaddr_in *out);
