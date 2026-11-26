/* Experimental PSK room admission, not Nintendo authentication or LDN hooks.
   Caller provides the game's room credential (16..64 bytes). Credentials must
   match on both sides; no secret or derived key is published in discovery.
   HMAC challenge-response authenticates possession, NOT a human/user identity.
   Weak/shared game credentials permit offline guessing/other credential holders.
   Packets/data are not encrypted. No forward secrecy or station mesh.
   This implementation admits host<->station links only, NOT a station mesh. */
#pragma once
#include "lan_transport.h"
#include "ldn_abi.h"
typedef struct MngLanAdmission MngLanAdmission;
typedef struct MngLanMember MngLanMember;
/* Transport must be host .1 and remain alive until stop. Uses a separate,
   ephemeral control UDP port and one owned worker. No virtual socket attach.
   At most max_nodes-1 peers, with no address reuse in this room lifetime. */
MngLanAdmission *mng_admission_host(MngLanTransport *transport, uint64_t title,
                                   const uint8_t room[16], const void *credential,
                                   size_t credential_size, unsigned max_nodes);
int mng_admission_endpoint(MngLanAdmission *host, struct sockaddr_in *out);
/* Committed transport members, including host. Pending reservations excluded. */
int mng_admission_count(MngLanAdmission *host);
/* Stops/joins worker and revokes all installed peers; does not close borrowed
   transport. Owner must still detach virtual sockets before transport close. */
void mng_admission_stop(MngLanAdmission *host);
/* Synchronous bounded join (100..5000ms). Untrusted destination/title/room may
   come from discovery; PSK proof is still required. Creates its own transport,
   installs host link only after verified acceptance. On failure closes all
   sockets, erases secrets, leaves assigned untouched. Result owns transport and
   heartbeat worker. Call member_leave only AFTER detaching virtual sockets.
   Heartbeat every 500ms, lease 3000ms, pending reservation 6000ms. Committed
   addresses remain retired after departure; room recreation needed for reuse.
   Host-only members (lost ACCEPT) expire. No reliable leave acknowledgement. */
MngLanMember *mng_admission_join(uint32_t interface_be,
                                  const struct sockaddr_in *destination,
                                  uint64_t title, const uint8_t room[16],
                                  const void *credential, size_t credential_size,
                                  int timeout_ms, uint32_t *assigned);

/* Borrowed transport lives until member_leave. Owner must not close it. */
MngLanTransport *mng_member_transport(MngLanMember *member);
int mng_member_active(MngLanMember *member);
/* Stops/joins heartbeat worker, best-effort authenticated departure, revokes
   host link, closes owned transport and erases secrets. No concurrent callers. */
void mng_member_leave(MngLanMember *member);

/* Authenticated guest metadata variant. Network must describe this host/room.
   Host network snapshot is refreshed on join/leave/heartbeat, and carried in
   authenticated ACCEPT/ALIVE. Untrusted discovery metadata is never authoritative. */
MngLanAdmission *mng_admission_host_network(MngLanTransport *transport, uint64_t title,
    const uint8_t room[16], const void *credential, size_t credential_size,
    const MngLdnNetwork *network);
int mng_admission_network(MngLanAdmission *host, MngLdnNetwork *out);
int mng_admission_advertise(MngLanAdmission *host, const void *data, size_t size);
MngLanMember *mng_admission_join_network(uint32_t interface_be,
    const struct sockaddr_in *destination, uint64_t title, const uint8_t room[16],
    const void *credential, size_t credential_size, int timeout_ms,
    const MngLdnNode *node, uint32_t *assigned);
int mng_member_network(MngLanMember *member, MngLdnNetwork *out);
