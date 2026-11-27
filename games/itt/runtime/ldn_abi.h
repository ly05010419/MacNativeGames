/* Guest LDN layouts, described by switchbrew/libnx ldn.h. This is a host-side
   layout declaration, not an SDK implementation. IPv4 fields are host numeric
   values (e.g. 0xc0a80101); socket sockaddr addresses use network byte order. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int64_t communication_id; uint8_t reserved0[2]; uint16_t scene; uint8_t reserved1[4]; } MngLdnIntent;
typedef struct { MngLdnIntent intent; uint8_t session[16]; } MngLdnNetworkId;
typedef struct { uint8_t length; char value[33]; } MngLdnSsid;
typedef struct { uint8_t bssid[6]; MngLdnSsid ssid; int16_t channel; int8_t link_level; uint8_t type; uint8_t reserved[4]; } MngLdnCommon;
typedef struct {
 uint32_t ipv4; uint8_t mac[6]; int8_t id; uint8_t connected; char name[33];
 uint8_t platform; int16_t version; uint8_t reserved[16];
} MngLdnNode;
typedef struct {
 MngLdnNetworkId network_id; MngLdnCommon common;
 uint8_t server_random[16]; uint16_t security; uint8_t accept_policy,version,reserved0[2];
 int8_t max_nodes; uint8_t node_count; MngLdnNode nodes[8];
 uint8_t reserved1[2]; uint16_t advertise_size; uint8_t advertise[384];
 uint8_t reserved2[140]; uint64_t reserved3;
} MngLdnNetwork;
typedef struct {
 MngLdnNetworkId network_id; uint32_t network_type; uint8_t bssid[6];
 MngLdnSsid ssid; uint8_t reserved[16]; uint32_t flags;
} MngLdnFilter;
typedef struct { MngLdnIntent intent; int16_t channel; int8_t max_nodes; uint8_t reserved0; int16_t version; uint8_t reserved1[10]; } MngLdnConfig;
typedef struct { uint16_t mode,size; uint8_t passphrase[64]; } MngLdnSecurity;
typedef struct { char name[33]; uint8_t reserved[15]; } MngLdnUser;
_Static_assert(sizeof(MngLdnIntent)==0x10,"LDN IntentId");
_Static_assert(sizeof(MngLdnNode)==0x40,"LDN NodeInfo");
_Static_assert(sizeof(MngLdnNetwork)==0x480,"LDN NetworkInfo");
_Static_assert(offsetof(MngLdnNetwork,nodes)==0x68,"LDN nodes");
_Static_assert(offsetof(MngLdnNetwork,advertise_size)==0x26a,"LDN advertise length");
_Static_assert(offsetof(MngLdnNetwork,advertise)==0x26c,"LDN advertise payload");
_Static_assert(sizeof(MngLdnFilter)==0x60,"LDN ScanFilter");
_Static_assert(sizeof(MngLdnConfig)==0x20,"LDN NetworkConfig");
_Static_assert(sizeof(MngLdnSecurity)==0x44,"LDN SecurityConfig");
_Static_assert(sizeof(MngLdnUser)==0x30,"LDN UserConfig");
