/* IPv4 UDP transport substrate. Guest handles are never host descriptors.
   Addresses/ports use network byte order. Errors use the Horizon BSD ABI.
   Private control-plane setup (lan_virtual.h) may select virtual routing before
   the first open; otherwise these calls retain native UDP behavior. No LDN
   address allocation or membership is implied by this API. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t address; uint16_t port; } MngUdpAddress;
int mng_udp_open(int *error);
int mng_udp_close(int handle, int *error);
int mng_udp_nonblock(int handle, int enabled, int *error);
int mng_udp_bind(int handle, const MngUdpAddress *address, int *error);
int mng_udp_name(int handle, MngUdpAddress *address, int *error);
int mng_udp_send(int handle, const void *data, size_t size, const MngUdpAddress *to, int *error);
int mng_udp_receive(int handle, void *data, size_t size, MngUdpAddress *from, int *error);
/* Guest BSD flags/options are translated, not forwarded to the host. */
int mng_udp_get_nonblock(int handle, int *error);
int mng_udp_connect(int handle, const MngUdpAddress *address, int *error);
int mng_udp_peer(int handle, MngUdpAddress *address, int *error);
int mng_udp_shutdown(int handle, int how, int *error);
int mng_udp_option(int handle, int set, int level, int option, int *value, int *error);
int mng_udp_send_flags(int handle, const void *data, size_t size, const MngUdpAddress *to, int flags, int *error);
int mng_udp_receive_flags(int handle, void *data, size_t size, MngUdpAddress *from, int flags, int *error);
typedef struct { int32_t handle; int16_t events, revents; } MngUdpPoll;
int mng_udp_poll(MngUdpPoll *fds, size_t count, int timeout_ms, int *error);
