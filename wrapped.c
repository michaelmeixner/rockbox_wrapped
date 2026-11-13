/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Listening Wrapped - a year-in-review built from the playback log
 *
 * Copyright (C) 2026 Michael Meixner
 *
 * Reads the playback log(s) written by Settings > Playback Settings >
 * Logging, keeps a permanent copy in /.rockbox/wrapped_history.log (so
 * history survives the LastFm Scrobbler deleting the log) and shows your
 * top tracks, artists, albums and listening habits as a set of slides.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "plugin.h"

#define HISTORY_FILE   ROCKBOX_DIR "/wrapped_history.log"
#define SUMMARY_FMT    ROCKBOX_DIR "/wrapped_%s.txt"

#define MIN_PLAY_MS    30000  /* like Spotify: 30 s or more counts as a play */
#define FINISHED_PCT   90     /* a play this far through is "finished" */
#define TOP_N          5
#define TRACK_BUCKETS  4096
#define NAME_BUCKETS   1024
#define MAX_DAYS       8192   /* ~22 years of per-day totals */
#define MAX_LOGS       64
#define TAG_CACHE_BUCKETS 1024
#define MAX_TRACK_ARTISTS 6    /* "A; B; C" credits each, up to this many */
#define NAME_PART_LEN  256

/* history lines may carry tags after the path, tab separated:
 * ts:elapsed:length:/path<TAB>artist<TAB>albumartist<TAB>album<TAB>title<TAB>genre
 * FAT names can't contain control characters, so a tab never appears in a
 * path. Lines without tags (older history, raw playback.log) still parse. */
enum { TAG_ARTIST = 0, TAG_ALBUMARTIST, TAG_ALBUM, TAG_TITLE, TAG_GENRE,
       TAG_FIELDS };
#define HIST_TAG_MAXLEN 120   /* bytes per field, cut on a UTF-8 boundary
                                 (tagcache.h already has a TAG_MAXLEN) */
#define TAGS_BUFSZ     (TAG_FIELDS * (HIST_TAG_MAXLEN + 1) + 1)
#define ENTRY_BUFSZ    (MAX_PATH + TAGS_BUFSZ + 64)

#define SLIDE_MAX      12
#define SLIDE_LINES    14
#define SLIDE_LINELEN  128

/****************** data ******************/

struct log_entry {
    unsigned long ts;      /* end of play, local time as seconds */
    long elapsed;          /* ms */
    long length;           /* ms */
    char *path;
    bool has_tags;
    char *tag[TAG_FIELDS]; /* "" when absent */
};

struct name {
    struct name *next_hash, *next_all;
    uint32_t hash;
    uint32_t plays;
    unsigned long long ms;
    char *str;
    char *sub;             /* albums: the artist */
};

struct track {
    struct track *next_hash, *next_all;
    uint32_t hash;
    uint32_t plays;
    uint32_t ms;
    struct name *album;
    struct name *artists[MAX_TRACK_ARTISTS]; /* everyone credited */
    int nartists;
    char *artist_str;          /* "A, B" for display */
    char *title;
    char *path;
    bool tagged;               /* tag[] came from the history file */
    char *tag[TAG_FIELDS];
};

struct name_table {
    struct name **buckets;
    struct name *all;
    int count;
};

static struct {
    int year;                      /* 0 = all time */
    unsigned long start, end;
    unsigned long long total_ms;
    uint32_t plays, skips;
    uint32_t hour_sec[24], wday_sec[7], month_sec[12];
    uint32_t *day_sec;
    unsigned long day0;
    bool day0_set;
    int ndays;
    unsigned long first_play_ts;
    struct track *first_track;
    struct track *top_tracks[TOP_N];
    struct name *top_artists[TOP_N];
    struct name *top_albums[TOP_N];
    int best_day, streak, streak_end;
    uint32_t best_day_sec;
    int unique_tracks, missing_meta;
    uint32_t spoken_skipped;       /* audiobook/podcast entries left out */
    bool out_of_memory;
} st;

static struct track **track_buckets;
static struct track *all_tracks;
static struct name_table artists, albums;

/* resume tracking between consecutive log entries */
static struct track *prev_track;
static uint32_t prev_hash;
static long prev_elapsed, prev_length, play_ms;
static bool prev_counted;

struct line { unsigned char style; char text[SLIDE_LINELEN]; };
struct slide { int n; struct line l[SLIDE_LINES]; };
enum { L_BODY = 0, L_HEAD, L_BIG, L_DIM };
#define L_STYLE_MASK 0x0f
#define L_GLUE       0x10  /* keep on the same page as the next line */
static struct slide slides[SLIDE_MAX];
static int nslides;
static bool have_stats;

static struct mp3entry id3;  /* too big for the stack */
static char this_label[24], last_label[24];

static const char * const month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};
static const char * const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};

/****************** memory ******************/

static char *arena_base, *arena_ptr, *arena_end;

static void *arena_alloc(size_t sz)
{
    sz = (sz + 3) & ~3;
    if (arena_ptr + sz > arena_end)
    {
        st.out_of_memory = true;
        return NULL;
    }
    void *p = arena_ptr;
    arena_ptr += sz;
    return p;
}

static void *arena_zalloc(size_t sz)
{
    void *p = arena_alloc(sz);
    if (p)
        rb->memset(p, 0, sz);
    return p;
}

static char *arena_strdup(const char *s)
{
    size_t n = rb->strlen(s) + 1;
    char *p = arena_alloc(n);
    if (p)
        rb->memcpy(p, s, n);
    return p;
}

static uint32_t hash_str(const char *s, uint32_t h)
{
    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}
#define HASH_SEED 2166136261u

/****************** log parsing ******************/

static bool parse_num(char **pp, unsigned long *out)
{
    char *p = *pp;
    unsigned long v = 0;
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    if (*p != ':')
        return false;
    *pp = p + 1;
    *out = v;
    return true;
}

/* "timestamp:elapsed:length:/path" ; lines copied by the scrobbler are
 * prefixed with ' ' (valid), '@' (duplicate) or '!' (too short) */
static bool parse_line(char *buf, struct log_entry *e)
{
    char *p = buf;
    unsigned long el, len;
    if (*p == ' ')
        p++;
    if (!parse_num(&p, &e->ts) || !parse_num(&p, &el) || !parse_num(&p, &len))
        return false;
    if (*p != '/')
        return false;
    char *end = p + rb->strlen(p);
    while (end > p && (end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    e->elapsed = (long)el;
    e->length  = (long)len;
    if (e->length > 0 && e->elapsed > e->length)
        e->elapsed = e->length;
    e->path = p;
    e->has_tags = false;
    for (int i = 0; i < TAG_FIELDS; i++)
        e->tag[i] = end; /* points at the terminating '\0' */

    char *f = rb->strchr(p, '\t');
    if (f)
    {
        *f++ = '\0';
        e->has_tags = true;
        for (int i = 0; i < TAG_FIELDS && f; i++)
        {
            e->tag[i] = f;
            f = rb->strchr(f, '\t');
            if (f)
                *f++ = '\0';
        }
    }
    return true;
}

/****************** tags ******************/

static bool tag_set(const char *s)
{
    return s && s[0] && rb->strcmp(s, "<Untagged>") != 0;
}

/* reads the file's tags into id3; false if it couldn't be read */
static bool read_tags(const char *path)
{
    bool ok = false;
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    rb->memset(&id3, 0, sizeof(id3));
    ok = rb->tagcache_fill_tags(&id3, path);
#endif
    if (!ok)
        ok = rb->get_metadata(&id3, -1, path);
    return ok;
}

static void clean_tag(char *dst, const char *src);

/* points f[] at id3's tags, cleaned exactly as the history file stores
 * them, so a live lookup and a saved line give the same names */
static void id3_fields(const char *f[TAG_FIELDS])
{
    static char clean[TAG_FIELDS][HIST_TAG_MAXLEN + 1];
    const char *raw[TAG_FIELDS];
    raw[TAG_ARTIST]      = id3.artist;
    raw[TAG_ALBUMARTIST] = id3.albumartist;
    raw[TAG_ALBUM]       = id3.album;
    raw[TAG_TITLE]       = id3.title;
    raw[TAG_GENRE]       = id3.genre_string;
    for (int i = 0; i < TAG_FIELDS; i++)
    {
        clean_tag(clean[i], raw[i]);
        f[i] = clean[i];
    }
}

/* copies a tag for the history file: capped length, no tabs or newlines */
static void clean_tag(char *dst, const char *src)
{
    size_t n = 0;
    if (tag_set(src))
    {
        while (src[n] && n < HIST_TAG_MAXLEN)
            n++;
        /* don't cut a UTF-8 character in half */
        if (src[n])
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
                n--;
        rb->memcpy(dst, src, n);
    }
    dst[n] = '\0';
    for (char *p = dst; *p; p++)
        if (*p == '\t' || *p == '\r' || *p == '\n')
            *p = ' ';
}

/* "\tartist\talbumartist\talbum\ttitle\tgenre", or "" if f is NULL */
static void format_tags(char *out, const char *f[TAG_FIELDS])
{
    out[0] = '\0';
    if (!f)
        return;
    char *o = out;
    for (int i = 0; i < TAG_FIELDS; i++)
    {
        *o++ = '\t';
        clean_tag(o, f[i]);
        o += rb->strlen(o);
    }
}

/****************** audiobooks & podcasts ******************/

/* case-insensitive (ASCII) substring test */
static bool contains_ci(const char *s, const char *needle)
{
    size_t n = rb->strlen(needle);
    for (; s && *s; s++)
        if (rb->strncasecmp(s, needle, n) == 0)
            return true;
    return false;
}

/* audiobooks and podcasts aren't music, so they're left out of the stats
 * (they stay in the history file). Caught by genre tag, by a folder named
 * like "Audiobooks" anywhere in the path, or by the .m4b extension.
 * Only tagged lines carry a genre; older untagged lines rely on the path. */
static bool is_spoken_word(const struct log_entry *e)
{
    static const char * const genres[] = {
        "audiobook", "audio book", "spoken", "podcast"
    };
    static const char * const dirs[] = {
        "audiobooks", "audiobook", "audio books", "audio book",
        "podcasts", "podcast"
    };

    if (e->has_tags)
        for (unsigned i = 0; i < sizeof(genres) / sizeof(genres[0]); i++)
            if (contains_ci(e->tag[TAG_GENRE], genres[i]))
                return true;

    const char *file = rb->strrchr(e->path, '/');
    for (const char *s = e->path; file && s < file; )
    {
        const char *seg = s + 1, *end = rb->strchr(seg, '/');
        size_t len = end - seg;
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
            if (rb->strlen(dirs[i]) == len
                && rb->strncasecmp(seg, dirs[i], len) == 0)
                return true;
        s = end;
    }

    size_t n = rb->strlen(e->path);
    return n >= 4 && rb->strcasecmp(e->path + n - 4, ".m4b") == 0;
}

/* tag suffixes already looked up during this merge, keyed by path */
struct tag_cache {
    struct tag_cache *next;
    uint32_t hash;
    char *path;
    char *suffix;
};

static bool is_playback_log(const char *name)
{
    size_t len = rb->strlen(name);
    return len >= 12 && rb->strncasecmp(name, "playback", 8) == 0
        && rb->strcasecmp(name + len - 4, ".log") == 0;
}

/* rotated logs (playback_NNNN.log, zero padded) sort by name; the live
 * playback.log is always the newest */
static bool log_before(const char *a, const char *b)
{
    bool a_live = rb->strcasecmp(a, ROCKBOX_DIR "/playback.log") == 0;
    bool b_live = rb->strcasecmp(b, ROCKBOX_DIR "/playback.log") == 0;
    if (a_live != b_live)
        return b_live;
    return rb->strcasecmp(a, b) < 0;
}

/* ---- set of (timestamp, path) pairs already in the history ----
 * 64-bit keys from two different string hashes, so a false "already saved"
 * (which would drop a play) is practically impossible even for huge logs */
static uint64_t *seen_keys;
static uint32_t seen_mask;

static uint64_t entry_key(const struct log_entry *e)
{
    uint32_t a = hash_str(e->path, HASH_SEED) ^ (uint32_t)(e->ts * 2654435761u);
    uint32_t b = 5381;                      /* djb2 */
    for (const char *s = e->path; *s; s++)
        b = b * 33 + (unsigned char)*s;
    b += (uint32_t)(e->ts * 40503u);
    uint64_t k = ((uint64_t)a << 32) | b;
    return k ? k : 1;                       /* 0 marks an empty slot */
}

/* 1 = added, 0 = already in the set, -1 = set is full */
static int seen_add(uint64_t key)
{
    uint32_t slot = (uint32_t)(key ^ (key >> 32)) & seen_mask;
    for (uint32_t probe = 0; probe <= seen_mask; probe++)
    {
        if (seen_keys[slot] == key)
            return 0;
        if (seen_keys[slot] == 0)
        {
            seen_keys[slot] = key;
            return 1;
        }
        slot = (slot + 1) & seen_mask;
    }
    return -1;
}

static off_t file_size(const char *path)
{
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    /* lseek, not filesize: newer Rockbox renamed that API (ffilesize) */
    off_t size = rb->lseek(fd, 0, SEEK_END);
    rb->close(fd);
    return size > 0 ? size : 0;
}

/* sizes the set from the files involved (a line is at least ~32 bytes)
 * and fills it with the history; false if there isn't memory for it */
static bool load_seen(off_t bytes, unsigned long *last)
{
    static char buf[ENTRY_BUFSZ];
    struct log_entry e;
    uint32_t est = (uint32_t)(bytes / 32) + 256, slots = 1024;
    while (slots < 2 * est && slots < (1u << 24))
        slots <<= 1;

    bool oom = st.out_of_memory;
    char *raw = arena_zalloc(slots * sizeof(uint64_t) + 8);
    st.out_of_memory = oom;
    if (!raw)
        return false;
    /* 8-byte align: 64-bit loads on ARM may need it */
    seen_keys = (uint64_t *)(((uintptr_t)raw + 7) & ~(uintptr_t)7);
    seen_mask = slots - 1;

    int fd = rb->open(HISTORY_FILE, O_RDONLY);
    if (fd < 0)
        return true;                        /* no history yet: empty set */
    bool ok = true;
    while (rb->read_line(fd, buf, sizeof(buf)) > 0)
    {
        if (!parse_line(buf, &e))
            continue;
        if (e.ts > *last)
            *last = e.ts;
        if (seen_add(entry_key(&e)) < 0)
        {
            ok = false;
            break;
        }
    }
    rb->close(fd);
    return ok;
}

static unsigned long history_last_ts(void)
{
    static char buf[ENTRY_BUFSZ];
    struct log_entry e;
    unsigned long last = 0;
    int fd = rb->open(HISTORY_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    while (rb->read_line(fd, buf, sizeof(buf)) > 0)
    {
        if (parse_line(buf, &e) && e.ts > last)
            last = e.ts;
    }
    rb->close(fd);
    return last;
}

/* the tag suffix to write after e's path: the line's own tags if it has
 * them, otherwise looked up from the file (once per path per merge).
 * Empty when the file can't be read, so later runs fall back as before. */
static const char *entry_tags(const struct log_entry *e,
                              struct tag_cache **cache)
{
    static char suffix[TAGS_BUFSZ];
    const char *f[TAG_FIELDS];
    if (e->has_tags)
    {
        for (int i = 0; i < TAG_FIELDS; i++)
            f[i] = e->tag[i];
        format_tags(suffix, f);
        return suffix;
    }

    uint32_t h = hash_str(e->path, HASH_SEED);
    struct tag_cache **b = cache ? &cache[h & (TAG_CACHE_BUCKETS - 1)] : NULL;
    if (b)
    {
        for (struct tag_cache *c = *b; c; c = c->next)
            if (c->hash == h && rb->strcmp(c->path, e->path) == 0)
                return c->suffix;
    }

    if (read_tags(e->path))
    {
        id3_fields(f);
        format_tags(suffix, f);
    }
    else
        format_tags(suffix, NULL);

    if (b)
    {
        /* cache misses on out of memory are harmless, just slower */
        char *mark = arena_ptr;
        bool oom = st.out_of_memory;
        struct tag_cache *c = arena_alloc(sizeof(*c));
        if (c && (c->path = arena_strdup(e->path))
              && (c->suffix = arena_strdup(suffix)))
        {
            c->hash = h;
            c->next = *b;
            *b = c;
        }
        else
            arena_ptr = mark;
        st.out_of_memory = oom;
    }
    return suffix;
}

/* copy log entries that aren't in the archive yet into it, with their
 * tags; returns the number added */
static int merge_logs(void)
{
    static char buf[ENTRY_BUFSZ];
    int nlogs = 0, added = 0;
    struct log_entry e;
    struct dirent *de;
    char *mark = arena_ptr;
    long next_update = *rb->current_tick + HZ / 2;

    /* the log names live in the arena, not in static memory: 16 KB is a
     * lot on players with a 64 KB plugin buffer (Sansa Clip v1) */
    char (*names)[MAX_PATH] = arena_alloc(MAX_LOGS * MAX_PATH);
    if (!names)
        return -1;

    struct tag_cache **cache =
        arena_zalloc(TAG_CACHE_BUCKETS * sizeof(struct tag_cache *));

    DIR *dir = rb->opendir(ROCKBOX_DIR);
    if (!dir)
    {
        arena_ptr = mark;
        return -1;
    }
    while ((de = rb->readdir(dir)) != NULL && nlogs < MAX_LOGS)
    {
        if (is_playback_log((char *)de->d_name))
            rb->snprintf(names[nlogs++], MAX_PATH, ROCKBOX_DIR "/%s", de->d_name);
    }
    rb->closedir(dir);

    /* readdir order is arbitrary: merge oldest first (playback_0001.log,
     * _0002, ..., then the live playback.log) so the history stays in time
     * order, which the stop/resume merging relies on */
    for (int i = 1; i < nlogs; i++)
    {
        for (int j = i; j > 0 && log_before(names[j], names[j - 1]); j--)
        {
            static char tmp[MAX_PATH];
            rb->strlcpy(tmp, names[j], MAX_PATH);
            rb->strlcpy(names[j], names[j - 1], MAX_PATH);
            rb->strlcpy(names[j - 1], tmp, MAX_PATH);
        }
    }

    /* Add every log line that isn't already saved, whatever its time:
     * plays stamped while the clock was wrong (an iPod's clock resets to
     * an old date when the battery runs flat) still get in. Without the
     * memory for that, only take lines newer than the newest saved one. */
    off_t bytes = file_size(HISTORY_FILE);
    for (int i = 0; i < nlogs; i++)
        bytes += file_size(names[i]);
    unsigned long last = 0;
    bool by_set = load_seen(bytes, &last);
    if (!by_set)
        last = history_last_ts();

    int out = -1;
    for (int i = 0; i < nlogs; i++)
    {
        int fd = rb->open(names[i], O_RDONLY);
        if (fd < 0)
            continue;
        while (rb->read_line(fd, buf, sizeof(buf)) > 0)
        {
            if (!parse_line(buf, &e))
                continue;
            if (by_set)
            {
                int r = seen_add(entry_key(&e));
                if (r == 0 || (r < 0 && e.ts <= last))
                    continue;   /* saved already (or set full: use cutoff) */
            }
            else if (e.ts <= last)
                continue;
            if (out < 0)
            {
                bool fresh = !rb->file_exists(HISTORY_FILE);
                out = rb->open(HISTORY_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
                if (out < 0)
                {
                    rb->close(fd);
                    arena_ptr = mark;
                    return -1;
                }
                if (fresh)
                    rb->fdprintf(out, "# Listening Wrapped history v2: "
                                 "timestamp:elapsed_ms:length_ms:path"
                                 "<TAB>artist<TAB>album artist<TAB>album"
                                 "<TAB>title<TAB>genre\n");
            }
            if (TIME_AFTER(*rb->current_tick, next_update))
            {
                rb->splashf(0, "Saving new plays... %d", added);
                next_update = *rb->current_tick + HZ / 2;
            }
            rb->fdprintf(out, "%lu:%ld:%ld:%s%s\n",
                         e.ts, e.elapsed, e.length, e.path,
                         entry_tags(&e, cache));
            added++;
            rb->yield();
        }
        rb->close(fd);
    }
    if (out >= 0)
        rb->close(out);
    arena_ptr = mark;
    return added;
}

/****************** aggregation ******************/

static struct track *get_track(const char *path)
{
    uint32_t h = hash_str(path, HASH_SEED);
    struct track **b = &track_buckets[h & (TRACK_BUCKETS - 1)];
    for (struct track *t = *b; t; t = t->next_hash)
    {
        if (t->hash == h && rb->strcmp(t->path, path) == 0)
            return t;
    }
    char *mark = arena_ptr;
    struct track *t = arena_zalloc(sizeof(*t));
    if (!t || !(t->path = arena_strdup(path)))
    {
        arena_ptr = mark;
        return NULL;
    }
    t->hash = h;
    t->next_hash = *b;
    *b = t;
    t->next_all = all_tracks;
    all_tracks = t;
    st.unique_tracks++;
    return t;
}

static struct name *get_name(struct name_table *tbl, const char *str,
                             const char *sub)
{
    uint32_t h = hash_str(str, HASH_SEED);
    if (sub)
        h = hash_str(sub, h ^ 0x1f);
    struct name **b = &tbl->buckets[h & (NAME_BUCKETS - 1)];
    for (struct name *n = *b; n; n = n->next_hash)
    {
        if (n->hash == h && rb->strcmp(n->str, str) == 0
            && (!sub || (n->sub && rb->strcmp(n->sub, sub) == 0)))
            return n;
    }
    char *mark = arena_ptr;
    struct name *n = arena_zalloc(sizeof(*n));
    if (!n || !(n->str = arena_strdup(str))
        || (sub && !(n->sub = arena_strdup(sub))))
    {
        arena_ptr = mark;
        return NULL;
    }
    n->hash = h;
    n->next_hash = *b;
    *b = n;
    n->next_all = tbl->all;
    tbl->all = n;
    tbl->count++;
    return n;
}

/* the history is in time order, so the newest tags for a track win
 * (someone may have fixed their tags since an older play) */
static void store_tags(struct track *t, const struct log_entry *e)
{
    for (int i = 0; i < TAG_FIELDS; i++)
    {
        if (t->tag[i] && rb->strcmp(t->tag[i], e->tag[i]) == 0)
            continue;
        char *s = arena_strdup(e->tag[i]);
        if (!s)
            return; /* keep what we have; out_of_memory is already set */
        t->tag[i] = s;
    }
    t->tagged = true;
}

static void process_entry(const struct log_entry *e)
{
    uint32_t h = hash_str(e->path, HASH_SEED);
    struct track *t = get_track(e->path);
    bool resume = false;
    long delta;

    if (t && e->has_tags)
        store_tags(t, e);

    /* stopping mid-track and resuming later logs two entries; the second
     * one's elapsed time includes the first, so only count the difference */
    if (h == prev_hash && t == prev_track && e->length == prev_length
        && prev_elapsed < prev_length / 100 * FINISHED_PCT
        && e->elapsed >= prev_elapsed)
        resume = true;

    if (resume)
    {
        delta = e->elapsed - prev_elapsed;
        play_ms += delta;
    }
    else
    {
        delta = e->elapsed;
        play_ms = delta;
        prev_counted = false;
        if (e->elapsed < MIN_PLAY_MS && e->length > MIN_PLAY_MS)
            st.skips++;
    }

    prev_hash = h;
    prev_track = t;
    prev_elapsed = e->elapsed;
    prev_length = e->length;

    if (delta <= 0)
        return;

    st.total_ms += delta;
    if (t)
        t->ms += delta;

    if (!prev_counted && play_ms >= MIN_PLAY_MS)
    {
        prev_counted = true;
        st.plays++;
        if (t)
        {
            t->plays++;
            if (!st.first_track || e->ts < st.first_play_ts)
            {
                st.first_track = t;
                st.first_play_ts = e->ts;
            }
        }
    }

    time_t ts = (time_t)e->ts;
    struct tm tm;
    rb->gmtime_r(&ts, &tm);
    uint32_t sec = delta / 1000;
    st.hour_sec[tm.tm_hour % 24] += sec;
    st.wday_sec[tm.tm_wday % 7] += sec;
    st.month_sec[tm.tm_mon % 12] += sec;

    unsigned long day = e->ts / 86400;
    if (!st.day0_set)
    {
        st.day0 = day;
        st.day0_set = true;
    }
    if (st.day_sec && day >= st.day0 && day - st.day0 < MAX_DAYS)
    {
        int idx = day - st.day0;
        st.day_sec[idx] += sec;
        if (idx + 1 > st.ndays)
            st.ndays = idx + 1;
    }
}

/* guess artist/album from /.../Artist/Album/file when tags are missing */
static void guess_from_path(const char *path, char *artist, char *album,
                            char *title, size_t sz)
{
    const char *seg[8];
    int n = 0;
    for (const char *p = path; *p; p++)
    {
        if (*p == '/')
        {
            if (n == 8)
            {
                rb->memmove(seg, seg + 1, sizeof(seg[0]) * 7);
                n = 7;
            }
            seg[n++] = p + 1;
        }
    }
    artist[0] = album[0] = '\0';
    /* seg[n-1] = file, seg[n-2] = album dir, seg[n-3] = artist dir */
    if (n >= 3)
    {
        const char *a = seg[n - 3], *b = seg[n - 2];
        rb->strlcpy(artist, a, MIN(sz, (size_t)(b - a)));
        rb->strlcpy(album, b, MIN(sz, (size_t)(seg[n - 1] - b)));
    }
    rb->strlcpy(title, n ? seg[n - 1] : path, sz);
    char *dot = rb->strrchr(title, '.');
    if (dot)
        *dot = '\0';
    char *q = title;
    while (*q >= '0' && *q <= '9')
        q++;
    if (q > title && q - title <= 3 && (*q == ' ' || *q == '-' || *q == '.' || *q == '_'))
    {
        while (*q == ' ' || *q == '-' || *q == '.' || *q == '_')
            q++;
        if (*q)
            rb->memmove(title, q, rb->strlen(q) + 1);
    }
}

/* "A; B ;C" -> distinct, space-trimmed, non-empty names (at most
 * MAX_TRACK_ARTISTS). Only ';' splits: "Simon & Garfunkel" and "AC/DC"
 * are one name each. */
static int split_names(const char *s, char parts[][NAME_PART_LEN])
{
    int n = 0;
    while (*s && n < MAX_TRACK_ARTISTS)
    {
        const char *end = rb->strchr(s, ';');
        if (!end)
            end = s + rb->strlen(s);
        const char *a = s, *b = end;
        while (a < b && *a == ' ')
            a++;
        while (b > a && b[-1] == ' ')
            b--;
        size_t len = MIN((size_t)(b - a), (size_t)NAME_PART_LEN - 1);
        if (len)
        {
            rb->memcpy(parts[n], a, len);
            parts[n][len] = '\0';
            bool dup = false;
            for (int i = 0; i < n && !dup; i++)
                dup = rb->strcmp(parts[i], parts[n]) == 0;
            if (!dup)
                n++;
        }
        s = *end ? end + 1 : end;
    }
    return n;
}

static void join_names(char parts[][NAME_PART_LEN], int n, char *out,
                       size_t sz)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++)
    {
        if (i)
            rb->strlcat(out, ", ", sz);
        rb->strlcat(out, parts[i], sz);
    }
}

#define JOINED_LEN (MAX_TRACK_ARTISTS * (NAME_PART_LEN + 2))

static void load_metadata(void)
{
    static char parts[MAX_TRACK_ARTISTS][NAME_PART_LEN];
    static char aparts[MAX_TRACK_ARTISTS][NAME_PART_LEN];
    static char shown[JOINED_LEN], album_by[JOINED_LEN];
    static char g_artist[128], g_album[128], g_title[128];
    int done = 0;
    long next_update = *rb->current_tick;

    artists.buckets = arena_zalloc(NAME_BUCKETS * sizeof(struct name *));
    albums.buckets  = arena_zalloc(NAME_BUCKETS * sizeof(struct name *));
    if (!artists.buckets || !albums.buckets)
        return;

    for (struct track *t = all_tracks; t; t = t->next_all, done++)
    {
        if (TIME_AFTER(*rb->current_tick, next_update))
        {
            rb->splash_progress(done, st.unique_tracks, "Reading tags");
            next_update = *rb->current_tick + HZ / 2;
        }
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
            break;

        /* tags saved in the history need no file access; only older
         * untagged lines need the slow lookup */
        const char *f[TAG_FIELDS] = { NULL };
        if (t->tagged)
        {
            for (int i = 0; i < TAG_FIELDS; i++)
                f[i] = t->tag[i];
        }
        else if (read_tags(t->path))
            id3_fields(f);

        guess_from_path(t->path, g_artist, g_album, g_title, sizeof(g_title));
        const char *artist = g_artist, *album = g_album, *title = g_title;
        if (tag_set(f[TAG_ARTIST]))
            artist = f[TAG_ARTIST];
        else if (tag_set(f[TAG_ALBUMARTIST]))
            artist = f[TAG_ALBUMARTIST];
        else
            st.missing_meta++;
        if (tag_set(f[TAG_ALBUM]))
            album = f[TAG_ALBUM];
        if (tag_set(f[TAG_TITLE]))
            title = f[TAG_TITLE];
        if (!artist[0])
            artist = "Unknown artist";

        /* credit every listed artist, like Spotify does */
        int n = split_names(artist, parts);
        if (!n)
        {
            rb->strlcpy(parts[0], "Unknown artist", NAME_PART_LEN);
            n = 1;
        }
        join_names(parts, n, shown, sizeof(shown));
        t->artist_str = arena_strdup(shown);
        t->nartists = 0;
        for (int i = 0; i < n; i++)
        {
            struct name *nm = get_name(&artists, parts[i], NULL);
            if (nm)
                t->artists[t->nartists++] = nm;
        }

        /* an album is by its album artist tag (tidied the same way),
         * else by the first credited artist */
        int an = tag_set(f[TAG_ALBUMARTIST])
                 ? split_names(f[TAG_ALBUMARTIST], aparts) : 0;
        if (an)
            join_names(aparts, an, album_by, sizeof(album_by));
        else
            rb->strlcpy(album_by, parts[0], sizeof(album_by));
        if (album[0])
            t->album = get_name(&albums, album, album_by);
        t->title = arena_strdup(title);
        rb->yield();
    }
}

static bool track_better(const struct track *a, const struct track *b)
{
    return a->plays > b->plays || (a->plays == b->plays && a->ms > b->ms);
}

static bool name_better(const struct name *a, const struct name *b)
{
    return a->ms > b->ms || (a->ms == b->ms && a->plays > b->plays);
}

static void rank(void)
{
    for (struct track *t = all_tracks; t; t = t->next_all)
    {
        /* a song counts in full for each artist credited on it */
        for (int i = 0; i < t->nartists; i++)
        {
            t->artists[i]->ms += t->ms;
            t->artists[i]->plays += t->plays;
        }
        if (t->album)  { t->album->ms  += t->ms; t->album->plays  += t->plays; }
        if (!t->plays)
            continue;
        for (int i = 0; i < TOP_N; i++)
        {
            if (!st.top_tracks[i] || track_better(t, st.top_tracks[i]))
            {
                rb->memmove(&st.top_tracks[i + 1], &st.top_tracks[i],
                            sizeof(st.top_tracks[0]) * (TOP_N - 1 - i));
                st.top_tracks[i] = t;
                break;
            }
        }
    }
    struct name_table *tbls[2] = { &artists, &albums };
    struct name **tops[2] = { st.top_artists, st.top_albums };
    for (int k = 0; k < 2; k++)
    {
        for (struct name *n = tbls[k]->all; n; n = n->next_all)
        {
            if (!n->plays)
                continue;
            for (int i = 0; i < TOP_N; i++)
            {
                if (!tops[k][i] || name_better(n, tops[k][i]))
                {
                    rb->memmove(&tops[k][i + 1], &tops[k][i],
                                sizeof(tops[k][0]) * (TOP_N - 1 - i));
                    tops[k][i] = n;
                    break;
                }
            }
        }
    }

    int run = 0;
    for (int d = 0; d < st.ndays; d++)
    {
        if (st.day_sec[d] > st.best_day_sec)
        {
            st.best_day_sec = st.day_sec[d];
            st.best_day = d;
        }
        run = st.day_sec[d] ? run + 1 : 0;
        if (run > st.streak)
        {
            st.streak = run;
            st.streak_end = d;
        }
    }
}

static unsigned long year_start(int year)
{
    struct tm tm;
    rb->memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mday = 1;
    return (unsigned long)rb->mktime(&tm);
}

static bool compute_stats(int year)
{
    static char buf[ENTRY_BUFSZ];
    struct log_entry e;

    arena_ptr = arena_base;
    rb->memset(&st, 0, sizeof(st));
    rb->memset(&artists, 0, sizeof(artists));
    rb->memset(&albums, 0, sizeof(albums));
    all_tracks = NULL;
    prev_track = NULL;
    prev_hash = 0;
    prev_elapsed = prev_length = play_ms = 0;
    prev_counted = false;

    st.year = year;
    if (year)
    {
        st.start = year_start(year);
        st.end = year_start(year + 1);
        st.day0 = st.start / 86400;
        st.day0_set = true;
    }
    else
    {
        st.start = 0;
        st.end = ~0UL;
    }

    track_buckets = arena_zalloc(TRACK_BUCKETS * sizeof(struct track *));
    st.day_sec = arena_zalloc(MAX_DAYS * sizeof(uint32_t));
    if (!track_buckets || !st.day_sec)
        return false;

    int fd = rb->open(HISTORY_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    off_t size = rb->lseek(fd, 0, SEEK_END);
    rb->lseek(fd, 0, SEEK_SET);
    off_t pos = 0;
    long next_update = *rb->current_tick;
    int rd;

    while ((rd = rb->read_line(fd, buf, sizeof(buf))) > 0)
    {
        pos += rd;
        if (TIME_AFTER(*rb->current_tick, next_update))
        {
            rb->splash_progress(pos / 1024, size / 1024 + 1, "Counting plays");
            next_update = *rb->current_tick + HZ / 2;
            rb->yield();
        }
        if (!parse_line(buf, &e) || e.ts < st.start || e.ts >= st.end)
            continue;
        if (is_spoken_word(&e))
        {
            st.spoken_skipped++;
            /* break any stop/resume chain, as the entry would have */
            prev_track = NULL;
            prev_hash = 0;
            continue;
        }
        process_entry(&e);
    }
    rb->close(fd);

    load_metadata();
    rank();
    return true;
}

/****************** slides ******************/

static struct slide *new_slide(void)
{
    if (nslides >= SLIDE_MAX)
        return NULL;
    struct slide *s = &slides[nslides++];
    s->n = 0;
    return s;
}

static void add_line(struct slide *s, int style, const char *fmt, ...)
{
    if (!s || s->n >= SLIDE_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    rb->vsnprintf(s->l[s->n].text, SLIDE_LINELEN, fmt, ap);
    va_end(ap);
    s->l[s->n].style = style;
    s->n++;
}

static const char *fmt_num(unsigned long long v, char *buf, size_t sz)
{
    char tmp[24];
    int len = rb->snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v);
    if (v > 0xFFFFFFFFull)
        len = rb->snprintf(tmp, sizeof(tmp), "%lu%03lu",
                           (unsigned long)(v / 1000), (unsigned long)(v % 1000));
    size_t o = 0;
    for (int i = 0; i < len && o + 1 < sz; i++)
    {
        if (i && (len - i) % 3 == 0 && o + 2 < sz)
            buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
    return buf;
}

static int argmax(const uint32_t *a, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (a[i] > a[best])
            best = i;
    return best;
}

static void day_label(int idx, char *buf, size_t sz)
{
    time_t ts = (time_t)((st.day0 + idx) * 86400UL);
    struct tm tm;
    rb->gmtime_r(&ts, &tm);
    if (st.year)
        rb->snprintf(buf, sz, "%s %d", month_names[tm.tm_mon], tm.tm_mday);
    else
        rb->snprintf(buf, sz, "%s %d, %d", month_names[tm.tm_mon],
                     tm.tm_mday, tm.tm_year + 1900);
}

static void build_slides(void)
{
    char n1[24], n2[24], label[48];
    struct slide *s;
    const char *period = label;
    nslides = 0;

    if (st.year)
        rb->snprintf(label, sizeof(label), "%d", st.year);
    else
        rb->strlcpy(label, "All time", sizeof(label));

    unsigned long minutes = (unsigned long)(st.total_ms / 60000);

    s = new_slide();
    add_line(s, L_DIM | L_GLUE, "Your");
    add_line(s, L_BIG | L_GLUE, "%s", period);
    add_line(s, L_HEAD, "Wrapped");
    if (!st.plays)
    {
        add_line(s, L_BODY, "");
        add_line(s, L_BODY, "No plays logged for this period yet.");
        add_line(s, L_DIM, "Turn on Playback Settings > Logging and "
                           "check back after some listening.");
        return;
    }
    add_line(s, L_BODY, "");
    add_line(s, L_DIM, "Scroll or press Select to continue");

    s = new_slide();
    add_line(s, L_DIM | L_GLUE, "You listened for");
    add_line(s, L_BIG | L_GLUE, "%s", fmt_num(minutes, n1, sizeof(n1)));
    add_line(s, L_HEAD, "minutes");
    add_line(s, L_BODY, "");
    if (minutes >= 60 * 24)
        add_line(s, L_BODY, "That's %lu.%lu days of music.",
                 minutes / 1440, (minutes % 1440) * 10 / 1440);
    else
        add_line(s, L_BODY, "That's %lu hours of music.", minutes / 60);
    add_line(s, L_DIM, "%s plays across %s songs",
             fmt_num(st.plays, n1, sizeof(n1)),
             fmt_num(st.unique_tracks, n2, sizeof(n2)));

    if (st.top_artists[0])
    {
        struct name *a = st.top_artists[0];
        s = new_slide();
        add_line(s, L_DIM | L_GLUE, "Your top artist was");
        add_line(s, L_BIG, "%s", a->str);
        add_line(s, L_BODY, "");
        add_line(s, L_BODY, "%s minutes, %s plays",
                 fmt_num(a->ms / 60000, n1, sizeof(n1)),
                 fmt_num(a->plays, n2, sizeof(n2)));
        if (st.total_ms)
            add_line(s, L_DIM, "%lu%% of everything you played",
                     (unsigned long)(a->ms * 100 / st.total_ms));

        s = new_slide();
        add_line(s, L_HEAD, "Top artists");
        add_line(s, L_BODY, "");
        for (int i = 0; i < TOP_N && st.top_artists[i]; i++)
            add_line(s, L_BODY, "%d. %s  (%s min)", i + 1, st.top_artists[i]->str,
                     fmt_num(st.top_artists[i]->ms / 60000, n1, sizeof(n1)));
        add_line(s, L_BODY, "");
        add_line(s, L_DIM, "%d artists in total", artists.count);
    }

    if (st.top_tracks[0])
    {
        struct track *t = st.top_tracks[0];
        s = new_slide();
        add_line(s, L_DIM | L_GLUE, st.year ? "Your song of the year" : "Your most played song");
        add_line(s, L_BIG | L_GLUE, "%s", t->title ? t->title : t->path);
        if (t->artist_str)
            add_line(s, L_BODY, "%s", t->artist_str);
        add_line(s, L_BODY, "");
        add_line(s, L_DIM, "Played %s times", fmt_num(t->plays, n1, sizeof(n1)));

        s = new_slide();
        add_line(s, L_HEAD, "Top songs");
        add_line(s, L_BODY, "");
        for (int i = 0; i < TOP_N && st.top_tracks[i]; i++)
        {
            t = st.top_tracks[i];
            add_line(s, L_BODY | L_GLUE, "%d. %s", i + 1, t->title ? t->title : t->path);
            add_line(s, L_DIM, "%s - %lu plays",
                     t->artist_str ? t->artist_str : "", (unsigned long)t->plays);
        }
    }

    if (st.top_albums[0])
    {
        s = new_slide();
        add_line(s, L_HEAD, "Top albums");
        add_line(s, L_BODY, "");
        for (int i = 0; i < TOP_N && st.top_albums[i]; i++)
        {
            struct name *a = st.top_albums[i];
            add_line(s, L_BODY | L_GLUE, "%d. %s", i + 1, a->str);
            add_line(s, L_DIM, "%s - %s min", a->sub ? a->sub : "",
                     fmt_num(a->ms / 60000, n1, sizeof(n1)));
        }
    }

    {
        int h = argmax(st.hour_sec, 24);
        int d = argmax(st.wday_sec, 7);
        int m = argmax(st.month_sec, 12);
        int h12 = h % 12 ? h % 12 : 12;
        s = new_slide();
        add_line(s, L_HEAD, "Your listening clock");
        add_line(s, L_BODY, "");
        add_line(s, L_DIM | L_GLUE, "Peak hour");
        add_line(s, L_BIG, "%d %s", h12, h < 12 ? "AM" : "PM");
        add_line(s, L_DIM | L_GLUE, "Favorite day");
        add_line(s, L_BODY, "%s", day_names[d]);
        add_line(s, L_DIM | L_GLUE, "Biggest month");
        add_line(s, L_BODY, "%s (%s min)", month_names[m],
                 fmt_num(st.month_sec[m] / 60, n1, sizeof(n1)));
    }

    if (st.best_day_sec)
    {
        char day[32];
        s = new_slide();
        add_line(s, L_HEAD, "Big days");
        add_line(s, L_BODY, "");
        day_label(st.best_day, day, sizeof(day));
        add_line(s, L_DIM | L_GLUE, "Most listening in one day");
        add_line(s, L_BIG | L_GLUE, "%s", day);
        add_line(s, L_BODY, "%s minutes",
                 fmt_num(st.best_day_sec / 60, n1, sizeof(n1)));
        add_line(s, L_BODY, "");
        add_line(s, L_DIM | L_GLUE, "Longest streak");
        add_line(s, L_BODY, "%d day%s in a row", st.streak,
                 st.streak == 1 ? "" : "s");
    }

    s = new_slide();
    add_line(s, L_HEAD, "By the numbers");
    add_line(s, L_BODY, "");
    add_line(s, L_BODY, "%s different songs", fmt_num(st.unique_tracks, n1, sizeof(n1)));
    add_line(s, L_BODY, "%s artists", fmt_num(artists.count, n1, sizeof(n1)));
    add_line(s, L_BODY, "%s albums", fmt_num(albums.count, n1, sizeof(n1)));
    add_line(s, L_BODY, "%s skips", fmt_num(st.skips, n1, sizeof(n1)));
    if (st.first_track && st.first_track->title)
    {
        add_line(s, L_BODY, "");
        add_line(s, L_DIM | L_GLUE, "First song of %s:", st.year ? "the year" : "your history");
        add_line(s, L_BODY, "%s", st.first_track->title);
    }
    if (st.missing_meta)
        add_line(s, L_DIM, "(%d song%s had no tags; named from folders)",
                 st.missing_meta, st.missing_meta == 1 ? "" : "s");
    if (st.spoken_skipped)
        add_line(s, L_DIM, "(Audiobooks and podcasts not counted)");
    if (st.out_of_memory)
        add_line(s, L_DIM, "(Ran out of memory - some songs not ranked)");

    s = new_slide();
    add_line(s, L_HEAD, "That's a wrap");
    add_line(s, L_BODY, "");
    add_line(s, L_BODY, "Choose \"Save summary\" in the menu to keep this as a text file.");
}

/****************** drawing ******************/

#ifdef HAVE_LCD_COLOR
static const unsigned slide_bg[] = {
    LCD_RGBPACK(18, 120, 60),  LCD_RGBPACK(70, 40, 170),
    LCD_RGBPACK(170, 30, 90),  LCD_RGBPACK(24, 24, 24),
    LCD_RGBPACK(180, 80, 15),  LCD_RGBPACK(20, 80, 160),
};
#define N_BG (int)(sizeof(slide_bg) / sizeof(slide_bg[0]))
#endif

/* a bigger font for headline lines, if the font pack is installed */
static int big_font = -1;

static void load_big_font(void)
{
    static const char * const candidates[] = {
#if LCD_HEIGHT >= 200
        "27-Adobe-Helvetica-Bold", "28-Terminus-Bold", "24-Terminus-Bold",
#endif
        "21-Adobe-Helvetica-Bold", "20-Terminus-Bold", "18-Adobe-Helvetica-Bold",
    };
    char path[MAX_PATH];
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    {
        rb->snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", candidates[i]);
        if (!rb->file_exists(path))
            continue;
        big_font = rb->font_load(path);
        if (big_font >= 0)
            return;
    }
}

static void set_line_font(int style)
{
    style &= L_STYLE_MASK;
    rb->lcd_setfont(style == L_BIG && big_font >= 0 ? big_font : FONT_UI);
}

/* draws (or just measures, when draw is false) text wrapped to width;
 * returns the height used */
static int draw_wrapped(const char *text, int y, bool draw)
{
    char buf[SLIDE_LINELEN];
    int w, h, used = 0;
    const int width = LCD_WIDTH - 8;
    const char *p = text;

    rb->lcd_getstringsize("Ay", &w, &h);
    if (!*p)
        return h / 2;

    while (*p)
    {
        int fit = 0, j = 0;
        for (;;)
        {
            while (p[j] && p[j] != ' ')
                j++;
            int n = MIN(j, SLIDE_LINELEN - 1);
            rb->memcpy(buf, p, n);
            buf[n] = '\0';
            rb->lcd_getstringsize(buf, &w, NULL);
            if (w > width)
                break;
            fit = n;
            if (!p[j] || n != j)
                break;
            j++;
        }
        if (fit == 0)
        {
            /* one long word: cut it at a character boundary */
            int k = 0;
            for (;;)
            {
                int nk = k + 1;
                while (p[nk] && (p[nk] & 0xC0) == 0x80)
                    nk++;
                if (nk >= SLIDE_LINELEN)
                    break;
                rb->memcpy(buf, p, nk);
                buf[nk] = '\0';
                rb->lcd_getstringsize(buf, &w, NULL);
                if (w > width && k > 0)
                    break;
                k = nk;
                if (!p[k] || p[k] == ' ')
                    break;
            }
            fit = k;
        }
        rb->memcpy(buf, p, fit);
        buf[fit] = '\0';
        if (draw)
        {
            rb->lcd_getstringsize(buf, &w, NULL);
            rb->lcd_putsxy((LCD_WIDTH - w) / 2, y + used, buf);
        }
        used += h;
        p += fit;
        while (*p == ' ')
            p++;
    }
    return used;
}

/* height of one line as drawn, including the extra space around big text */
static int line_height(const struct line *l)
{
    int style = l->style & L_STYLE_MASK;
    set_line_font(style);
    int h = draw_wrapped(l->text, 0, false) + (style == L_BIG ? 4 : 0);
    set_line_font(L_BODY);
    return h;
}

static void draw_line(const struct line *l, int *y)
{
    int style = l->style & L_STYLE_MASK;
#ifdef HAVE_LCD_COLOR
    switch (style)
    {
        case L_HEAD: rb->lcd_set_foreground(LCD_RGBPACK(255, 225, 90)); break;
        case L_DIM:  rb->lcd_set_foreground(LCD_RGBPACK(215, 215, 215)); break;
        default:     rb->lcd_set_foreground(LCD_WHITE); break;
    }
#endif
    if (style == L_BIG)
        *y += 2;
    set_line_font(style);
    *y += draw_wrapped(l->text, *y, true);
    set_line_font(L_BODY);
    if (style == L_BIG)
        *y += 2;
}

/* Slides that don't fit the screen (small ones like the iPod Mini's) are
 * split into pages. A continuation page repeats the slide's heading, and
 * lines marked L_GLUE stay on the same page as the line after them. On
 * screens where every slide fits, there's one page per slide. */
#define MAX_PAGES 64
#define PAGE_AREA (LCD_HEIGHT - 12)      /* room above the page dots */
struct page { uint8_t slide, first, end; bool cont; };
static struct page pages[MAX_PAGES];
static int npages;
static const struct line blank_line = { L_BODY, "" };

static bool has_heading(const struct slide *s)
{
    return s->n && (s->l[0].style & L_STYLE_MASK) == L_HEAD;
}

static void paginate(void)
{
    npages = 0;
    for (int si = 0; si < nslides && npages < MAX_PAGES; si++)
    {
        const struct slide *s = &slides[si];
        int head = has_heading(s)
                   ? line_height(&s->l[0]) + line_height(&blank_line) : 0;
        int i = 0;
        while (i < s->n && npages < MAX_PAGES)
        {
            bool cont = i > 0;
            if (cont)                   /* no blank lines at the top */
                while (i < s->n && !s->l[i].text[0])
                    i++;
            if (i >= s->n)
                break;
            int start = i, used = cont ? head : 0;
            while (i < s->n)
            {
                int j = i, group = 0;
                do
                    group += line_height(&s->l[j]);
                while ((s->l[j++].style & L_GLUE) && j < s->n);
                if (used + group > PAGE_AREA && i > start)
                    break;              /* starts the next page */
                used += group;
                i = j;
            }
            pages[npages++] = (struct page){ si, start, i, cont };
        }
    }
}

static void draw_page(int idx)
{
    const struct page *pg = &pages[idx];
    const struct slide *s = &slides[pg->slide];
    bool head = pg->cont && has_heading(s);
#ifdef HAVE_LCD_COLOR
    rb->lcd_set_background(slide_bg[pg->slide % N_BG]); /* same colour on
                                                           every page */
#endif
    rb->lcd_clear_display();

    int h = head ? line_height(&s->l[0]) + line_height(&blank_line) : 0;
    for (int i = pg->first; i < pg->end; i++)
        h += line_height(&s->l[i]);
    int y = (LCD_HEIGHT - 10 - h) / 2;
    if (y < 2)
        y = 2;

    if (head)
    {
        draw_line(&s->l[0], &y);
        draw_line(&blank_line, &y);
    }
    for (int i = pg->first; i < pg->end; i++)
        draw_line(&s->l[i], &y);

    /* page dots, smaller if there are many pages on a narrow screen */
#ifdef HAVE_LCD_COLOR
    rb->lcd_set_foreground(LCD_WHITE);
#endif
    int dot = 4, gap = 4;
    if (npages * (dot + gap) > LCD_WIDTH - 8)
        dot = 3, gap = 2;
    int x = (LCD_WIDTH - npages * (dot + gap) + gap) / 2;
    if (x < 0)
        x = 0;
    for (int i = 0; i < npages; i++, x += dot + gap)
    {
        if (i == idx)
            rb->lcd_fillrect(x, LCD_HEIGHT - 7, dot, dot);
        else
            rb->lcd_drawrect(x, LCD_HEIGHT - 7, dot, dot);
    }
    rb->lcd_update();
}

static int show_slides(void)
{
    int cur = 0, ret = PLUGIN_OK;
    long last_scroll = 0;
    bool done = false;

    paginate();
    if (!npages)
        return ret;
    draw_page(cur);
    while (!done)
    {
        int prev = cur;
        int action = rb->get_action(CONTEXT_STD, TIMEOUT_BLOCK);
        switch (action)
        {
            case ACTION_STD_NEXT:
                if (TIME_BEFORE(*rb->current_tick, last_scroll + HZ / 4))
                    break;
                last_scroll = *rb->current_tick;
                /* fall through */
            case ACTION_STD_OK:
                if (cur < npages - 1)
                    cur++;
                else if (action == ACTION_STD_OK)
                    done = true;
                break;
            case ACTION_STD_PREV:
                if (TIME_BEFORE(*rb->current_tick, last_scroll + HZ / 4))
                    break;
                last_scroll = *rb->current_tick;
                if (cur > 0)
                    cur--;
                break;
            case ACTION_STD_CANCEL:
                if (cur > 0)
                    cur--;
                else
                    done = true;
                break;
            case ACTION_STD_MENU:
                done = true;
                break;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = PLUGIN_USB_CONNECTED;
                    done = true;
                }
                break;
        }
        if (!done && cur != prev)
            draw_page(cur);
    }
#ifdef HAVE_LCD_COLOR
    rb->lcd_set_background(LCD_DEFAULT_BG);
    rb->lcd_set_foreground(LCD_DEFAULT_FG);
#endif
    return ret;
}

static void save_summary(void)
{
    char path[MAX_PATH], label[16];
    if (st.year)
        rb->snprintf(label, sizeof(label), "%d", st.year);
    else
        rb->strlcpy(label, "all-time", sizeof(label));
    rb->snprintf(path, sizeof(path), SUMMARY_FMT, label);

    int fd = rb->open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        rb->splash(HZ * 2, "Could not save summary");
        return;
    }
    for (int i = 0; i < nslides; i++)
    {
        for (int j = 0; j < slides[i].n; j++)
            rb->fdprintf(fd, "%s\n", slides[i].l[j].text);
        rb->fdprintf(fd, "\n----\n\n");
    }
    rb->close(fd);
    rb->splashf(HZ * 2, "Saved %s", path);
}

/****************** main ******************/

static void ask_enable_logging(void)
{
    const char *lines[] = { "Listening Wrapped", "Playback logging is off",
                            "Turn it on?" };
    const char *yes[] = { "Logging enabled", "Your plays will be counted" };
    const struct text_message message = { lines, 3 };
    const struct text_message yes_msg = { yes, 2 };
    if (rb->gui_syncyesno_run(&message, &yes_msg, NULL) == YESNO_YES)
    {
        rb->global_settings->playback_log = true;
        rb->settings_save();
        rb->sleep(HZ);
    }
}

static int run_wrapped(int year)
{
    rb->splash(0, "Crunching numbers...");
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    bool ok = compute_stats(year);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
    if (!ok)
    {
        rb->splash(HZ * 2, "No listening history yet");
        return PLUGIN_OK;
    }
    build_slides();
    have_stats = true;
    return show_slides();
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    size_t size;
    int ret = PLUGIN_OK;

    load_big_font(); /* before grabbing the audio buffer fonts load from */

    /* the audio buffer is much bigger, but only free while nothing plays */
    if (rb->audio_status() == 0)
        arena_base = rb->plugin_get_audio_buffer(&size);
    else
        arena_base = rb->plugin_get_buffer(&size);
    arena_ptr = arena_base;
    arena_end = arena_base + size;

    if (!rb->global_settings->playback_log)
        ask_enable_logging();

    rb->add_playbacklog(NULL); /* flush anything still buffered in RAM */
    rb->splash(0, "Reading playback log...");
    int added = merge_logs();
    if (added < 0)
        rb->splash(HZ * 2, "Could not update history");

    int this_year = rb->get_time()->tm_year + 1900;
    rb->snprintf(this_label, sizeof(this_label), "%d Wrapped", this_year);
    rb->snprintf(last_label, sizeof(last_label), "%d Wrapped", this_year - 1);

    MENUITEM_STRINGLIST(menu, "Listening Wrapped", NULL,
                        this_label, last_label, "All time",
                        "Save summary", "Quit");
    int selection = 0;
    bool quit = false;
    while (!quit && ret == PLUGIN_OK)
    {
        switch (rb->do_menu(&menu, &selection, NULL, false))
        {
            case 0: ret = run_wrapped(this_year); break;
            case 1: ret = run_wrapped(this_year - 1); break;
            case 2: ret = run_wrapped(0); break;
            case 3:
                if (!have_stats)
                {
                    rb->splash(0, "Crunching numbers...");
                    if (compute_stats(this_year))
                    {
                        build_slides();
                        have_stats = true;
                    }
                }
                if (have_stats)
                    save_summary();
                break;
            case MENU_ATTACHED_USB:
                ret = PLUGIN_USB_CONNECTED;
                break;
            default:
                quit = true;
                break;
        }
    }
    if (big_font >= 0)
        rb->font_unload(big_font);
    return ret;
}
