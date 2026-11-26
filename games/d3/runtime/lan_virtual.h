/* Private virtual UDP backend. No discovery/admission or automatic activation.
   Prepare is one-way and must precede every socket in this process. Attach is
   control-plane only: caller supplies a transport with trusted peers. Transport
   is borrowed until detach returns; never close it or call receive meanwhile.
   Detach flushes queues and cancels blocked operations; bindings survive.
   Revoked peers are filtered from queues on receive/poll (no address reuse).
   Room teardown still requires detach before closing the borrowed transport.
   New room lifetimes require a FRESH transport, not reattachment of a transport
   with pending native packets from the preceding lifetime. */
#pragma once
#include "lan_udp.h"
#include "lan_transport.h"
int mng_udp_virtual_prepare(void);
int mng_udp_virtual_attach(MngLanTransport *transport);
void mng_udp_virtual_detach(void);
/* Backend entry points; callers normally use lan_udp.h instead. */
int mng_vudp_attach(MngLanTransport *transport);
int mng_vudp_open(int *error);
int mng_vudp_close(int handle, int *error);
int mng_vudp_nonblock(int handle, int enabled, int *error);
int mng_vudp_get_nonblock(int handle, int *error);
int mng_vudp_bind(int handle, const MngUdpAddress *address, int *error);
int mng_vudp_name(int handle, MngUdpAddress *address, int *error);
int mng_vudp_connect(int handle, const MngUdpAddress *address, int *error);
int mng_vudp_peer(int handle, MngUdpAddress *address, int *error);
int mng_vudp_shutdown(int handle, int how, int *error);
int mng_vudp_option(int handle, int set, int level, int option, int *value, int *error);
int mng_vudp_send_flags(int handle, const void *data, size_t size, const MngUdpAddress *to, int flags, int *error);
int mng_vudp_receive_flags(int handle, void *data, size_t size, MngUdpAddress *from, int flags, int *error);
int mng_vudp_poll(MngUdpPoll *fds, size_t count, int timeout_ms, int *error);
