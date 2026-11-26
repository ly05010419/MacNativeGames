/* Experimental LDN data plane, NOT guest IP assignment or admission.
   A trusted membership layer must supply peers and fresh pairwise keys. Never
   derive authorization from unauthenticated discovery advertisements.
   IPv4/guest ports are numeric host-order; native endpoint fields are separate.
   Packets are authenticated (HMAC-SHA256), not encrypted. */
#pragma once
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#define MNG_DATA_MAX_PEERS 7
#define MNG_DATA_MAX_PAYLOAD (65507u - 96u)
typedef struct MngLanTransport MngLanTransport;
typedef struct {
    uint32_t address;          /* virtual 10.77.0.1 .. 10.77.0.254; never OS-bound */
    uint64_t cookie;           /* fresh per transport lifetime */
    struct sockaddr_in native;
} MngLanEndpoint;
typedef struct {
    uint32_t source, destination;
    uint16_t source_port, destination_port;
    size_t size;
    uint8_t data[MNG_DATA_MAX_PAYLOAD];
} MngLanDatagram;
/* Explicit local interface; OS chooses an independent native port per process.
   Addresses are supplied by a future room allocator, not chosen/claimed here.
   Creation starts no threads. NULL on failure, errno is a host errno. */
MngLanTransport *mng_transport_open(uint32_t interface_be, uint64_t title,
                                  const uint8_t room[16], uint32_t address);
/* Immutable room context check for a control-plane owner. */
int mng_transport_context(MngLanTransport *transport, uint64_t title, const uint8_t room[16]);
/* Admission-only address assignment before adding ANY peers. Caller must have
   exclusive ownership: no concurrent API calls. Changes no native endpoint. */
int mng_transport_assign(MngLanTransport *transport, uint32_t address);
int mng_transport_endpoint(MngLanTransport *transport, MngLanEndpoint *out);
/* Control-plane only. Reject duplicate virtual identities, native endpoints,
   cookies or keys, including reuse of retired identities/keys this lifetime.
   Key is a fresh, nonzero 32-byte pairwise secret, NOT a discovery field. */
int mng_transport_add_peer(MngLanTransport *transport, const MngLanEndpoint *peer,
                           const uint8_t key[32]);
/* Current authorization, for dropping already queued data after revocation. */
int mng_transport_has_peer(MngLanTransport *transport, uint32_t address);
int mng_transport_remove_peer(MngLanTransport *transport, uint32_t address);
/* Datagram semantics: no reliability, broadcast, TCP or relay emulation yet.
   Returns payload size (including zero), or -1 with errno. Unknown peers fail. */
int mng_transport_send(MngLanTransport *transport, uint32_t destination,
                       uint16_t source_port, uint16_t destination_port,
                       const void *data, size_t size);
/* One receive consumer per transport. Returns 1=datagram, 0=timeout, -1=error.
   Deadline 0..2000ms; at most 256 packets inspected per call. All invalid,
   unauthenticated, stale-session and replayed packets are discarded. Output is
   untouched unless return=1. Other threads may send or edit membership. */
int mng_transport_receive(MngLanTransport *transport, MngLanDatagram *out, int timeout_ms);
/* Owner must stop/join all API callers first. Erases keys and closes socket. */
void mng_transport_close(MngLanTransport *transport);
