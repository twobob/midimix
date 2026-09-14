#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t tick; uint32_t off, len; } Ev;
typedef struct { Ev *ev; size_t n; } Track;
typedef struct {
    unsigned char *buf; size_t size;
    int division, ntracks;
    Track *tracks;
    unsigned chans;
} Midi;

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "midimix: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(1);
}

static uint32_t be(const unsigned char *p, int n) {
    uint32_t v = 0;
    while (n--) v = v << 8 | *p++;
    return v;
}

static int vlq(const unsigned char *buf, size_t *pos, size_t end, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= end) return 0;
        unsigned char c = buf[(*pos)++];
        v = v << 7 | (c & 0x7f);
        if (!(c & 0x80)) { *out = v; return 1; }
    }
    return 0;
}

static void push(Track *t, size_t *cap, Ev e) {
    if (t->n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        t->ev = realloc(t->ev, *cap * sizeof *t->ev);
        if (!t->ev) die("out of memory%s", "");
    }
    t->ev[t->n++] = e;
}

#define STATUS_SHIFT 24

static void load(const char *path, Midi *m) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    m->buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!m->buf || fread(m->buf, 1, (size_t)sz, f) != (size_t)sz) die("cannot read %s", path);
    fclose(f);
    m->size = (size_t)sz;

    size_t p = 0;

    while (p + 14 <= m->size && memcmp(m->buf + p, "MThd", 4)) p++;
    if (p + 14 > m->size) die("%s is not a MIDI file", path);
    uint32_t hlen = be(m->buf + p + 4, 4);
    int ntr = (int)be(m->buf + p + 10, 2);
    int div = (int)be(m->buf + p + 12, 2);
    if (div & 0x8000) die("%s uses an SMPTE division, not supported", path);
    m->division = div;
    p += 8 + hlen;

    m->tracks = calloc((size_t)ntr + 1, sizeof *m->tracks);
    m->ntracks = 0;
    m->chans = 0;
    while (m->ntracks < ntr && p + 8 <= m->size) {
        uint32_t clen = be(m->buf + p + 4, 4);
        size_t start = p + 8, end = start + clen;
        if (end > m->size) end = m->size;
        if (memcmp(m->buf + p, "MTrk", 4)) { p = end; continue; }
        Track *t = &m->tracks[m->ntracks++];
        size_t cap = 0, q = start;
        uint64_t tick = 0;
        unsigned char running = 0;
        while (q < end) {
            uint32_t dt, len;
            if (!vlq(m->buf, &q, end, &dt) || q >= end) break;
            tick += dt;
            unsigned char s = m->buf[q];
            Ev e = { tick, 0, 0 };
            if (s == 0xff) {
                if (q + 2 > end) break;
                size_t r = q + 2;
                if (!vlq(m->buf, &r, end, &len) || r + len > end) break;
                e.off = (uint32_t)q; e.len = (uint32_t)(r + len - q);
                q = r + len;
                running = 0;
                if (m->buf[e.off + 1] == 0x2f) { push(t, &cap, e); break; }
            } else if (s == 0xf0 || s == 0xf7) {
                size_t r = q + 1;
                if (!vlq(m->buf, &r, end, &len) || r + len > end) break;
                e.off = (uint32_t)q; e.len = (uint32_t)(r + len - q);
                q = r + len;
                running = 0;
            } else {
                if (s & 0x80) { running = s; q++; }
                if (!running) break;
                int hi = running >> 4;
                uint32_t nd = (hi == 0xc || hi == 0xd) ? 1 : 2;
                if (q + nd > end) break;
                e.off = (uint32_t)q; e.len = nd | (uint32_t)running << STATUS_SHIFT;
                q += nd;
                m->chans |= 1u << (running & 15);
            }
            push(t, &cap, e);
        }
        p = end;
    }
}

static void put_vlq(FILE *f, uint64_t v) {
    unsigned char b[10];
    int n = 0;
    b[n++] = v & 0x7f;
    while (v >>= 7) b[n++] = (unsigned char)(0x80 | (v & 0x7f));
    while (n) fputc(b[--n], f);
}

static void put_be(FILE *f, uint32_t v, int n) {
    while (n--) fputc((v >> (8 * n)) & 0xff, f);
}

static void write_tracks(FILE *f, const Midi *m, int mul, int divd, const int *chmap,
                         int drop_timing) {
    for (int i = 0; i < m->ntracks; i++) {
        const Track *t = &m->tracks[i];
        fwrite("MTrk", 1, 4, f);
        long lenpos = ftell(f);
        put_be(f, 0, 4);
        long body = ftell(f);
        uint64_t last = 0;
        int ended = 0;
        for (size_t j = 0; j < t->n; j++) {
            const Ev *e = &t->ev[j];
            if (drop_timing && !(e->len >> STATUS_SHIFT) && m->buf[e->off] == 0xff
                && (m->buf[e->off + 1] == 0x51 || m->buf[e->off + 1] == 0x58)) continue;
            uint64_t tick = (e->tick * (uint64_t)mul + (uint64_t)divd / 2) / (uint64_t)divd;
            if (tick < last) tick = last;
            put_vlq(f, tick - last);
            last = tick;
            uint32_t status = e->len >> STATUS_SHIFT;
            if (status) {
                fputc((int)((status & 0xf0) | (unsigned)chmap[status & 15]), f);
                fwrite(m->buf + e->off, 1, e->len & 0xffffff, f);
            } else {
                fwrite(m->buf + e->off, 1, e->len, f);
                if (m->buf[e->off] == 0xff && m->buf[e->off + 1] == 0x2f) ended = 1;
            }
        }
        if (!ended) { put_vlq(f, 0); fputc(0xff, f); fputc(0x2f, f); fputc(0, f); }
        long endp = ftell(f);
        fseek(f, lenpos, SEEK_SET);
        put_be(f, (uint32_t)(endp - body), 4);
        fseek(f, endp, SEEK_SET);
    }
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

int main(int argc, char **argv) {
    const char *out = "mix.mid", *in[2];
    int nin = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] != '-' && nin < 2) in[nin++] = argv[i];
        else nin = 3;
    }
    if (nin != 2) { fprintf(stderr, "usage: midimix [-o out.mid] a.mid b.mid\n"); return 2; }

    Midi a, b;
    load(in[0], &a);
    load(in[1], &b);

    int da = a.division ? a.division : 480, db = b.division ? b.division : 480;
    long l = (long)da / gcd(da, db) * db;
    int div = l <= 32767 ? (int)l : (da > db ? da : db);

    int mapa[16], mapb[16];
    unsigned used = a.chans;
    for (int c = 0; c < 16; c++) mapa[c] = mapb[c] = c;
    for (int c = 0; c < 16; c++) {
        if (c == 9 || !(b.chans & (1u << c))) continue;
        if (!(a.chans & (1u << c))) { used |= 1u << c; continue; }
        for (int d = 0; d < 16; d++) {
            if (d == 9 || (used | b.chans) & (1u << d)) continue;
            mapb[c] = d;
            used |= 1u << d;
            break;
        }
        if (mapb[c] == c) fprintf(stderr, "midimix: no free channel for %s channel %d, left shared\n", in[1], c + 1);
    }

    FILE *f = fopen(out, "wb");
    if (!f) die("cannot write %s", out);
    fwrite("MThd", 1, 4, f);
    put_be(f, 6, 4); put_be(f, 1, 2);
    put_be(f, (uint32_t)(a.ntracks + b.ntracks), 2);
    put_be(f, (uint32_t)div, 2);
    write_tracks(f, &a, div, da, mapa, 0);
    write_tracks(f, &b, div, db, mapb, 1);
    fclose(f);
    fprintf(stderr, "midimix: %d + %d tracks -> %s\n", a.ntracks, b.ntracks, out);
    return 0;
}
