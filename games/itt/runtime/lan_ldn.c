#include "lan_ldn.h"
#include "lan_mode.h"
#include "lan_host.h"
#include "lan_admission.h"
#include "lan_virtual.h"
#include <time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public libnx/switchbrew state values. Generic module-203 failure until exact
   SDK error contracts have been confirmed from executed guest call sites. */
enum { NONE, INITIALIZED, ACCESS_POINT, CREATED, STATION, CONNECTED };
#define UNAVAILABLE (203u | (1u << 9))
typedef struct { uint8_t changes, reserved[7]; } NodeUpdate;
_Static_assert(sizeof(NodeUpdate) == 8, "LDN node history ABI");
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state;
static uint64_t game_title;
static uint32_t interface_be;
static uint16_t discovery_port;
static MngLanHost *host;
static MngLanTransport *transport;
static MngLanAdmission *admission;
static MngLanMember *member;
static uint32_t virtual_ip;
static MngLdnNetwork observed;
static uint8_t changes[8];
static int transport_mode(void) {
    return mng_lan_mode()==MNG_LAN_TRANSPORT;
}
static void clear_room(void) {
    mng_lan_host_stop(host);host=NULL;
    if(transport) mng_udp_virtual_detach();
    mng_admission_stop(admission);admission=NULL;
    if(member) mng_member_leave(member);else mng_transport_close(transport);
    member=NULL;transport=NULL;virtual_ip=0;
    memset(&observed,0,sizeof observed);memset(changes,0,sizeof changes);
}
static int refresh(MngLdnNetwork *out) {
    if(state==CONNECTED && !mng_member_active(member)) {clear_room();state=STATION;return -1;}
    MngLdnNetwork n;
    int rc=state==CREATED && admission?mng_admission_network(admission,&n):
           state==CONNECTED?mng_member_network(member,&n):-1;
    if(rc || !mng_ldn_network_valid(&n)) return -1;
    for(unsigned i=0;i<8;++i) {
        if(!observed.nodes[i].connected && n.nodes[i].connected) changes[i]|=1;
        if(observed.nodes[i].connected && !n.nodes[i].connected) changes[i]|=2;
    }
    if(host && memcmp(&observed,&n,sizeof n)) {
        if(mng_lan_host_update(host,&n)) return -1;
    }
    observed=n;if(out) *out=n;return 0;
}
/* AP configuration exists before CreateNetwork. D3 sets its advertisement in
   ACCESS_POINT state; ITT may set it after creation. Own the bytes in either
   case, retain across Destroy/Create within the same AP, reset on AP close or
   Finalize so another session cannot publish stale configuration. Guard: lock. */
static uint8_t ap_advertise[384];
static uint16_t ap_advertise_size;
static void clear_advertise(void) {
    memset(ap_advertise, 0, sizeof ap_advertise); ap_advertise_size = 0;
}
/* NACP LocalCommunicationId[0] verified from each supported title's control NCA.
   Not an assumption that every title uses its application ID. */
static int64_t default_intent(void) {
    switch (game_title) {
    case 0x01001B300B9BE000ULL: return 0x01001B300B9BE000LL;
    case 0x010092A0172E4000ULL: return 0x010092A0172E4000LL;
    default: return -1;
    }
}
static int interface_address(const char *requested, struct in_addr *address) {
    if (requested && strcmp(requested, "auto")) return inet_pton(AF_INET, requested, address) == 1;
    if (!requested && !MNG_LAN_DEFAULT_TRANSPORT && !MNG_LAN_DISCOVERY_TEST_BUILD && !MNG_LAN_TRANSPORT_TEST_BUILD) return 0;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return 0;
    int found = 0;
    /* Prefer physical en interfaces over VPN/point-to-point interfaces. */
    for (int pass = 0; pass < 2 && !found; ++pass) for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
            !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST) ||
            (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) ||
            (!pass && strncmp(i->ifa_name, "en", 2))) continue;
        *address = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        if (address->s_addr) { found = 1; break; }
    }
    freeifaddrs(list);
    if (!found) address->s_addr = htonl(INADDR_LOOPBACK); /* same-Mac tests offline */
    return 1;
}
/* Bounded diagnostic output: no names, passphrases, tokens or payload bytes. */
static void trace(const char *operation, unsigned result) {
    static _Atomic unsigned lines;
    if (atomic_fetch_add(&lines, 1) < 256)
        fprintf(stderr, "[mng-lan] %s result=0x%x\n", operation, result);
}

static uint32_t initialize(void) {
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    const char *interface = getenv("MNG_LAN_INTERFACE");
    struct in_addr address;
    if (state == NONE && (!transport_mode() || !mng_udp_virtual_prepare()) && interface_address(interface, &address)) {
        uint32_t ip = ntohl(address.s_addr);
        const char *port = getenv("MNG_LAN_PORT");
        char *end = NULL;
        unsigned long value = port ? strtoul(port, &end, 10) : MNG_LAN_DISCOVERY_PORT;
        if (ip && (ip >> 24) < 224 && value > 0 && value <= 65535 && (!port || (*port && end && !*end))) {
            interface_be = address.s_addr; discovery_port = (uint16_t)value;
            state = INITIALIZED; result = 0;
            char ip_text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, ip_text, sizeof ip_text);
            fprintf(stderr, "[mng-lan] %s TEST mode: interface=%s port=%u\n",transport_mode()?"transport":"discovery",ip_text,discovery_port);
        }
    }
    pthread_mutex_unlock(&lock); trace("Initialize", result); return result;
}
static void finalize(void) {
    pthread_mutex_lock(&lock);
    clear_room(); clear_advertise(); state = NONE; trace("Finalize", 0);
    pthread_mutex_unlock(&lock);
}
static int get_state(void) {
    pthread_mutex_lock(&lock);if(transport) (void)refresh(NULL); int result = state; pthread_mutex_unlock(&lock);
    static _Atomic int last = -1;
    if (atomic_exchange(&last, result) != result) trace("GetState", (unsigned)result);
    return result;
}
static uint32_t transition(int from, int to) {
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    if (state == from) { state = to; result = 0; }
    pthread_mutex_unlock(&lock); return result;
}
static uint32_t open_ap(void) { uint32_t r = transition(INITIALIZED, ACCESS_POINT); trace("OpenAccessPoint", r); return r; }
static uint32_t open_station(void) { uint32_t r = transition(INITIALIZED, STATION); trace("OpenStation", r); return r; }
static uint32_t close_station(void) {
    pthread_mutex_lock(&lock);uint32_t r=UNAVAILABLE;
    if(state==STATION || state==CONNECTED) {clear_room();state=INITIALIZED;r=0;}
    pthread_mutex_unlock(&lock);trace("CloseStation",r);return r;
}
static uint32_t destroy(void) {
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    if (state == CREATED) {
        clear_room(); state = ACCESS_POINT; result = 0;
    }
    pthread_mutex_unlock(&lock); trace("DestroyNetwork", result); return result;
}
static uint32_t close_ap(void) {
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    if (state == ACCESS_POINT || state == CREATED) {
        clear_room(); clear_advertise(); state = INITIALIZED; result = 0;
    }
    pthread_mutex_unlock(&lock); trace("CloseAccessPoint", result); return result;
}
static uint32_t create(const MngLdnConfig *config, const MngLdnSecurity *security, const MngLdnUser *user) {
    if (!config || !security || !user || config->max_nodes < 1 || config->max_nodes > 8 ||
        config->version < 0 || config->intent.communication_id < -1 ||
        security->mode < 1 || security->mode > 2 || security->size < 16 || security->size > 64 ||
        !memchr(user->name, 0, sizeof user->name)) { trace("CreateNetwork invalid configuration", UNAVAILABLE); return UNAVAILABLE; }
    /* Only admission owns a credential copy; discovery never receives it. */
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    int64_t intent = config->intent.communication_id == -1 ? default_intent() : config->intent.communication_id;
    if (state == ACCESS_POINT && intent >= 0) {
        MngLanRoom room = {0};
        arc4random_buf(room.room, 16); room.room[0] |= 1; room.revision = 1;
        MngLdnNetwork *n = &room.network;
        n->network_id.intent.communication_id = intent;
        n->network_id.intent.scene = config->intent.scene;
        memcpy(n->network_id.session, room.room, 16);
        arc4random_buf(n->server_random, 16);
        memcpy(n->common.bssid, room.room, 6);
        n->common.bssid[0] = (n->common.bssid[0] & 0xfc) | 2;
        n->common.ssid.length = 32;
        for (unsigned i = 0; i < 16; ++i) snprintf(n->common.ssid.value + i*2, 3, "%02x", room.room[i]);
        n->common.channel = config->channel; n->common.type = 2; n->common.link_level = 3;
        n->security = security->mode; n->accept_policy = 1; /* reject: no admission yet */
        n->max_nodes = config->max_nodes; n->node_count = 1;
        n->nodes[0].connected = 1; n->nodes[0].version = config->version;
        memcpy(n->nodes[0].mac, n->common.bssid, 6);
        memcpy(n->nodes[0].name, user->name, strlen(user->name) + 1);
        memcpy(n->advertise, ap_advertise, sizeof n->advertise);
        n->advertise_size = ap_advertise_size;
        if(transport_mode()) {
            n->accept_policy=0;n->nodes[0].ipv4=0x0a4d0001;
            transport=mng_transport_open(interface_be,game_title,room.room,0x0a4d0001);
            if(transport) admission=mng_admission_host_network(transport,game_title,room.room,security->passphrase,security->size,n);
            struct sockaddr_in endpoint;
            if(!admission || mng_admission_endpoint(admission,&endpoint) || mng_udp_virtual_attach(transport)) {
                clear_room();pthread_mutex_unlock(&lock);trace("CreateNetwork transport failed",UNAVAILABLE);return UNAVAILABLE;
            }
            virtual_ip=0x0a4d0001;room.control_port=ntohs(endpoint.sin_port);room.control_version=3;
        }
        host = mng_lan_host_start(interface_be, discovery_port, 1, game_title, &room);
        if(!host && transport) clear_room();
        if (host) { state = CREATED; result = 0;if(transport) (void)refresh(NULL); }
    }
    pthread_mutex_unlock(&lock); trace("CreateNetwork", result); return result;
}
static uint32_t advertise(const void *data, size_t size) {
    if (size > sizeof ap_advertise || (!data && size)) {
        trace("SetAdvertiseData invalid buffer", UNAVAILABLE); return UNAVAILABLE;
    }
    pthread_mutex_lock(&lock);
    MngLanRoom room; uint32_t result = UNAVAILABLE;
    uint8_t next[sizeof ap_advertise] = {0};
    if (state == ACCESS_POINT || state == CREATED) {
        if (size) memcpy(next, data, size);
        if (state == ACCESS_POINT) result = 0;
        else if (!mng_lan_host_snapshot(host, &room)) {
            memcpy(room.network.advertise, next, sizeof next);
            room.network.advertise_size = (uint16_t)size;
            if ((!admission || !mng_admission_advertise(admission,next,size)) && !mng_lan_host_update(host, &room.network)) result = 0;
        }
        /* Do not change the pending configuration when publication fails. */
        if (!result) {
            memcpy(ap_advertise, next, sizeof next); ap_advertise_size = (uint16_t)size;
        }
    }
    pthread_mutex_unlock(&lock); trace("SetAdvertiseData", result); return result;
}
static uint32_t network_info(MngLdnNetwork *out) {
    if (!out) return UNAVAILABLE;
    pthread_mutex_lock(&lock);
    MngLanRoom room; uint32_t result = UNAVAILABLE;
    if(transport) {if(!refresh(out)) result=0;}
    else if (state == CREATED && !mng_lan_host_snapshot(host, &room)) { *out = room.network; result = 0; }
    pthread_mutex_unlock(&lock); return result;
}
static uint32_t network_history(MngLdnNetwork *out, NodeUpdate *updates, int count) {
    if (count < 0 || count > 8 || (!updates && count)) return UNAVAILABLE;
    uint32_t result = network_info(out);
    if (!result && count) {
        memset(updates,0,sizeof *updates*(unsigned)count);
        pthread_mutex_lock(&lock);
        for(int i=0;i<count;++i) {updates[i].changes=changes[i];changes[i]=0;}
        pthread_mutex_unlock(&lock);
    }
    return result;
}
static uint32_t scan(MngLdnNetwork *out, int *count, int capacity, const MngLdnFilter *filter, int channel) {
    /* The guest output capacity is not our discovery cache capacity. Accept a
       larger output array but never write more than the bounded cache holds. */
    static _Atomic unsigned scan_calls;
    if (atomic_fetch_add(&scan_calls, 1) < 32)
        fprintf(stderr, "[mng-lan] Scan capacity=%d channel=%d flags=0x%x\n",
                capacity, channel, filter ? filter->flags : 0);
    if (!out || !count || !filter || capacity < 1 ||
        filter->flags & ~63u || channel < 0) { trace("Scan invalid arguments", UNAVAILABLE); return UNAVAILABLE; }
    size_t bounded_capacity = capacity < MNG_LAN_ROOMS_MAX ? (size_t)capacity : MNG_LAN_ROOMS_MAX;
    pthread_mutex_lock(&lock);
    uint32_t result = UNAVAILABLE;
    /* D3 scans while hosting its own room; this must not tear down the AP or
       stop its responder. Keep the lifecycle lock across the bounded scan. */
    MngLanRoom own;
    int have_own = state == CREATED;
    if ((state == INITIALIZED || state == ACCESS_POINT || state == CREATED || state == STATION || state == CONNECTED) &&
        (!have_own || !mng_lan_host_snapshot(host, &own))) {
        struct sockaddr_in dest = {0}; dest.sin_len = sizeof dest; dest.sin_family = AF_INET;
        dest.sin_port = htons(discovery_port); inet_pton(AF_INET, MNG_LAN_DISCOVERY_GROUP, &dest.sin_addr);
        MngLanRoom rooms[MNG_LAN_ROOMS_MAX];
        MngLdnFilter resolved = *filter;
        if ((resolved.flags & 1) && resolved.network_id.intent.communication_id == -1)
            resolved.network_id.intent.communication_id = default_intent();
        int found = mng_lan_scan(&dest, interface_be, game_title, &resolved, rooms, MNG_LAN_ROOMS_MAX, 300);
        if (found < 0) {
            int error = errno;
            trace("Scan host errno", (unsigned)error);
        }
        if (found >= 0) {
            int written = 0;
            /* Collect before applying caller capacity: our own multicast reply
               or a different channel must not consume the caller's only slot.
               Do not exclude by host IP: same-Mac peers share that address. */
            for (int i = 0; i < found && (size_t)written < bounded_capacity; ++i) {
                if (have_own && !memcmp(rooms[i].room, own.room, sizeof own.room)) continue;
                if (!channel || rooms[i].network.common.channel == channel)
                    out[written++] = rooms[i].network;
            }
            *count = written; result = 0; trace("Scan room count", (unsigned)written);
        }
    }
    pthread_mutex_unlock(&lock); trace("Scan", result); return result;
}
static uint32_t disconnect_station(void) {
    pthread_mutex_lock(&lock);uint32_t r=UNAVAILABLE;
    if(state==CONNECTED) {clear_room();state=STATION;r=0;}
    pthread_mutex_unlock(&lock);trace("Disconnect",r);return r;
}
static uint32_t get_address(uint32_t *address, uint32_t *mask) {
    if(!address || !mask) return UNAVAILABLE;
    pthread_mutex_lock(&lock);uint32_t r=UNAVAILABLE;
    if(transport && !refresh(NULL)) {*address=virtual_ip;*mask=0xffffff00u;r=0;}
    pthread_mutex_unlock(&lock);trace("GetIpv4Address",r);return r;
}
static uint32_t connect_station(const MngLdnNetwork *network, const MngLdnSecurity *security,
                                const MngLdnUser *user, int version, unsigned option) {
    static _Atomic unsigned connect_lines;
    int diagnostic=atomic_fetch_add(&connect_lines,1)<24;
    const char *stage="arguments";int error=0,found=0;
    uint32_t result=UNAVAILABLE;
    /* libnx LdnSecurityMode_Any=0. ITT supplies Any to Connect but Product
       to Create. Any selects the room mode, NEVER disables PSK admission. */
    if(!transport_mode() || !network || !mng_ldn_network_valid(network) || !security || !user ||
       security->size<16 || security->size>64 ||
       (security->mode && security->mode!=network->security) ||
       version<0 || version>32767 || version!=network->nodes[0].version || option ||
       !memchr(user->name,0,sizeof user->name)) {
        if(diagnostic) fprintf(stderr,"[mng-lan] Connect stage=%s version=%d option=%u result=0x%x\n",stage,version,option,result);
        return result;
    }
    pthread_mutex_lock(&lock);stage="state";
    if(state==STATION) {
        /* The game may retain NetworkInfo across Finalize or a long menu
           pause. Resolve its exact identity afresh; never trust a saved native
           endpoint or treat discovery itself as authorization. */
        stage="resolve";
        struct sockaddr_in dest={0};dest.sin_len=sizeof dest;dest.sin_family=AF_INET;
        dest.sin_port=htons(discovery_port);inet_pton(AF_INET,MNG_LAN_DISCOVERY_GROUP,&dest.sin_addr);
        MngLdnFilter filter={0};filter.flags=1|2|32;filter.network_id=network->network_id;
        MngLanRoom rooms[MNG_LAN_ROOMS_MAX];
        found=mng_lan_scan(&dest,interface_be,game_title,&filter,rooms,MNG_LAN_ROOMS_MAX,300);
        if(found<0) error=errno;
        const MngLanRoom *selected=NULL;
        for(int i=0;i<found;++i) if(!memcmp(&network->network_id,&rooms[i].network.network_id,sizeof network->network_id)) {
            if(selected) {selected=NULL;stage="ambiguous";break;}selected=&rooms[i];
        }
        struct sockaddr_in endpoint;
        if(selected && !mng_lan_room_endpoint(selected,&endpoint)) {
            stage="admission";
            MngLdnNode node={0};memcpy(node.name,user->name,strlen(user->name)+1);node.version=(int16_t)version;
            member=mng_admission_join_network(interface_be,&endpoint,game_title,selected->room,
                security->passphrase,security->size,2000,&node,&virtual_ip);
            if(!member) error=errno;
            else {
                stage="authenticated-network";
                transport=mng_member_transport(member);MngLdnNetwork authenticated;
                if(!mng_member_network(member,&authenticated) && mng_ldn_network_valid(&authenticated) &&
                   !memcmp(&authenticated.network_id,&network->network_id,sizeof network->network_id) &&
                   authenticated.security==network->security && authenticated.nodes[0].version==version) {
                    stage="attach";
                    if(!mng_udp_virtual_attach(transport)) {
                        state=CONNECTED;result=0;stage="connected";(void)refresh(NULL);
                    } else error=errno;
                }
                if(result) clear_room();
            }
        } else if(selected) {stage="endpoint";error=errno;}
    }
    if(diagnostic) fprintf(stderr,"[mng-lan] Connect stage=%s state=%d rooms=%d errno=%d result=0x%x\n",stage,state,found,error,result);
    pthread_mutex_unlock(&lock);trace("Connect",result);return result;
}
static uint32_t make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}
static uint8_t equal_ipv4(uint32_t a, uint32_t b) { return a == b; }
void *mng_lan_ldn_override(const char *name, uint64_t title) {
    if (!name || !title || mng_lan_mode()==MNG_LAN_OFF) return NULL;
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn3ldn10InitializeEv", initialize}, {"_ZN2nn3ldn8FinalizeEv", finalize},
        {"_ZN2nn3ldn8GetStateEv", get_state}, {"_ZN2nn3ldn10DisconnectEv", disconnect_station},
        {"_ZN2nn3ldn11OpenStationEv", open_station}, {"_ZN2nn3ldn12CloseStationEv", close_station},
        {"_ZN2nn3ldn15OpenAccessPointEv", open_ap}, {"_ZN2nn3ldn16CloseAccessPointEv", close_ap},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", create},
        {"_ZN2nn3ldn14DestroyNetworkEv", destroy},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", get_address},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", network_info},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoEPNS0_16NodeLatestUpdateEi", network_history},
        {"_ZN2nn3ldn15MakeIpv4AddressEhhhh", make_ipv4}, {"_ZN2nn3ldneqENS0_11Ipv4AddressES1_", equal_ipv4},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", advertise},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", scan},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", connect_station},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; ++i) if (!strcmp(name, entries[i].name)) {
        pthread_mutex_lock(&lock);
        if (game_title && game_title != title) { pthread_mutex_unlock(&lock); return NULL; }
        game_title = title;
        pthread_mutex_unlock(&lock); return entries[i].fn;
    }
    return NULL;
}
