/* net.c - online play (port extra, net.h): the lockstep link over UDP.
 *
 * Packets (little-endian, as every platform the port builds for): "RRNP", a type byte, then
 *   HELLO   guest -> host: proto u32, NetHello, the setup part wanted (u32)   (sent until the setup is in)
 *   PART    host -> guest: NetHello, part u32, parts u32, total u32, up to PART_MAX bytes of the setup blob
 *   READY   guest -> host: the setup is in, the game starts (the guest's first INPUT counts too)
 *   REJECT  either way: a reason (text)
 *   INPUT   first u32, count u8, ack u32 (the last frame of the other side's held in a row, + 1), hash_frame u32,
 *           hash u32, then count NetInputs from frame first: every frame not yet acknowledged, each time (a lost
 *           packet costs nothing but the next one's arrival)
 *   BYE     the game was closed
 * The setup blob: NetSetup's header (its fields up to prof), then prof_n bytes, then wprof_n bytes. */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
typedef int socklen_t_;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
typedef socklen_t socklen_t_;
#endif

#define MAGIC 0x504E5252u                            /* "RRNP" */
enum { P_HELLO = 1, P_PART, P_READY, P_REJECT, P_INPUT, P_BYE };
#define PART_MAX 1024
#define RING 512                                     /* frames kept; far more than delay + a stall's worth */
#define MAX_SEND 96                                  /* frames in one INPUT packet at most */
#define TIMEOUT_PLAY 20.0                            /* seconds without a packet: the link is gone (a load on the
                                                      * other side sends nothing for a while) */

static struct {
    sock_t s;
    struct sockaddr_storage peer;
    socklen_t_ peer_len;
    int active, host, delay;
    uint32_t frame;                                  /* the next step to run */
    NetInput mine[RING], theirs[RING];
    uint32_t mine_top;                               /* frames of ours made: 0 .. mine_top - 1 */
    uint32_t theirs_top;                             /* frames of theirs held in a row: 0 .. theirs_top - 1 */
    uint32_t acked;                                  /* frames of ours they hold in a row */
    uint32_t hash_mine[RING], hash_theirs[RING], hash_f_mine[RING], hash_f_theirs[RING];
    uint8_t has_mine[RING], has_theirs[RING];
    uint32_t last_hash_frame, last_hash;             /* ours, the newest, sent with every packet */
    int desync;
    uint32_t desync_frame;
    double last_recv, last_send;
    double ping_sent_at[RING];                       /* when each of our frames first went out (the ping) */
    float ping;
    int drop;                                        /* RAMPAGE_NET_DROP: percent of packets not sent (a test) */
} N = { .s = SOCK_BAD };

/* ---- the clock */
double net_time(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

void net_sleep(double seconds)
{
    if (seconds <= 0)
        return;
#ifdef _WIN32
    static int period;
    if (!period) {                                   /* 1 ms sleeps (winmm's timeBeginPeriod, loaded as needed) */
        HMODULE wm = LoadLibraryA("winmm.dll");
        typedef UINT (WINAPI *TBP)(UINT);
        TBP tbp = wm ? (TBP)(void *)GetProcAddress(wm, "timeBeginPeriod") : NULL;
        if (tbp)
            tbp(1);
        period = 1;
    }
    DWORD ms = (DWORD)(seconds * 1000);
    Sleep(ms ? ms : 0);
#else
    struct timespec ts = { (time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9) };
    nanosleep(&ts, NULL);
#endif
}

void net_build_id(char *out, int cap)
{
    char path[1024] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, path, sizeof path);
#else
    snprintf(path, sizeof path, "/proc/self/exe");
#endif
    uint64_t h = 1469598103934665603ull;
    long size = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        static unsigned char b[65536];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) {
            for (size_t i = 0; i < n; i++)
                h = (h ^ b[i]) * 1099511628211ull;
            size += (long)n;
        }
        fclose(f);
    }
    snprintf(out, (size_t)cap, "%016llX/%ld/p%d", (unsigned long long)h, size, NET_PROTO);
}

/* ---- the socket */
static int sock_open(int port)
{
#ifdef _WIN32
    static int wsa;
    if (!wsa) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return -1;
        wsa = 1;
    }
#endif
    N.s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);  /* dual stack: IPv4 and IPv6 alike */
    int v6 = N.s != SOCK_BAD;
    if (v6) {
        int off = 0;
        setsockopt(N.s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_port = htons((unsigned short)port);   /* sin6_addr all zeros: in6addr_any (which VitaSDK lacks) */
        if (bind(N.s, (struct sockaddr *)&a, sizeof a) != 0) {
            sock_close(N.s);
            N.s = SOCK_BAD;
            if (port)
                return -1;
        }
    }
    if (N.s == SOCK_BAD) {                           /* no IPv6 here: IPv4 alone */
        N.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (N.s == SOCK_BAD)
            return -1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(N.s, (struct sockaddr *)&a, sizeof a) != 0) {
            sock_close(N.s);
            N.s = SOCK_BAD;
            return -1;
        }
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(N.s, FIONBIO, &nb);
    BOOL no = FALSE;                                 /* an ICMP "port unreachable" must not break the socket */
    DWORD got;
    WSAIoctl(N.s, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &got, NULL, NULL);   /* SIO_UDP_CONNRESET */
#else
    fcntl(N.s, F_SETFL, fcntl(N.s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int buf = 1 << 20;
    setsockopt(N.s, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof buf);
    return 0;
}

/* the peer's address from "host", "host:port", "[v6]:port" */
static int resolve(const char *addr, int port, char *err, int errcap)
{
    char host[256], ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    snprintf(host, sizeof host, "%s", addr);
    char *colon = strrchr(host, ':');
    if (host[0] == '[') {                            /* [::1]:7777 */
        char *close = strchr(host, ']');
        if (close) {
            if (close[1] == ':')
                snprintf(ps, sizeof ps, "%s", close + 2);
            *close = 0;
            memmove(host, host + 1, strlen(host));
        }
    } else if (colon && strchr(host, ':') == colon) {   /* one colon: a port (more: a bare IPv6 address) */
        snprintf(ps, sizeof ps, "%s", colon + 1);
        *colon = 0;
    }
    struct addrinfo hints, *res = NULL, *r;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        snprintf(err, (size_t)errcap, "Cannot find the address %s.", addr);
        return -1;
    }
    /* our socket is IPv6 dual stack (an IPv4 address as ::ffff:a.b.c.d) or IPv4 alone */
    struct sockaddr_storage me;
    socklen_t_ ml = sizeof me;
    getsockname(N.s, (struct sockaddr *)&me, &ml);
    for (r = res; r; r = r->ai_next) {
        if (me.ss_family == AF_INET6 && r->ai_family == AF_INET) {
            struct sockaddr_in6 a;
            memset(&a, 0, sizeof a);
            a.sin6_family = AF_INET6;
            a.sin6_port = ((struct sockaddr_in *)r->ai_addr)->sin_port;
            a.sin6_addr.s6_addr[10] = a.sin6_addr.s6_addr[11] = 0xFF;
            memcpy(&a.sin6_addr.s6_addr[12], &((struct sockaddr_in *)r->ai_addr)->sin_addr, 4);
            memcpy(&N.peer, &a, sizeof a);
            N.peer_len = sizeof a;
            break;
        }
        if (r->ai_family == me.ss_family) {
            memcpy(&N.peer, r->ai_addr, r->ai_addrlen);
            N.peer_len = (socklen_t_)r->ai_addrlen;
            break;
        }
    }
    freeaddrinfo(res);
    if (!r) {
        snprintf(err, (size_t)errcap, "Cannot reach %s from here (IPv4 / IPv6).", addr);
        return -1;
    }
    return 0;
}

static void send_raw(const void *p, int n)
{
    if (N.drop && N.active && rand() % 100 < N.drop)
        return;
    sendto(N.s, (const char *)p, n, 0, (const struct sockaddr *)&N.peer, N.peer_len);
    N.last_send = net_time();
}

static int same_peer(const struct sockaddr_storage *a, socklen_t_ al)
{
    if (al != N.peer_len || a->ss_family != N.peer.ss_family)
        return 0;
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)&N.peer;
        return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, 16);
    }
    const struct sockaddr_in *x = (const void *)a, *y = (const void *)&N.peer;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}

/* one packet, or 0 bytes */
static int recv_raw(uint8_t *buf, int cap, struct sockaddr_storage *from, socklen_t_ *fl)
{
    *fl = sizeof *from;
    int n = (int)recvfrom(N.s, (char *)buf, cap, 0, (struct sockaddr *)from, fl);
    if (n < 5)
        return 0;
    uint32_t m;
    memcpy(&m, buf, 4);
    return m == MAGIC ? n : 0;
}

static int put_head(uint8_t *b, int type)
{
    uint32_t m = MAGIC;
    memcpy(b, &m, 4);
    b[4] = (uint8_t)type;
    return 5;
}

static void reject(const char *why)
{
    uint8_t b[300];
    int n = put_head(b, P_REJECT);
    int l = (int)strlen(why);
    if (l > 280) l = 280;
    memcpy(b + n, why, (size_t)l);
    send_raw(b, n + l);
}

/* the two sides' data must be the same */
static int hello_check(const NetHello *a, const NetHello *b, char *err, int errcap)
{
    if (strncmp(a->build, b->build, sizeof a->build)) {
        snprintf(err, (size_t)errcap, "The two programs are not the same build (%.*s here, %.*s there): both players "
                 "need the same RampageRE.exe.", (int)sizeof a->build, a->build, (int)sizeof b->build, b->build);
        return -1;
    }
    if (a->vol_files != b->vol_files || a->nmonsters != b->nmonsters) {
        snprintf(err, (size_t)errcap, "The game data differ (%u files here, %u there).", a->vol_files, b->vol_files);
        return -1;
    }
    if ((a->wii_files != 0) != (b->wii_files != 0) || a->ncities != b->ncities) {
        snprintf(err, (size_t)errcap, "One side has the Wii's extras (wii_data=) and the other not: both or neither "
                 "(wii_data=none in rampage.cfg leaves them out).");
        return -1;
    }
    return 0;
}

#define SETUP_HEAD offsetof(NetSetup, prof)

int net_connect(const char *addr, int port, const NetHello *mine, NetSetup *setup, int (*pump)(void *), void *ctx,
                char *err, int errcap)
{
    static uint8_t blob[SETUP_HEAD + 2 * NET_PROFILE_MAX];
    uint8_t buf[2048];
    struct sockaddr_storage from;
    socklen_t_ fl;
    N.host = addr == NULL;
    if (sock_open(N.host ? port : 0) != 0) {
        snprintf(err, (size_t)errcap, N.host ? "Cannot open UDP port %d (is another copy running?)." :
                 "Cannot open a UDP socket.", port);
        return -1;
    }
    if (!N.host && resolve(addr, port, err, errcap) != 0)
        return -1;
    double t0 = net_time(), last = 0;
    if (N.host) {                                    /* wait for a HELLO; hand out the setup's parts; READY */
        uint32_t total = (uint32_t)SETUP_HEAD + setup->prof_n + setup->wprof_n;
        memcpy(blob, setup, SETUP_HEAD);
        memcpy(blob + SETUP_HEAD, setup->prof, setup->prof_n);
        memcpy(blob + SETUP_HEAD + setup->prof_n, setup->wprof, setup->wprof_n);
        uint32_t parts = (total + PART_MAX - 1) / PART_MAX;
        int have_peer = 0;
        for (;;) {
            int n;
            while ((n = recv_raw(buf, sizeof buf, &from, &fl)) > 0) {
                if (have_peer && !same_peer(&from, fl))
                    continue;                        /* (one guest) */
                if (buf[4] == P_HELLO && n >= 5 + 4 + (int)sizeof(NetHello) + 4) {
                    uint32_t proto, want;
                    NetHello h;
                    memcpy(&proto, buf + 5, 4);
                    memcpy(&h, buf + 9, sizeof h);
                    memcpy(&want, buf + 9 + sizeof h, 4);
                    memcpy(&N.peer, &from, sizeof from);
                    N.peer_len = fl;
                    char why[400];
                    if (proto != NET_PROTO) {
                        snprintf(why, sizeof why, "The other side speaks netplay version %u, this one %u.", proto,
                                 NET_PROTO);
                        reject(why);
                        continue;
                    }
                    if (hello_check(mine, &h, why, sizeof why) != 0) {
                        reject(why);
                        printf("netplay: refused a player: %s\n", why);
                        continue;
                    }
                    if (!have_peer)
                        printf("netplay: a player is joining\n");
                    have_peer = 1;
                    if (want < parts) {
                        int o = put_head(buf, P_PART);
                        uint32_t len = total - want * PART_MAX > PART_MAX ? PART_MAX : total - want * PART_MAX;
                        memcpy(buf + o, mine, sizeof *mine); o += sizeof *mine;
                        memcpy(buf + o, &want, 4); o += 4;
                        memcpy(buf + o, &parts, 4); o += 4;
                        memcpy(buf + o, &total, 4); o += 4;
                        memcpy(buf + o, blob + want * PART_MAX, len); o += (int)len;
                        send_raw(buf, o);
                    }
                } else if (have_peer && (buf[4] == P_READY || buf[4] == P_INPUT)) {
                    N.delay = setup->delay;
                    goto connected;
                }
            }
            if (pump && pump(ctx)) {
                snprintf(err, (size_t)errcap, "Cancelled.");
                return -1;
            }
            net_sleep(0.01);
        }
    } else {                                         /* HELLO for each part in turn, then READY */
        uint32_t want = 0, parts = 1, total = 0;
        while (want < parts) {
            double now = net_time();
            if (now - last > 0.25) {                 /* (again until it answers) */
                int o = put_head(buf, P_HELLO);
                uint32_t proto = NET_PROTO;
                memcpy(buf + o, &proto, 4); o += 4;
                memcpy(buf + o, mine, sizeof *mine); o += sizeof *mine;
                memcpy(buf + o, &want, 4); o += 4;
                send_raw(buf, o);
                last = now;
            }
            if (now - t0 > 30) {
                snprintf(err, (size_t)errcap, "No answer from %s on UDP port %d after 30 seconds. Is the host waiting, "
                         "and is that port forwarded to it?", addr, port);
                return -1;
            }
            int n;
            while ((n = recv_raw(buf, sizeof buf, &from, &fl)) > 0) {
                if (!same_peer(&from, fl))
                    continue;
                if (buf[4] == P_REJECT) {
                    snprintf(err, (size_t)errcap, "The host refused: %.*s", n - 5, (const char *)buf + 5);
                    return -1;
                }
                int hl = 5 + (int)sizeof(NetHello) + 12;
                if (buf[4] == P_PART && n >= hl) {
                    NetHello h;
                    uint32_t part, ps, tot;
                    memcpy(&h, buf + 5, sizeof h);
                    memcpy(&part, buf + 5 + sizeof h, 4);
                    memcpy(&ps, buf + 9 + sizeof h, 4);
                    memcpy(&tot, buf + 13 + sizeof h, 4);
                    if (hello_check(mine, &h, err, errcap) != 0)
                        return -1;
                    if (part != want || tot > sizeof blob || tot < SETUP_HEAD ||
                        ps != (tot + PART_MAX - 1) / PART_MAX || (uint64_t)part * PART_MAX + (uint32_t)(n - hl) > tot)
                        continue;                    /* (a part that is not this one, or would not fit the blob) */
                    memcpy(blob + part * PART_MAX, buf + hl, (size_t)(n - hl));
                    parts = ps;
                    total = tot;
                    want++;
                    last = 0;                        /* the next part at once */
                }
            }
            if (pump && pump(ctx)) {
                snprintf(err, (size_t)errcap, "Cancelled.");
                return -1;
            }
            net_sleep(0.005);
        }
        memcpy(setup, blob, SETUP_HEAD);
        if (setup->prof_n > NET_PROFILE_MAX || setup->wprof_n > NET_PROFILE_MAX ||
            SETUP_HEAD + setup->prof_n + setup->wprof_n != total) {
            snprintf(err, (size_t)errcap, "The host's setup came garbled.");
            return -1;
        }
        memcpy(setup->prof, blob + SETUP_HEAD, setup->prof_n);
        memcpy(setup->wprof, blob + SETUP_HEAD + setup->prof_n, setup->wprof_n);
        N.delay = setup->delay;
        for (int k = 0; k < 4; k++) {                /* READY (its first INPUT follows anyway) */
            int o = put_head(buf, P_READY);
            send_raw(buf, o);
        }
    }
connected:
    if (N.delay < 1) N.delay = 1;
    if (N.delay > 12) N.delay = 12;
    N.active = 1;
    N.frame = 0;
    N.mine_top = N.theirs_top = 0;
    N.acked = 0;
    memset(N.mine, 0, sizeof N.mine);                /* the first delay frames: no one presses anything */
    memset(N.theirs, 0, sizeof N.theirs);
    N.mine_top = (uint32_t)N.delay;
    N.theirs_top = (uint32_t)N.delay;
    N.last_recv = net_time();
    N.last_hash_frame = 0xFFFFFFFFu;                 /* (no hash yet) */
    {
        const char *d = getenv("RAMPAGE_NET_DROP");  /* the test of a bad link: this share of our packets lost */
        N.drop = d ? atoi(d) : 0;
    }
    printf("netplay: connected as the %s, input delay %d frames\n", N.host ? "host (player 1)" : "guest (player 2)",
           N.delay);
    return 0;
}

int net_active(void) { return N.active; }
int net_is_host(void) { return N.host; }
int net_slot(void) { return N.host ? 0 : 1; }
int net_delay(void) { return N.delay; }
uint32_t net_frame(void) { return N.frame; }
float net_ping_ms(void) { return N.ping; }

int net_desynced(uint32_t *frame)
{
    if (N.desync && frame)
        *frame = N.desync_frame;
    return N.desync;
}

static void hash_compare(uint32_t f)
{
    int i = (int)(f % RING);
    if (N.has_mine[i] && N.has_theirs[i] && N.hash_f_mine[i] == f && N.hash_f_theirs[i] == f &&
        N.hash_mine[i] != N.hash_theirs[i] && !N.desync) {
        N.desync = 1;
        N.desync_frame = f;
        printf("netplay: DESYNC at frame %u (state hash %08X here, %08X there)\n", f, N.hash_mine[i], N.hash_theirs[i]);
    }
}

void net_hash(uint32_t frame, uint32_t hash)
{
    int i = (int)(frame % RING);
    N.hash_mine[i] = hash;
    N.hash_f_mine[i] = frame;
    N.has_mine[i] = 1;
    N.last_hash_frame = frame;
    N.last_hash = hash;
    hash_compare(frame);
}

static void send_inputs(void)
{
    uint8_t buf[5 + 17 + MAX_SEND * sizeof(NetInput)];
    uint32_t first = N.acked, top = N.mine_top;
    if (top - first > MAX_SEND)
        first = top - MAX_SEND;
    uint8_t count = (uint8_t)(top - first);
    int o = put_head(buf, P_INPUT);
    memcpy(buf + o, &first, 4); o += 4;
    buf[o++] = count;
    memcpy(buf + o, &N.theirs_top, 4); o += 4;
    memcpy(buf + o, &N.last_hash_frame, 4); o += 4;
    memcpy(buf + o, &N.last_hash, 4); o += 4;
    for (uint32_t f = first; f < top; f++) {
        memcpy(buf + o, &N.mine[f % RING], sizeof(NetInput));
        o += sizeof(NetInput);
    }
    send_raw(buf, o);
}

static void receive(void)
{
    uint8_t buf[4096];
    struct sockaddr_storage from;
    socklen_t_ fl;
    int n;
    while (N.active && (n = recv_raw(buf, sizeof buf, &from, &fl)) > 0) {
        if (!same_peer(&from, fl))
            continue;
        N.last_recv = net_time();
        if (buf[4] == P_BYE) {
            printf("netplay: the other player left\n");
            N.active = 0;
            return;
        }
        if (buf[4] != P_INPUT || n < 5 + 17)
            continue;
        uint32_t first, ack, hf, h;
        memcpy(&first, buf + 5, 4);
        uint8_t count = buf[9];
        memcpy(&ack, buf + 10, 4);
        memcpy(&hf, buf + 14, 4);
        memcpy(&h, buf + 18, 4);
        if (n < 22 + count * (int)sizeof(NetInput))
            continue;
        if (ack > N.acked && ack <= N.mine_top) {
            double now = net_time();
            float p = (float)((now - N.ping_sent_at[(ack - 1) % RING]) * 1000);
            N.ping = N.ping ? N.ping * 0.9f + p * 0.1f : p;
            N.acked = ack;
        }
        if (hf != 0xFFFFFFFFu) {
            int i = (int)(hf % RING);
            N.hash_theirs[i] = h;
            N.hash_f_theirs[i] = hf;
            N.has_theirs[i] = 1;
            hash_compare(hf);
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t f = first + k;
            if (f == N.theirs_top && f < N.frame + RING) {   /* the next in a row (older: had; newer: wait) */
                memcpy(&N.theirs[f % RING], buf + 22 + k * sizeof(NetInput), sizeof(NetInput));
                N.theirs_top++;
            }
        }
    }
}

int net_step(const NetInput *local, NetInput in[2], int (*pump)(void *), void *ctx)
{
    if (!N.active)
        return 0;
    uint32_t f = N.frame + (uint32_t)N.delay;        /* ours goes out delay frames ahead */
    if (f == N.mine_top) {
        N.mine[f % RING] = *local;
        N.ping_sent_at[f % RING] = net_time();
        N.mine_top++;
    }
    send_inputs();
    double wait_from = net_time(), resend = wait_from;
    int said = 0;
    for (;;) {
        receive();
        if (!N.active)
            return 0;
        if (N.theirs_top > N.frame)
            break;
        double now = net_time();
        if (now - resend > 0.03) {                   /* still waiting: theirs may be lost, or ours */
            send_inputs();
            resend = now;
        }
        if (now - wait_from > 1 && !said) {
            printf("netplay: waiting for the other player (frame %u)\n", N.frame);
            said = 1;
        }
        if (now - N.last_recv > TIMEOUT_PLAY) {
            printf("netplay: nothing from the other player for %.0f seconds: the link is gone\n", TIMEOUT_PLAY);
            net_close();
            return 0;
        }
        if (pump && pump(ctx)) {
            net_close();
            return 0;
        }
        net_sleep(0.001);
    }
    const NetInput *m = &N.mine[N.frame % RING], *t = &N.theirs[N.frame % RING];
    in[0] = N.host ? *m : *t;
    in[1] = N.host ? *t : *m;
    N.frame++;
    return 1;
}

void net_close(void)
{
    if (N.s != SOCK_BAD) {
        if (N.active) {
            uint8_t b[8];
            int n = put_head(b, P_BYE);
            for (int k = 0; k < 3; k++)
                send_raw(b, n);
        }
        sock_close(N.s);
        N.s = SOCK_BAD;
    }
    N.active = 0;
}
