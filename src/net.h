/* net.h - online play (port extra): two copies of the game kept in lockstep over UDP.
 *
 * Both computers run the whole program - the front end, the menus and the game - on the same inputs, one step of
 * 1/60 s at a time. Only the players' pads cross the wire (NetInput, 16 bytes a frame): the host is player 1 (pad
 * slot 0), the guest player 2 (slot 1). A step runs once both pads for it are in; the local pad is sent NET_DELAY
 * frames ahead, so on a link faster than that the game never waits. At the connection the host hands the guest
 * what decides the game besides the pads: its profile (rampage_profile.bin and its .wii.bin), its language,
 * four_players and profile_autoload. Each side hashes its game state every frame and sends the hash with its
 * pads; a different hash is a desync, which is logged and shown in the window's title.
 *
 *   RampageRE.exe --host [PORT]            wait for a player on UDP PORT (default 7777)
 *   RampageRE.exe --join HOST[:PORT]       join one (an IP address or a name)
 *   --net-delay N                          the input delay in frames (1 .. 12, default 3; the host's is used)
 */
#ifndef RAMPAGE_NET_H
#define RAMPAGE_NET_H
#include <stdint.h>

#define NET_PORT 7777
#define NET_PROTO 1
#define NET_PROFILE_MAX 8192

typedef struct {                       /* one player's pad for one frame, as both sides use it */
    uint32_t buttons;                  /* 1 << PAD_* (pad.h) */
    int8_t lx, ly, rx, ry;             /* the sticks, -127 .. 127 */
    uint8_t lt, rt;                    /* the triggers, 0 .. 255 */
    uint8_t kind;                      /* PADK_* (the button pictures) */
    uint8_t movie;                     /* 0x80 | movie serial when this side's movie has ended, else 0 */
    uint8_t typed;                     /* a key for the name entry: 'A' .. 'Z', '0' .. '9', '\r', '\b'; 0 none */
    uint8_t pad_[3];
} NetInput;

typedef struct {                       /* what each side says of itself at the connection */
    char build[48];                    /* the program's build (net_build_id): both must be the same exe */
    uint32_t vol_files, wii_files;     /* the data: the VOL's files, the Wii folder's (0: none) */
    uint32_t nmonsters, ncities;
} NetHello;

typedef struct {                       /* what the host decides for both */
    int32_t lang, four_players, profile_autoload, delay;
    uint32_t epoch;                    /* the host's clock at the start (seconds): the profile's save times */
    uint32_t prof_n, wprof_n;          /* the profile files' sizes (0: none) */
    uint8_t prof[NET_PROFILE_MAX], wprof[NET_PROFILE_MAX];
} NetSetup;

/* the connection, before the game starts: host (addr NULL) or join addr ("1.2.3.4", "1.2.3.4:7777", a name).
 * pump is called about every 15 ms while waiting (the window's messages); a non-zero return cancels. The host fills
 * *setup before, the guest gets it. 0 = connected, else -1 with the reason in err */
int net_connect(const char *addr, int port, const NetHello *mine, NetSetup *setup, int (*pump)(void *), void *ctx,
                char *err, int errcap);
int net_active(void);                  /* connected and playing */
int net_is_host(void);
int net_slot(void);                    /* this side's player: 0 host, 1 guest */
int net_delay(void);
uint32_t net_frame(void);              /* the step about to run */

/* each step: this side's pad for step net_frame() + delay goes out; then both pads for net_frame() are waited for
 * (pumping as above). 1 = in[0] (host) and in[1] (guest) are set and the frame moved on; 0 = the link is gone (the
 * game goes on alone: net_active() is 0 from now on) */
int net_step(const NetInput *local, NetInput in[2], int (*pump)(void *), void *ctx);
void net_hash(uint32_t frame, uint32_t hash);   /* this side's state after step frame */
int net_desynced(uint32_t *frame);     /* a hash differed: the first frame it did */
float net_ping_ms(void);
void net_close(void);

void net_build_id(char *out, int cap);  /* this program's own file hashed: both sides must run the same one */
double net_time(void);                 /* seconds, a monotonic clock */
void net_sleep(double seconds);
#endif
