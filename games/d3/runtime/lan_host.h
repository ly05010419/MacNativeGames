/* Experimental room-advertisement lifetime, explicitly started by its owner.
   Used by the opt-in discovery-only LDN adapter. Does not admit members or carry game
   packets. Caller serializes update/snapshot/stop; worker state is protected. */
#pragma once
#include "lan_discovery.h"
typedef struct MngLanHost MngLanHost;
/* Opens the listener before returning, then services queries on a bounded-rate
   worker. The supplied room must be valid. No secret/passphrase is accepted. */
MngLanHost *mng_lan_host_start(uint32_t interface_be, uint16_t port, int multicast,
                             uint64_t title, const MngLanRoom *room);
uint16_t mng_lan_host_port(const MngLanHost *host);
/* Identity cannot change within a lifetime. Revision is advanced by this API.
   Failure leaves the published room unchanged. */
int mng_lan_host_update(MngLanHost *host, const MngLdnNetwork *network);
/* Snapshot includes asynchronous listener failure as errno; no partial output. */
int mng_lan_host_snapshot(MngLanHost *host, MngLanRoom *out);
/* Joins the worker and closes its listener. No replies after return. NULL safe. */
void mng_lan_host_stop(MngLanHost *host);
