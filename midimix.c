#define _CRT_SECURE_NO_WARNINGS
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SEP '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#define SEP '/'
#endif

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
#define MAX_PORTS 128

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

/* Tempo (FF 51) and time signature (FF 58): the tempo map. */
static int is_timing(const Midi *m, const Ev *e) {
    return !(e->len >> STATUS_SHIFT) && m->buf[e->off] == 0xff
        && (m->buf[e->off + 1] == 0x51 || m->buf[e->off + 1] == 0x58);
}

static int has_timing(const Midi *m, const Track *t) {
    for (size_t j = 0; j < t->n; j++) if (is_timing(m, &t->ev[j])) return 1;
    return 0;
}

/* A global tempo track: carries the tempo map and no channel events. */
static int is_tempo_track(const Midi *m, const Track *t) {
    for (size_t j = 0; j < t->n; j++) if (t->ev[j].len >> STATUS_SHIFT) return 0;
    return has_timing(m, t);
}

/* Do two tracks carry the same tempo map once rescaled to the output division? */
static int same_timing(const Midi *a, const Track *ta, int da,
                       const Midi *b, const Track *tb, int db, int div) {
    size_t i = 0, j = 0;
    for (;;) {
        while (i < ta->n && !is_timing(a, &ta->ev[i])) i++;
        while (j < tb->n && !is_timing(b, &tb->ev[j])) j++;
        if (i == ta->n || j == tb->n) return i == ta->n && j == tb->n;
        const Ev *x = &ta->ev[i++], *y = &tb->ev[j++];
        if ((x->tick * (uint64_t)div + (uint64_t)da / 2) / (uint64_t)da
            != (y->tick * (uint64_t)div + (uint64_t)db / 2) / (uint64_t)db
            || x->len != y->len || memcmp(a->buf + x->off, b->buf + y->off, x->len)) return 0;
    }
}

/* One track holding only t's tempo map. */
static void write_tempo_track(FILE *f, const Midi *m, const Track *t, int mul, int divd, int port) {
    fwrite("MTrk", 1, 4, f);
    long lenpos = ftell(f);
    put_be(f, 0, 4);
    long body = ftell(f);
    uint64_t last = 0;
    if (port >= 0) { put_vlq(f, 0); fputc(0xff, f); fputc(0x21, f); fputc(1, f); fputc(port, f); }
    for (size_t j = 0; j < t->n; j++) {
        const Ev *e = &t->ev[j];
        if (!is_timing(m, e)) continue;
        uint64_t tick = (e->tick * (uint64_t)mul + (uint64_t)divd / 2) / (uint64_t)divd;
        if (tick < last) tick = last;
        put_vlq(f, tick - last);
        last = tick;
        fwrite(m->buf + e->off, 1, e->len, f);
    }
    put_vlq(f, 0); fputc(0xff, f); fputc(0x2f, f); fputc(0, f);
    long endp = ftell(f);
    fseek(f, lenpos, SEEK_SET);
    put_be(f, (uint32_t)(endp - body), 4);
    fseek(f, endp, SEEK_SET);
}

/* port < 0 writes no port events; otherwise each track opens with FF 21 (MIDI port)
   and the input's own port events are dropped. */
static void write_tracks(FILE *f, const Midi *m, int mul, int divd, const int *chmap,
                         int drop_timing, int port) {
    for (int i = 0; i < m->ntracks; i++) {
        const Track *t = &m->tracks[i];
        fwrite("MTrk", 1, 4, f);
        long lenpos = ftell(f);
        put_be(f, 0, 4);
        long body = ftell(f);
        uint64_t last = 0;
        int ended = 0;
        if (port >= 0) { put_vlq(f, 0); fputc(0xff, f); fputc(0x21, f); fputc(1, f); fputc(port, f); }
        for (size_t j = 0; j < t->n; j++) {
            const Ev *e = &t->ev[j];
            int meta = !(e->len >> STATUS_SHIFT) && m->buf[e->off] == 0xff;
            if (drop_timing && is_timing(m, e)) continue;
            if (port >= 0 && meta && m->buf[e->off + 1] == 0x21) continue;
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

static int is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}

static int is_midi_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char ext[6];
    size_t n = strlen(dot);
    if (n > 5) return 0;
    for (size_t i = 0; i <= n; i++) ext[i] = (char)(dot[i] >= 'A' && dot[i] <= 'Z' ? dot[i] + 32 : dot[i]);
    return !strcmp(ext, ".mid") || !strcmp(ext, ".midi");
}

static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir);
    char *s = malloc(n + strlen(name) + 2);
    if (!s) die("out of memory%s", "");
    strcpy(s, dir);
    if (n && dir[n - 1] != '/' && dir[n - 1] != '\\') s[n++] = SEP;
    strcpy(s + n, name);
    return s;
}

static int by_name(const void *x, const void *y) {
    return strcmp(*(char *const *)x, *(char *const *)y);
}

/* Absolute path, or NULL. On Linux the file must exist. */
static char *full_path(const char *p) {
#ifdef _WIN32
    return _fullpath(NULL, p, 0);
#else
    return realpath(p, NULL);
#endif
}

/* True when path names the file skip (an absolute path, or NULL for none). */
static int is_output(const char *path, const char *skip) {
    if (!skip) return 0;
    char *full = full_path(path);
    if (!full) return 0;
#ifdef _WIN32
    int same = !_stricmp(full, skip);
#else
    int same = !strcmp(full, skip);
#endif
    free(full);
    return same;
}

/* Every .mid/.midi file directly inside dir except skip and own, sorted by name. */
static char **list_midi(const char *dir, const char *skip, const char *own, int *count) {
    char **v = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char *pat = join(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) die("cannot list %s", dir);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !is_midi_name(fd.cFileName)) continue;
        char *p = join(dir, fd.cFileName);
        if (is_output(p, skip)) { fprintf(stderr, "midimix: skipping %s, the output\n", p); free(p); continue; }
        if (is_output(p, own)) { fprintf(stderr, "midimix: skipping %s, the folder's own merge\n", p); free(p); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof *v); }
        if (!v) die("out of memory%s", "");
        v[n++] = p;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) die("cannot list %s", dir);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!is_midi_name(de->d_name)) continue;
        char *p = join(dir, de->d_name);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) { free(p); continue; }
        if (is_output(p, skip)) { fprintf(stderr, "midimix: skipping %s, the output\n", p); free(p); continue; }
        if (is_output(p, own)) { fprintf(stderr, "midimix: skipping %s, the folder's own merge\n", p); free(p); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof *v); }
        if (!v) die("out of memory%s", "");
        v[n++] = p;
    }
    closedir(d);
#endif
    if (n) qsort(v, (size_t)n, sizeof *v, by_name);
    *count = n;
    return v;
}

/* The folder's own name plus .mid, inside the folder; NULL for a root folder. */
static char *folder_output(const char *dir) {
    char *full = full_path(dir);
    if (!full) die("cannot resolve %s", dir);
    size_t n = strlen(full);
    while (n && (full[n - 1] == '/' || full[n - 1] == '\\')) n--;
    if (!n || full[n - 1] == ':') { free(full); return NULL; }
    full[n] = 0;
    const char *name = full + n;
    while (name > full && name[-1] != '/' && name[-1] != '\\') name--;
    char *out = malloc(2 * n + 6);
    if (!out) die("out of memory%s", "");
    sprintf(out, "%s%c%s.mid", full, SEP, name);
    free(full);
    return out;
}

/* Every subfolder directly inside dir, sorted by name. Hidden folders and links are not followed. */
static char **list_dirs(const char *dir, int *count) {
    char **v = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char *pat = join(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) die("cannot list %s", dir);
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || fd.cFileName[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof *v); }
        if (!v) die("out of memory%s", "");
        v[n++] = join(dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) die("cannot list %s", dir);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *p = join(dir, de->d_name);
        struct stat st;
        if (lstat(p, &st) || !S_ISDIR(st.st_mode)) { free(p); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, (size_t)cap * sizeof *v); }
        if (!v) die("out of memory%s", "");
        v[n++] = p;
    }
    closedir(d);
#endif
    if (n) qsort(v, (size_t)n, sizeof *v, by_name);
    *count = n;
    return v;
}

static void mix(char **in, int nin, const char *out, int tempo_merge);

/* Mix dir into its own <dir>.mid, then do the same for every subfolder. */
static void walk(const char *dir, int tempo_merge, int *made) {
    char *own = folder_output(dir);
    if (own) {
        char *skip = full_path(own);
        int n;
        char **v = list_midi(dir, NULL, skip, &n);
        if (n) {
            for (int i = 0; i < n; i++) fprintf(stderr, "midimix: %s\n", v[i]);
            mix(v, n, own, tempo_merge);
            (*made)++;
        }
        for (int i = 0; i < n; i++) free(v[i]);
        free(v);
        free(skip);
        free(own);
    } else {
        fprintf(stderr, "midimix: %s is a root folder and gets no mix of its own\n", dir);
    }
    int nd;
    char **sub = list_dirs(dir, &nd);
    for (int i = 0; i < nd; i++) { walk(sub[i], tempo_merge, made); free(sub[i]); }
    free(sub);
}

static void usage(void) {
    fprintf(stderr, "usage: midimix [-o out.mid] [--no-global-tempo-merge] a.mid b.mid [more.mid ...]\n"
                    "       midimix [-o out.mid] [--no-global-tempo-merge] folder\n"
                    "       midimix -R [--no-global-tempo-merge] folder\n"
                    "       --no-suno-fudge is an alias of --no-global-tempo-merge\n"
                    "       -R, --recurse mixes every folder under folder into its own <name>.mid\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *out = NULL;
    char **in = calloc((size_t)argc, sizeof *in);
    int nin = 0, tempo_merge = 1, recurse = 0;
    if (!in) die("out of memory%s", "");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--no-global-tempo-merge") || !strcmp(argv[i], "--no-suno-fudge")) tempo_merge = 0;
        else if (!strcmp(argv[i], "-R") || !strcmp(argv[i], "--recurse")) recurse = 1;
        else if (argv[i][0] != '-') in[nin++] = argv[i];
        else usage();
    }
    if (recurse) {
        if (nin != 1 || !is_dir(in[0])) usage();
        if (out) die("-o cannot be used with --recurse: each folder gets its own <name>.mid%s", "");
        int made = 0;
        walk(in[0], tempo_merge, &made);
        if (!made) die("no .mid files under %s", in[0]);
        fprintf(stderr, "midimix: %d folder%s mixed\n", made, made == 1 ? "" : "s");
        return 0;
    }
    if (nin == 1 && is_dir(in[0])) {
        const char *dir = in[0];
        char *own = folder_output(dir);
        if (!out) out = own;
        if (!out) die("%s is a root folder; name the output with -o", dir);
        in = list_midi(dir, full_path(out), own ? full_path(own) : NULL, &nin);
        if (!nin) die("no .mid files in %s", dir);
        for (int i = 0; i < nin; i++) fprintf(stderr, "midimix: %s\n", in[i]);
    } else if (nin < 2) {
        usage();
    }
    if (!out) out = "mix.mid";
    mix(in, nin, out, tempo_merge);
    return 0;
}

static void mix(char **in, int nin, const char *out, int tempo_merge) {
    Midi *m = calloc((size_t)nin, sizeof *m);
    int *dv = calloc((size_t)nin, sizeof *dv);
    int (*map)[16] = calloc((size_t)nin, sizeof *map);
    if (!m || !dv || !map) die("out of memory%s", "");
    long total = 0;
    for (int i = 0; i < nin; i++) {
        load(in[i], &m[i]);
        dv[i] = m[i].division ? m[i].division : 480;
        total += m[i].ntracks;
    }

    /* Common division: the LCM if it fits, else the largest input's. */
    long l = dv[0];
    int maxd = dv[0];
    for (int i = 1; i < nin; i++) {
        if (dv[i] > maxd) maxd = dv[i];
        if (l <= 32767) l = l / gcd((int)l, dv[i]) * dv[i];
    }
    int div = l <= 32767 ? (int)l : maxd;

    /* Global tempo merge: when the output's first track is not a tempo track, and every track
       that carries a tempo map carries the same one, that map goes into a new first track
       and is dropped from every other track. */
    const Midi *tm = NULL;
    const Track *tt = NULL;
    int td = 0, hoist = 0;
    if (tempo_merge && m[0].ntracks && !is_tempo_track(&m[0], &m[0].tracks[0])) {
        int carriers = 0, agree = 1;
        for (int i = 0; i < nin; i++)
            for (int k = 0; k < m[i].ntracks; k++) {
                const Track *t = &m[i].tracks[k];
                if (!has_timing(&m[i], t)) continue;
                carriers++;
                if (!tt) { tm = &m[i]; tt = t; td = dv[i]; }
                else if (!same_timing(tm, tt, td, &m[i], t, dv[i], div)) agree = 0;
            }
        hoist = carriers > 0 && agree;
        if (hoist)
            fprintf(stderr, "midimix: tempo map shared by all %d tracks that carry one, merged into a global tempo track\n", carriers);
        else if (carriers)
            fprintf(stderr, "midimix: tracks carry different tempo maps; no global tempo track, the first file's map is kept\n");
    } else if (m[0].ntracks && is_tempo_track(&m[0], &m[0].tracks[0])) {
        /* The first file already opens with a tempo track: it stays the global map. */
        int differ = 0;
        for (int i = 0; i < nin; i++)
            for (int k = i ? 0 : 1; k < m[i].ntracks; k++) {
                const Track *t = &m[i].tracks[k];
                if (has_timing(&m[i], t) && !same_timing(&m[0], &m[0].tracks[0], dv[0], &m[i], t, dv[i], div)) {
                    if (!differ) fprintf(stderr, "midimix: warning: tempo maps differ from the first file's tempo track:\n");
                    fprintf(stderr, "midimix:   %s track %d\n", in[i], k + 1);
                    differ++;
                }
            }
        fprintf(stderr, "midimix: %s opens with a tempo track; kept as the global tempo map, every later file's tempo dropped", in[0]);
        if (differ) fprintf(stderr, " (%d track%s differed)", differ, differ == 1 ? "" : "s");
        fprintf(stderr, "\n");
    }
    total += hoist;
    if (total > 65535) die("too many tracks for one file%s", "");

    /* Each file goes whole onto the first MIDI port it fits on. On that port a channel an
       earlier file already uses moves to a free one; channel 10 (drums) never moves. */
    unsigned used[MAX_PORTS] = { 0 };
    int *port = calloc((size_t)nin, sizeof *port);
    int nports = 1;
    if (!port) die("out of memory%s", "");
    for (int i = 0; i < nin; i++) {
        int p = 0;
        for (; p < MAX_PORTS; p++) {
            int clash = 0, room = 0;
            for (int c = 0; c < 16; c++) {
                if (c == 9) continue;
                if (m[i].chans & used[p] & (1u << c)) clash++;
                if (!((used[p] | m[i].chans) & (1u << c))) room++;
            }
            if (clash <= room) break;
        }
        if (p == MAX_PORTS) die("%s does not fit in 128 ports of 16 channels", in[i]);
        port[i] = p;
        if (p + 1 > nports) nports = p + 1;
        unsigned prev = used[p];
        for (int c = 0; c < 16; c++) map[i][c] = c;
        for (int c = 0; c < 16; c++) {
            if (!(m[i].chans & (1u << c))) continue;
            if (c == 9 || !(prev & (1u << c))) { used[p] |= 1u << c; continue; }
            for (int d = 0; d < 16; d++) {
                if (d == 9 || (used[p] | m[i].chans) & (1u << d)) continue;
                map[i][c] = d;
                used[p] |= 1u << d;
                break;
            }
        }
    }
    if (nports > 1)
        for (int i = 0; i < nin; i++) fprintf(stderr, "midimix: port %d: %s\n", port[i] + 1, in[i]);

    FILE *f = fopen(out, "wb");
    if (!f) die("cannot write %s", out);
    fwrite("MThd", 1, 4, f);
    put_be(f, 6, 4); put_be(f, 1, 2);
    put_be(f, (uint32_t)total, 2);
    put_be(f, (uint32_t)div, 2);
    if (hoist) write_tempo_track(f, tm, tt, div, td, nports > 1 ? 0 : -1);
    for (int i = 0; i < nin; i++)
        write_tracks(f, &m[i], div, dv[i], map[i], hoist || i > 0, nports > 1 ? port[i] : -1);
    fclose(f);
    fprintf(stderr, "midimix: ");
    for (int i = 0; i < nin; i++) fprintf(stderr, "%s%d", i ? " + " : "", m[i].ntracks);
    fprintf(stderr, " tracks");
    if (hoist) fprintf(stderr, " + 1 tempo track");
    if (nports > 1) fprintf(stderr, " on %d ports", nports);
    fprintf(stderr, " -> %s\n", out);
    for (int i = 0; i < nin; i++) {
        for (int k = 0; k < m[i].ntracks; k++) free(m[i].tracks[k].ev);
        free(m[i].tracks);
        free(m[i].buf);
    }
    free(m); free(dv); free(map); free(port);
}
