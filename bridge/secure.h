/* secure.h - small pure helpers for the checks of data that arrives from the other PC (no Windows calls, so tools/test_secure.c can test them).
   Used by cod2net5.c. Audit 2026-11-22: F1 (session token), F3 (a received saved game), F4 (log injection), F13 (device names). */
#ifndef COOP_SECURE_H
#define COOP_SECURE_H

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* F4: a log line must stay ONE line of plain text: every control character and every byte above 126 becomes '?' (a packet can carry a line feed). */
static void SecSanitize(char* s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) *s = '?';
    }
}

/* F1: the session token of a guest. A packet ends with the word "T<decimal digits>" ("... 51 T1234567890123"), the hello is "H 1 T<digits>".
   Returns 0 when the packet has no token (an older version, or nothing that looks like one). */
static unsigned long long SecPktToken(const char* b) {
    const char* last = NULL;
    for (const char* p = b; *p; p++)
        if (*p == ' ' && p[1] == 'T' && p[2] >= '0' && p[2] <= '9') last = p + 2;
    if (!last) return 0;
    unsigned long long v = 0;
    for (; *last >= '0' && *last <= '9'; last++) {
        unsigned long long d = (unsigned long long)(*last - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10ull) return 0;      /* larger than a 64-bit number */
        v = v * 10ull + d;
    }
    if (*last != 0) return 0;                         /* the token must be the last word */
    return v;
}

/* F1/F14: may a packet take/keep the partner slot?
   - a known partner at the same address keeps it;
   - in host-auto mode a hello/pose may introduce the partner;
   - while the current partner is alive (stale == 0) a token, when one was issued, must match;
   - after PEER_TIMEOUT_MS of total silence (stale != 0) a restarted guest with a new token,
     or a guest on a new address, may take the slot again.
   This is a pure decision function so tools/test_secure.c can cover the reconnect cases. */
static int SecPeerAdmit(int mode, int peerKnown, int peerAuto, int sameAddr, int isPose, int isHello,
                        unsigned long long peerTok, const char* pkt, int stale) {
    int allowed = (peerKnown && sameAddr) ? 1 : 0;
    if (!allowed && peerAuto && (isPose || isHello)) allowed = 1;
    if (allowed && mode == 1 && peerKnown && peerTok != 0 && !stale && (isPose || isHello) &&
        SecPktToken(pkt) != peerTok) allowed = 0;
    return allowed;
}

/* F15: checkpoint ACK. 0 = ERR/retry, 1 = written with this CRC, 2 = receiver already has a newer
   checkpoint (OK IGN). A zero CRC in "OK 0" is accepted for older/ignored cases. */
static int SecSaveAck(const char* ack, unsigned long crc) {
    if (!ack) return 0;
    if (!strncmp(ack, "OK IGN", 6)) return 2;
    if (strncmp(ack, "OK", 2)) return 0;
    const char* p = ack + 2;
    while (*p == ' ') p++;
    unsigned long v = 0;
    int digits = 0;
    for (;; p++, digits++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned long)d;
    }
    if (!digits) return 0;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p) return 0;
    return (v == 0 || v == crc) ? 1 : 0;
}

/* Stage 1 of the world synchronisation: a world id names ONE actor on both PCs.
   Form: "<key>#<ordinal>", e.g. "enemy_sniper#0", "flak88#0", "halftrack_obj2#1".
   - key      = 1..31 characters of [A-Za-z0-9_-] (script_noteworthy/targetname, alias-normalised by the mod);
   - ordinal  = 0..99999, how many actors with that key this PC had seen before it, no leading zeros.
   Anything else is refused, so a packet cannot smuggle a line feed or a long string into the log or the
   script string table. At most 40 characters in total. */
static int SecWorldId(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n] && s[n] != '#') {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        if (++n > 36) return 0;      /* key length: an axis class name reaches 33 (actor_axis_snow_wehr_officer_mp40) */
    }
    if (n == 0 || s[n] != '#') return 0;
    const char* d = s + n + 1;
    if (*d < '0' || *d > '9') return 0;
    if (*d == '0' && d[1] >= '0' && d[1] <= '9') return 0;      /* no leading zeros: one name has one spelling */
    int digits = 0;
    long v = 0;
    for (; *d >= '0' && *d <= '9'; d++, digits++) {
        if (digits >= 5) return 0;
        v = v * 10 + (*d - '0');
    }
    return *d == 0 && v <= 99999 && n + 1 + (size_t)digits <= 40;
}

/* "WD <world id> <seq>": a death that names the actor instead of a position. Used by the host (a
   kill claim from the guest and the authoritative death it sends back), so the parser lives here
   and tools/test_secure.c covers it. Returns 1 and fills wid/seq on success. */
static int SecWorldIdToken(const char* p, char* wid, size_t cap, const char** end) {
    if (!p || !wid || cap < 2) return 0;
    size_t n = 0;
    while (*p && *p != ' ') {
        if (n + 1 >= cap) return 0;
        wid[n++] = *p++;
    }
    wid[n] = 0;
    if (*p != ' ' || !SecWorldId(wid)) return 0;
    if (end) *end = p;
    return 1;
}

/* a decimal value in [lo, hi], followed by the end of the packet or by a space. A leading '-' is
   allowed: positions are negative half of the time; the range check rejects a negative where it
   makes no sense (a sequence number, health, damage). */
static int SecWorldNumber(const char** pp, long lo, long hi, long* out) {
    const char* p = *pp;
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    long long v = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        v = v * 10 + (*p - '0');
        if (v > 2147483647LL) return 0;
    }
    if (!digits || (*p && *p != ' ')) return 0;
    if (neg) v = -v;
    if (v < (long long)lo || v > (long long)hi) return 0;
    if (out) *out = (long)v;
    *pp = p;
    return 1;
}

static int SecWorldPkt(const char* b, char* wid, size_t cap, long* seq) {
    if (!b || strncmp(b, "WD ", 3)) return 0;
    const char* p = b + 3;
    if (!SecWorldIdToken(p, wid, cap, &p)) return 0;
    if (!SecWorldNumber(&p, 0, 2147483647L, seq)) return 0;
    return *p == 0;
}

/* "WH <world id> <health> <seq>" (host -> guest: the authoritative health of one actor) and
   "HD <world id> <damage> <seq>" (guest -> host: damage this PC did to that actor). Same shape as
   WD, one small number in the middle, so one parser covers both and the tests are shared. */
static int SecWorldStatePkt(const char* b, const char* tag, char* wid, size_t cap, long* val, long* seq) {
    if (!b || !tag || !tag[0] || !tag[1] || !wid) return 0;
    if (strncmp(b, tag, 2) || b[2] != ' ') return 0;
    const char* p = b + 3;
    if (!SecWorldIdToken(p, wid, cap, &p)) return 0;
    if (!SecWorldNumber(&p, 0, 1000, val)) return 0;                 /* health and damage are small numbers */
    if (!SecWorldNumber(&p, 0, 2147483647L, seq)) return 0;
    return *p == 0;
}

/* "WP <world id> <x> <y> <z> <yaw>": where the host's actor stands, in whole units. This is the coarse body sync: the guest does not
   interpolate, it teleports its copy only when it is far away from that point (the engine refuses `.origin` on an AI and points at
   `teleport`, which is exactly the function the game's own scripts use to reposition characters). */
static int SecWorldPosPkt(const char* b, char* wid, size_t cap, long* x, long* y, long* z, long* yaw) {
    if (!b || strncmp(b, "WP ", 3) || !wid) return 0;
    const char* p = b + 3;
    if (!SecWorldIdToken(p, wid, cap, &p)) return 0;
    if (!SecWorldNumber(&p, -100000, 100000, x)) return 0;
    if (!SecWorldNumber(&p, -100000, 100000, y)) return 0;
    if (!SecWorldNumber(&p, -100000, 100000, z)) return 0;
    if (!SecWorldNumber(&p, -360, 360, yaw)) return 0;
    return *p == 0;
}

/* the token is a secret of the session: a log line that ends in " T<digits>" is written with the digits replaced by '*' */
static void SecHideToken(char* s) {
    char* last = NULL;
    for (char* p = s; *p; p++)
        if (*p == ' ' && p[1] == 'T' && p[2] >= '0' && p[2] <= '9') last = p + 2;
    if (!last) return;
    char* q = last; while (*q >= '0' && *q <= '9') q++;
    if (*q != 0) return;
    for (; last < q; last++) *last = '*';
}

/* F13: names that Windows treats as devices, also with an extension (CON.svg is the console). The part before the first '-' counts. */
static int SecReservedName(const char* name) {
    char t[16]; size_t n = 0;
    for (; name[n] && name[n] != '-' && name[n] != '.' && n < sizeof t - 1; n++) t[n] = (char)toupper((unsigned char)name[n]);
    t[n] = 0;
    if (name[n] && name[n] != '-' && name[n] != '.') return 0;   /* a long name is not a device name */
    if (!strcmp(t, "CON") || !strcmp(t, "PRN") || !strcmp(t, "AUX") || !strcmp(t, "NUL") || !strcmp(t, "CLOCK$")) return 1;
    if ((!strncmp(t, "COM", 3) || !strncmp(t, "LPT", 3)) && t[3] >= '0' && t[3] <= '9' && t[4] == 0) return 1;
    return 0;
}

/* F3: what every saved game of this game looks like (measured on 232 real files of 39 missions): the size is about 2.0-2.7 MB, the first 32-bit number
   is the save version 153, and at offset 0x20 stands the NAME OF THE MISSION as a zero-terminated word of letters, digits and "_". A file that does not
   look like that is not written into the save folder. This is a plausibility check, not a proof that the file is sound. */
static int SecSvgOk(const unsigned char* b, long sz) {
    if (sz < 1000000L || sz > 12L * 1024 * 1024) return 0;
    if (b[0] != 153 || b[1] != 0 || b[2] != 0 || b[3] != 0) return 0;
    int n = 0;
    while (n < 31 && ((b[0x20 + n] >= 'a' && b[0x20 + n] <= 'z') || (b[0x20 + n] >= '0' && b[0x20 + n] <= '9') || b[0x20 + n] == '_')) n++;
    if (n < 3) return 0;
    return b[0x20 + n] == 0;
}

/* F16: the mission name embedded in a saved game. Used to cross-check the file name the partner
   sent (moscow-1.svg must contain mission "moscow"), so a file cannot silently claim another map. */
static int SecSvgMission(const unsigned char* b, long sz, char* out, size_t cap) {
    if (!SecSvgOk(b, sz)) return 0;
    size_t n = 0;
    while (n < 31 && b[0x20 + n]) { if (out && n + 1 < cap) out[n] = (char)b[0x20 + n]; n++; }
    if (out && cap) out[n < cap ? n : cap - 1] = 0;
    return 1;
}

/* F16: "moscow" matches "moscow", "moscow-1" and "moscow-levelend", but not "moscow2". */
static int SecMapMatches(const char* header_map, const char* file_map) {
    size_t n = header_map ? strlen(header_map) : 0;
    if (!n || !file_map) return 0;
    if (_strnicmp(header_map, file_map, n)) return 0;
    return file_map[n] == 0 || file_map[n] == '-';
}

#endif

