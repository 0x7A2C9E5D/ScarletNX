/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Eden room (common/network/net_room.c) against a room of this test's own: an ENet server that reads and relays the
   messages as Eden's src/network/room.cpp does, from its own reading of the bytes, so a field the member puts in the
   wrong place shows here. The messages byte for byte; joining and each refusal; ldn's networks through the room (scan,
   join, sync, leave, a station that leaves the room without a word, the host destroying its network); the game's
   datagram sockets (to a member, broadcasts, Eden's protocol byte, poll, timeouts, truncation, a big datagram); and the
   room closing */
#include "common/network/net_ldn.h"
#include "common/network/net_room.h"
#include "common/network/net_internal.h"
#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ldn_room.c:%d: check failed: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
static void sleep_ms(int ms) { snx_net_poll(NULL, 0, ms); }
static void quiet(const char* m) { (void)m; }

/* ============================================================ the room */
#define ROOM_SLOTS 4
typedef struct FakeMember {
    ENetPeer* peer;
    char nickname[64];
    uint8_t ip[4];
} FakeMember;
typedef struct FakeRoom {
    SnxLock lk;
    ENetHost* host;
    uint16_t port;
    FakeMember members[ROOM_SLOTS];
    int stop, done, close_now;
    uint8_t last_datagram[64]; /* the start of the last datagram message relayed */
    size_t last_datagram_size;
    char last_game[64];
    uint64_t last_game_id;
} FakeRoom;
static FakeRoom g_room;

typedef struct Rd {
    const uint8_t* p;
    size_t n, at;
    int bad;
} Rd;
static uint32_t rd_be(Rd* r, int bytes) {
    uint32_t v = 0;
    int i;
    if (r->at + (size_t)bytes > r->n) {
        r->bad = 1;
        return 0;
    }
    for (i = 0; i < bytes; i++) v = v << 8 | r->p[r->at++];
    return v;
}
static void rd_string(Rd* r, char* out, size_t size) {
    uint32_t n = rd_be(r, 4);
    if (r->bad || r->at + n > r->n || n >= size) {
        r->bad = 1;
        out[0] = 0;
        return;
    }
    memcpy(out, r->p + r->at, n);
    out[n] = 0;
    r->at += n;
}
typedef struct Wr {
    uint8_t b[2048];
    size_t n;
} Wr;
static void wr_be(Wr* w, uint32_t v, int bytes) {
    while (bytes--) w->b[w->n++] = (uint8_t)(v >> (8 * bytes));
}
static void wr_string(Wr* w, const char* s) {
    wr_be(w, (uint32_t)strlen(s), 4);
    memcpy(w->b + w->n, s, strlen(s));
    w->n += strlen(s);
}
static void room_send(ENetPeer* peer, const uint8_t* data, size_t size) {
    enet_peer_send(peer, 0, enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE));
}
/* room.cpp's BroadcastRoomInformation */
static void room_information(FakeRoom* f) {
    Wr w;
    int i, n = 0;
    w.n = 0;
    wr_be(&w, 3, 1);
    wr_string(&w, "Test room");
    wr_string(&w, "");
    wr_be(&w, ROOM_SLOTS, 4);
    wr_be(&w, f->port, 2);
    wr_string(&w, "");
    wr_string(&w, "");
    for (i = 0; i < ROOM_SLOTS; i++) n += f->members[i].peer != NULL;
    wr_be(&w, (uint32_t)n, 4);
    for (i = 0; i < ROOM_SLOTS; i++) {
        if (!f->members[i].peer) continue;
        wr_string(&w, f->members[i].nickname);
        memcpy(w.b + w.n, f->members[i].ip, 4);
        w.n += 4;
        wr_string(&w, "game");
        wr_be(&w, 0, 4);
        wr_be(&w, 0, 4);
        wr_string(&w, "1.0");
        wr_string(&w, "");
        wr_string(&w, "");
        wr_string(&w, "");
    }
    for (i = 0; i < ROOM_SLOTS; i++)
        if (f->members[i].peer) room_send(f->members[i].peer, w.b, w.n);
}
/* room.cpp's IsValidNickname, written apart from users.c's */
static int nickname_ok(FakeRoom* f, const char* s) {
    size_t n = strlen(s), i;
    if (n < 4 || n > 20) return 0;
    for (i = 0; i < n; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-", s[i])) return 0;
    for (i = 0; i < ROOM_SLOTS; i++)
        if (f->members[i].peer && !strcmp(f->members[i].nickname, s)) return 0;
    return 1;
}
static void room_join(FakeRoom* f, ENetPeer* peer, const uint8_t* data, size_t size) {
    char nickname[64], password[64], token[64];
    uint8_t answer = 0, ip[4], ok[5];
    uint32_t version;
    Rd r;
    int i, free_slot = -1;
    r.p = data;
    r.n = size;
    r.at = 1;
    r.bad = 0;
    rd_string(&r, nickname, sizeof nickname);
    for (i = 0; i < 4; i++) ip[i] = (uint8_t)rd_be(&r, 1);
    version = rd_be(&r, 4);
    rd_string(&r, password, sizeof password);
    rd_string(&r, token, sizeof token);
    CHECK(!r.bad && r.at == size); /* the join request, whole */
    CHECK(ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255 && !token[0]);
    for (i = 0; i < ROOM_SLOTS && free_slot < 0; i++)
        if (!f->members[i].peer) free_slot = i;
    if (free_slot < 0)
        answer = 13; /* IdRoomIsFull */
    else if (strcmp(password, "secret"))
        answer = 11; /* IdWrongPassword */
    else if (!nickname_ok(f, nickname))
        answer = 8; /* IdNameCollision */
    else if (version != 1)
        answer = 10; /* IdVersionMismatch */
    if (answer) {
        room_send(peer, &answer, 1);
        return;
    }
    f->members[free_slot].peer = peer;
    strcpy(f->members[free_slot].nickname, nickname);
    f->members[free_slot].ip[0] = 192; /* GenerateFakeIPAddress: the first free */
    f->members[free_slot].ip[1] = 168;
    f->members[free_slot].ip[2] = 1;
    f->members[free_slot].ip[3] = (uint8_t)(free_slot + 1);
    room_information(f);
    ok[0] = 2; /* IdJoinSuccess */
    memcpy(ok + 1, f->members[free_slot].ip, 4);
    room_send(peer, ok, 5);
}
/* room.cpp's HandleProxyPacket and HandleLdnPacket: everyone but the sender for a broadcast, else the member at the
   remote address */
static void room_relay(FakeRoom* f, ENetPeer* from, const uint8_t* data, size_t size, size_t remote_at,
                       size_t broadcast_at) {
    int i;
    CHECK(size > broadcast_at);
    for (i = 0; i < ROOM_SLOTS; i++) {
        FakeMember* m = &f->members[i];
        if (!m->peer || m->peer == from) continue;
        if (data[broadcast_at] || !memcmp(m->ip, data + remote_at, 4)) room_send(m->peer, data, size);
    }
}
static void room_message(FakeRoom* f, ENetPeer* peer, const uint8_t* data, size_t size) {
    Rd r;
    switch (data[0]) {
        case 1: room_join(f, peer, data, size); break;
        case 4: /* IdSetGameInfo */
            r.p = data;
            r.n = size;
            r.at = 1;
            r.bad = 0;
            rd_string(&r, f->last_game, sizeof f->last_game);
            f->last_game_id = (uint64_t)rd_be(&r, 4) << 32;
            f->last_game_id |= rd_be(&r, 4);
            CHECK(!r.bad);
            break;
        case 5: /* IdProxyPacket: type, domain, ip, port, domain, ip (at 9), port, protocol, broadcast (at 16) */
            snx_lock(&f->lk);
            f->last_datagram_size = size < sizeof f->last_datagram ? size : sizeof f->last_datagram;
            memcpy(f->last_datagram, data, f->last_datagram_size);
            snx_unlock(&f->lk);
            room_relay(f, peer, data, size, 9, 16);
            break;
        case 6: /* IdLdnPacket: type, ldn type, local ip, remote ip (at 6), broadcast (at 10) */
            room_relay(f, peer, data, size, 6, 10);
            break;
        default: CHECK(!"a message a member doesn't send");
    }
}
static void room_thread(void* arg) {
    FakeRoom* f = arg;
    for (;;) {
        ENetEvent ev;
        int i, stop, close_now;
        snx_lock(&f->lk);
        stop = f->stop;
        close_now = f->close_now;
        f->close_now = 0;
        snx_unlock(&f->lk);
        if (stop) break;
        if (close_now) {
            uint8_t closing = 12; /* IdCloseRoom */
            for (i = 0; i < ROOM_SLOTS; i++)
                if (f->members[i].peer) room_send(f->members[i].peer, &closing, 1);
            enet_host_flush(f->host);
        }
        while (enet_host_service(f->host, &ev, 5) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                room_message(f, ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                for (i = 0; i < ROOM_SLOTS; i++)
                    if (f->members[i].peer == ev.peer) memset(&f->members[i], 0, sizeof f->members[i]);
                room_information(f);
            }
        }
    }
    snx_lock(&f->lk);
    f->done = 1;
    snx_unlock(&f->lk);
}
static void room_start(FakeRoom* f) {
    ENetAddress a;
    memset(f, 0, sizeof *f);
    snx_lock_init(&f->lk);
    enet_address_set_host(&a, "127.0.0.1");
    a.port = 0;
    f->host = enet_host_create(&a, ROOM_SLOTS + 2, 1, 0, 0);
    CHECK(f->host != NULL);
    CHECK(enet_socket_get_address(f->host->socket, &a) == 0 && a.port);
    f->port = a.port;
    CHECK(snx_net_thread(room_thread, f) == 0);
}
static void room_stop(FakeRoom* f) {
    int done = 0;
    snx_lock(&f->lk);
    f->stop = 1;
    snx_unlock(&f->lk);
    while (!done) {
        sleep_ms(1);
        snx_lock(&f->lk);
        done = f->done;
        snx_unlock(&f->lk);
    }
    enet_host_destroy(f->host);
    snx_lock_destroy(&f->lk);
}
static int join(SnxRoom** out, const char* nickname, const char* password, int* why) {
    return snx_room_join(out, "127.0.0.1", g_room.port, nickname, password, "mk8d", 0x0100152000022000ull, "", why);
}

/* ============================================================ the messages */
static void messages(void) {
    static const uint8_t join_bytes[] = {1, 0, 0, 0, 4, 'A', 'o', 'g', 'o', 255, 255, 255, 255, 0,
                                         0, 0, 1, 0, 0, 0,   2,   'p', 'w', 0,   0,   0,   0};
    static const uint8_t ldn_bytes[] = {6, 3, 192, 168, 1, 1, 192, 168, 1, 2, 0, 0, 0, 0, 3, 'n', 'e', 't'};
    static const uint8_t datagram_bytes[] = {5,   1,    192,  168, 1, 2, 0x30, 0x39, 1, 192, 168, 1,
                                             255, 0x04, 0xD2, 4,   1, 0, 0,    0,    2, 'h', 'i'};
    uint8_t out[64];
    SnxRoomLdn p, q;
    SnxRoomDatagram d, e;
    CHECK(snx_room_pack_join(out, sizeof out, "Aogo", "pw") == sizeof join_bytes);
    CHECK(!memcmp(out, join_bytes, sizeof join_bytes));
    CHECK(snx_room_pack_join(out, 8, "Aogo", "pw") == 0);
    memset(&p, 0, sizeof p);
    p.type = SNX_ROOM_LDN_SYNC_NETWORK;
    memcpy(p.local_ip, ldn_bytes + 2, 4);
    memcpy(p.remote_ip, ldn_bytes + 6, 4);
    p.data = (const uint8_t*)"net";
    p.size = 3;
    CHECK(snx_room_pack_ldn(out, sizeof out, &p) == sizeof ldn_bytes && !memcmp(out, ldn_bytes, sizeof ldn_bytes));
    CHECK(snx_room_parse_ldn(ldn_bytes, sizeof ldn_bytes, &q) == 1);
    CHECK(q.type == 3 && !memcmp(q.local_ip, p.local_ip, 4) && !memcmp(q.remote_ip, p.remote_ip, 4) && !q.broadcast);
    CHECK(q.size == 3 && !memcmp(q.data, "net", 3));
    CHECK(snx_room_parse_ldn(ldn_bytes, sizeof ldn_bytes - 1, &q) == 0); /* cut short */
    memcpy(out, ldn_bytes, sizeof ldn_bytes);
    out[sizeof ldn_bytes] = 0;
    CHECK(snx_room_parse_ldn(out, sizeof ldn_bytes + 1, &q) == 0); /* a byte after it */
    memset(&d, 0, sizeof d);
    memcpy(d.local_ip, datagram_bytes + 2, 4);
    d.local_port = 12345;
    memcpy(d.remote_ip, datagram_bytes + 9, 4);
    d.remote_port = 1234;
    d.broadcast = 1;
    d.data = (const uint8_t*)"hi";
    d.size = 2;
    CHECK(snx_room_pack_datagram(out, sizeof out, &d) == sizeof datagram_bytes);
    CHECK(!memcmp(out, datagram_bytes, sizeof datagram_bytes));
    CHECK(snx_room_parse_datagram(datagram_bytes, sizeof datagram_bytes, &e) == 1);
    CHECK(e.local_port == 12345 && e.remote_port == 1234 && e.broadcast && e.size == 2 && !memcmp(e.data, "hi", 2));
}

/* ============================================================ joining */
static void joining(void) {
    SnxRoom *a, *b, *c;
    uint8_t ip[4];
    int why, r;
    r = join(&a, "Aogo", "secret", &why);
    CHECK(r == 0 && why == SNX_ROOM_IN && snx_room_state(a) == SNX_ROOM_IN);
    snx_room_fake_ip(a, ip);
    CHECK(ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 1 && snx_room_has_member(a, ip));
    CHECK(!strcmp(g_room.last_game, "mk8d") && g_room.last_game_id == 0x0100152000022000ull);
    /* each refusal, as the room says it */
    CHECK(join(&b, "Xuji", "wrong", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_WRONG_PASSWORD && !b);
    CHECK(join(&b, "Aogo", "secret", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_NAME_COLLISION);
    CHECK(join(&b, "Ada", "secret", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_NAME_COLLISION);
    CHECK(snx_room_why_text(SNX_ROOM_NAME_COLLISION)[0] &&
          strstr(snx_room_why_text(SNX_ROOM_NAME_COLLISION), "4 to 20"));
    r = join(&b, "Xuji", "secret", &why);
    CHECK(r == 0);
    snx_room_fake_ip(b, ip);
    CHECK(ip[3] == 2);
    /* the members each sees: the other one, by the room's information */
    for (r = 0; r < 200 && !snx_room_has_member(a, ip); r++) sleep_ms(5);
    CHECK(snx_room_has_member(a, ip));
    /* nobody at that port: given up after the connect's time out */
    CHECK(snx_room_join(&c, "127.0.0.1", 9, "Afugo", "secret", "mk8d", 1, "", &why) == -SNX_ETIMEDOUT);
    CHECK(why == SNX_ROOM_UNREACHABLE);
    snx_room_leave(b);
    snx_room_fake_ip(a, ip);
    ip[3] = 2;
    for (r = 0; r < 200 && snx_room_has_member(a, ip); r++) sleep_ms(5);
    CHECK(!snx_room_has_member(a, ip)); /* the room told the others at once */
    snx_room_leave(a);
}

/* ============================================================ ldn through the room */
static int changes_a, changes_b;
static void changed_a(void* arg) {
    (void)arg;
    __atomic_add_fetch(&changes_a, 1, __ATOMIC_SEQ_CST);
}
static void changed_b(void* arg) {
    (void)arg;
    __atomic_add_fetch(&changes_b, 1, __ATOMIC_SEQ_CST);
}
static int wait_for(int* counter, int at_least, int ms) {
    int i;
    for (i = 0; i < ms && __atomic_load_n(counter, __ATOMIC_SEQ_CST) < at_least; i++) sleep_ms(1);
    return __atomic_load_n(counter, __ATOMIC_SEQ_CST) >= at_least;
}
static void name_of(uint8_t out[SNX_LDN_USER_NAME], const char* name) {
    memset(out, 0, SNX_LDN_USER_NAME);
    strcpy((char*)out, name);
}
static void network_of(SnxLdn* host, SnxLdnNetworkInfo* n) {
    uint8_t ip[4], mask[4], name[SNX_LDN_USER_NAME];
    int i;
    memset(n, 0, sizeof *n);
    n->local_communication_id = 0x0100152000022000ull;
    n->session_id[0] = 0x1122334455667788ull;
    snx_ldn_address(host, ip, mask);
    snx_ldn_fake_mac(ip, n->bssid);
    n->ssid_length = 32;
    memset(n->ssid, 'a', 32);
    n->node_count_max = 8;
    n->node_count = 1;
    for (i = 0; i < SNX_LDN_NODES; i++) n->nodes[i].node_id = (int8_t)i;
    name_of(name, "Aogo");
    snx_ldn_node_init(host, &n->nodes[0], name, 4);
}
static void networks(void) {
    SnxRoom *ra, *rb;
    SnxLdn *a, *b;
    SnxLdnNetworkInfo net, found[4], info;
    uint8_t name[SNX_LDN_USER_NAME], updates[SNX_LDN_NODES], ip[4], mask[4];
    int why;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    CHECK(snx_ldn_open_room(&a, ra, changed_a, NULL) == 0 && snx_ldn_open_room(&b, rb, changed_b, NULL) == 0);
    snx_ldn_address(b, ip, mask);
    CHECK(ip[0] == 192 && ip[3] == 2 && mask[0] == 255 && mask[2] == 255 && mask[3] == 0);
    CHECK(snx_ldn_scan(b, NULL, found, 4, 200) == 0); /* nothing yet */
    network_of(a, &net);
    CHECK(snx_ldn_set_advertise_data(a, "hello", 5) == 0 && snx_ldn_create_network(a, &net) == 0);
    /* the station finds it and joins: node 1 at the room's address */
    CHECK(snx_ldn_scan(b, NULL, found, 4, 1000) == 1);
    CHECK(!memcmp(found[0].bssid, net.bssid, 6) && found[0].advertise_data_size == 5);
    CHECK(snx_ldn_scan(a, NULL, found, 4, 200) == 0); /* the host doesn't find itself */
    name_of(name, "Xuji");
    changes_a = changes_b = 0;
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    CHECK(wait_for(&changes_a, 1, 2000));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1 && info.node_count == 2 && updates[1] == SNX_LDN_NODE_CONNECT);
    CHECK(!strcmp((char*)info.nodes[1].user_name, "Xuji") && info.nodes[1].ipv4[0] == 2 &&
          info.nodes[1].ipv4[3] == 192);
    CHECK(snx_ldn_network_info(b, &info, NULL) == 1 && info.node_count == 2);
    /* the host's changes reach the station */
    changes_b = 0;
    CHECK(snx_ldn_set_advertise_data(a, "again", 5) == 0);
    CHECK(wait_for(&changes_b, 1, 2000));
    CHECK(snx_ldn_network_info(b, &info, NULL) == 1 && !memcmp(info.advertise_data, "again", 5));
    /* the station leaves */
    changes_a = 0;
    snx_ldn_disconnect(b);
    CHECK(!snx_ldn_connected(b) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DISCONNECTED_BY_USER);
    CHECK(wait_for(&changes_a, 1, 2000));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1 && info.node_count == 1 &&
          updates[1] == SNX_LDN_NODE_DISCONNECT);
    /* joins again, and the host destroys the network: the station is told */
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    changes_b = 0;
    snx_ldn_destroy_network(a);
    CHECK(wait_for(&changes_b, 1, 2000) && !snx_ldn_connected(b));
    CHECK(snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DESTROYED_BY_USER);
    /* no network: the connect waits for a sync that never comes */
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 300) == -SNX_ETIMEDOUT);
    /* the station's console leaves the room without a word: the host takes its node out */
    CHECK(snx_ldn_create_network(a, &net) == 0);
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    CHECK(wait_for(&changes_a, 1, 2000));
    changes_a = 0;
    snx_room_leave(rb);
    CHECK(wait_for(&changes_a, 1, 3000));
    CHECK(snx_ldn_network_info(a, &info, NULL) == 1 && info.node_count == 1 && !info.nodes[1].is_connected);
    snx_ldn_close(b); /* its goodbyes fail quietly: out of the room */
    snx_ldn_close(a);
    snx_room_leave(ra);
}

/* ============================================================ datagrams */
static SnxSockAddr to_of(const uint8_t ip[4], uint16_t port) {
    SnxSockAddr a;
    memset(&a, 0, sizeof a);
    a.family = SNX_AF_INET;
    memcpy(a.addr, ip, 4);
    a.port = (uint16_t)(port >> 8 | port << 8);
    return a;
}
static int64_t recv_wait(SnxSocket* s, void* buf, size_t size, SnxSockAddr* from, int* flags) {
    SnxTimeval tv = {2, 0};
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, sizeof tv);
    return snx_socket_recv(s, buf, size, 0, from, flags);
}
static void datagrams(void) {
    static const uint8_t any[4] = {0, 0, 0, 0}, bcast[4] = {192, 168, 1, 255};
    SnxRoom *ra, *rb;
    SnxSocket *a, *a17, *b, *b17, *c;
    SnxSockAddr addr, from;
    SnxPollEntry e;
    SnxTimeval tv = {0, 100000};
    uint8_t ip_a[4], ip_b[4], buf[64], *big;
    int why, one = 1, flags, value;
    size_t size;
    int64_t n;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    snx_room_fake_ip(ra, ip_a);
    snx_room_fake_ip(rb, ip_b);
    CHECK(snx_room_socket_open(ra, &a, SNX_AF_INET, SNX_SOCK_STREAM, 0) == -SNX_EPROTONOSUPPORT);
    CHECK(snx_room_socket_open(ra, &a, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0 && snx_room_socket(a));
    CHECK(snx_room_socket_open(ra, &a17, SNX_AF_INET, SNX_SOCK_DGRAM, SNX_IPPROTO_UDP) == 0);
    CHECK(snx_room_socket_open(rb, &b, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
    CHECK(snx_room_socket_open(rb, &b17, SNX_AF_INET, SNX_SOCK_DGRAM, SNX_IPPROTO_UDP) == 0);
    addr = to_of(any, 1234);
    CHECK(snx_socket_bind(a, &addr) == 0);
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_bind(a17, &addr) == -SNX_EADDRINUSE); /* the port is a's */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_bind(b, &addr) == -SNX_EADDRNOTAVAIL); /* another console's address */
    addr = to_of(ip_a, 1235);
    CHECK(snx_socket_bind(a17, &addr) == 0);
    CHECK(snx_socket_name(a, &addr) == 0 && addr.port == (uint16_t)(1234 >> 8 | 1234 << 8) &&
          !memcmp(addr.addr, any, 4));
    /* to a member: from the sender's address and the port its first send took; poll says so first */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b, "hello", 5, 0, &addr) == 5);
    e.socket = a;
    e.host_fd = -1;
    e.events = SNX_POLLIN;
    CHECK(snx_net_poll(&e, 1, 2000) == 1 && (e.revents & SNX_POLLIN));
    CHECK(snx_socket_readable_bytes(a, &value) == 0 && value == 5);
    n = recv_wait(a, buf, sizeof buf, &from, &flags);
    CHECK(n == 5 && !memcmp(buf, "hello", 5) && !memcmp(from.addr, ip_b, 4) && from.port && !flags);
    CHECK(snx_socket_name(b, &addr) == 0 && addr.port == from.port);
    /* the answer goes back to it */
    CHECK(snx_socket_send(a, "back", 4, 0, &from) == 4);
    n = recv_wait(b, buf, sizeof buf, &from, NULL);
    CHECK(n == 4 && !memcmp(buf, "back", 4) && !memcmp(from.addr, ip_a, 4));
    /* Eden's protocol byte: protocol 0 is IP (1), UDP is UDP (4), and each hears only its own */
    snx_lock(&g_room.lk);
    CHECK(g_room.last_datagram_size > 16 && g_room.last_datagram[15] == 1);
    snx_unlock(&g_room.lk);
    addr = to_of(ip_a, 1235);
    CHECK(snx_socket_send(b17, "udp", 3, 0, &addr) == 3);
    n = recv_wait(a17, buf, sizeof buf, NULL, NULL);
    CHECK(n == 3 && !memcmp(buf, "udp", 3));
    snx_lock(&g_room.lk);
    CHECK(g_room.last_datagram[15] == 4);
    snx_unlock(&g_room.lk);
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b17, "lost", 4, 0, &addr) == 4); /* to a's port, but a protocol-0 socket */
    snx_socket_setsockopt(a, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(snx_socket_recv(a, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN); /* the time out: nothing came */
    /* a broadcast needs SO_BROADCAST to send, and to be heard */
    addr = to_of(bcast, 1234);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == -SNX_EACCES);
    CHECK(snx_socket_setsockopt(b, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one) == 0);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == 3);
    CHECK(snx_socket_recv(a, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN); /* a hasn't SO_BROADCAST */
    CHECK(snx_socket_setsockopt(a, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one) == 0);
    size = sizeof value;
    CHECK(snx_socket_getsockopt(a, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &value, &size) == 0 && value == 1);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == 3);
    n = recv_wait(a, buf, sizeof buf, &from, NULL);
    CHECK(n == 3 && !memcmp(buf, "all", 3) && !memcmp(from.addr, ip_b, 4));
    /* a datagram bigger than the buffer: its start, with MSG_TRUNC; MSG_PEEK leaves it */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b, "0123456789", 10, 0, &addr) == 10);
    e.revents = 0;
    CHECK(snx_net_poll(&e, 1, 2000) == 1);
    CHECK(snx_socket_recv(a, buf, 4, SNX_MSG_PEEK, NULL, &flags) == 4 && (flags & SNX_MSG_TRUNC));
    CHECK(snx_socket_recv(a, buf, 4, 0, NULL, &flags) == 4 && !memcmp(buf, "0123", 4) && (flags & SNX_MSG_TRUNC));
    CHECK(snx_socket_recv(a, buf, sizeof buf, SNX_MSG_DONTWAIT, NULL, NULL) == -SNX_EAGAIN); /* the rest is gone */
    /* a datagram as big as UDP takes, through zstd, whole */
    big = malloc(SNX_ROOM_DATAGRAM_MAX + 1);
    CHECK(big != NULL);
    for (size = 0; size < SNX_ROOM_DATAGRAM_MAX; size++) big[size] = (uint8_t)(size * 7 + (size >> 9));
    CHECK(snx_socket_send(b, big, SNX_ROOM_DATAGRAM_MAX + 1, 0, &addr) == -SNX_EMSGSIZE);
    CHECK(snx_socket_send(b, big, SNX_ROOM_DATAGRAM_MAX, 0, &addr) == SNX_ROOM_DATAGRAM_MAX);
    {
        uint8_t* back = malloc(SNX_ROOM_DATAGRAM_MAX);
        CHECK(back != NULL);
        n = recv_wait(a, back, SNX_ROOM_DATAGRAM_MAX, NULL, NULL);
        CHECK(n == SNX_ROOM_DATAGRAM_MAX && !memcmp(back, big, SNX_ROOM_DATAGRAM_MAX));
        free(back);
    }
    free(big);
    /* a connected socket sends without an address; one with none to send to is told */
    CHECK(snx_room_socket_open(rb, &c, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
    CHECK(snx_socket_send(c, "x", 1, 0, NULL) == -SNX_EDESTADDRREQ);
    CHECK(snx_socket_connect(c, &addr) == 0 && snx_socket_send(c, "conn", 4, 0, NULL) == 4);
    n = recv_wait(a, buf, sizeof buf, NULL, NULL);
    CHECK(n == 4 && !memcmp(buf, "conn", 4));
    /* out of the room: a send says so; the sockets still close after */
    snx_room_leave(rb);
    CHECK(snx_socket_send(c, "late", 4, 0, NULL) == -SNX_ENETDOWN);
    snx_socket_unref(c);
    snx_socket_unref(b17);
    snx_socket_unref(b);
    snx_socket_unref(a17);
    snx_socket_unref(a);
    snx_room_leave(ra);
}

/* ============================================================ the room closing */
static void closing(void) {
    SnxRoom *ra, *rb;
    SnxLdn *a, *b;
    SnxLdnNetworkInfo net, found[2];
    uint8_t name[SNX_LDN_USER_NAME];
    int why, i;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    CHECK(snx_ldn_open_room(&a, ra, changed_a, NULL) == 0 && snx_ldn_open_room(&b, rb, changed_b, NULL) == 0);
    network_of(a, &net);
    CHECK(snx_ldn_create_network(a, &net) == 0 && snx_ldn_scan(b, NULL, found, 2, 1000) == 1);
    name_of(name, "Xuji");
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    changes_b = 0;
    snx_lock(&g_room.lk);
    g_room.close_now = 1;
    snx_unlock(&g_room.lk);
    CHECK(wait_for(&changes_b, 1, 3000));
    for (i = 0; i < 200 && snx_room_state(rb) == SNX_ROOM_IN; i++) sleep_ms(5);
    CHECK(snx_room_state(rb) == SNX_ROOM_CLOSED && snx_room_state(ra) == SNX_ROOM_CLOSED);
    CHECK(!snx_ldn_connected(b) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DESTROYED_BY_USER);
    snx_ldn_close(b);
    snx_ldn_close(a);
    snx_room_leave(rb);
    snx_room_leave(ra);
}

int main(void) {
    CHECK(snx_net_init() == 0);
    CHECK(enet_initialize() == 0);
    snx_net_set_log(quiet);
    messages();
    room_start(&g_room);
    joining();
    networks();
    datagrams();
    closing();
    room_stop(&g_room);
    enet_deinitialize();
    printf("ldn_room: %u checks passed\n", checks);
    return 0;
}
