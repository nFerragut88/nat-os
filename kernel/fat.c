/* nat-os — read-only FAT16/FAT32. See fat.h.
 *
 * Offsets are the ones in Microsoft's "FAT: General Overview of On-Disk
 * Format" (fatgen103), the document every FAT implementation descends from.
 */

#include <stddef.h>
#include "fat.h"
#include "sd.h"
#include "uart.h"
#include "xtensa.h"
#include "mp3hdr.h"

void *memcpy(void *dst, const void *src, size_t n);     /* kstring.c */

#define CPU_HZ 80000000u

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- the volume ------------------------------------------------------------ */
static int      g_mounted;
static int      g_fat32;
static uint32_t g_part_lba;         /* first block of the volume */
static uint32_t g_spc;              /* sectors per cluster */
static uint32_t g_fat_lba;          /* first FAT */
static uint32_t g_fat_sectors;
static uint32_t g_root_lba;         /* FAT16 fixed root region */
static uint32_t g_root_sectors;     /* 0 on FAT32 */
static uint32_t g_root_cluster;     /* FAT32 root, 0 on FAT16 */
static uint32_t g_data_lba;         /* cluster 2 */
static uint32_t g_clusters;         /* data clusters, the FAT-type decider */

/* One sector for directories and partial file reads, one for the FAT. Each
 * remembers which block it holds, so walking a chain inside one FAT sector
 * costs one read, not one per cluster. */
static uint8_t  g_sec[SD_BLOCK_SIZE];
static uint32_t g_sec_lba = 0xFFFFFFFFu;
static uint8_t  g_fatsec[SD_BLOCK_SIZE];
static uint32_t g_fatsec_lba = 0xFFFFFFFFu;

static uint32_t g_blocks_read;      /* instrument: every sd_read_block issued */
static uint32_t g_read_hops;        /* instrument: clusters fat_read() followed */

int fat_mounted(void) { return g_mounted; }

static int read_block(uint32_t lba, uint8_t *dst)
{
    g_blocks_read++;
    return sd_read_block(lba, dst) == SD_OK ? FAT_OK : FAT_ERR_SD;
}

static int load_sec(uint32_t lba)
{
    if (g_sec_lba == lba) {
        return FAT_OK;
    }
    g_sec_lba = 0xFFFFFFFFu;        /* a failed read must not look cached */
    int rc = read_block(lba, g_sec);
    if (rc == FAT_OK) {
        g_sec_lba = lba;
    }
    return rc;
}

const char *fat_strerror(int rc)
{
    switch (rc) {
    case FAT_OK:         return "ok";
    case FAT_ERR_SD:     return "SD read failed";
    case FAT_ERR_NOFS:   return "no FAT boot sector";
    case FAT_ERR_TYPE:   return "FAT12/exFAT/odd geometry - refused";
    case FAT_ERR_NOENT:  return "not found";
    case FAT_ERR_NOTDIR: return "not a directory";
    case FAT_ERR_ISDIR:  return "is a directory";
    case FAT_ERR_CHAIN:  return "cluster chain broken";
    case FAT_ERR_MOUNT:  return "not mounted";
    default:             return "?";
    }
}

/* A boot sector is a FAT one if its geometry is sane. The 0x55AA signature
 * alone does not say so: an MBR carries it too, and telling those two apart
 * is exactly the decision being made here. */
static int looks_like_bpb(const uint8_t *b)
{
    uint32_t bps = le16(b + 11), spc = b[13];
    return (b[0] == 0xEBu || b[0] == 0xE9u)
        && bps == 512u
        && spc != 0u && (spc & (spc - 1u)) == 0u
        && le16(b + 14) != 0u               /* reserved sectors */
        && b[16] != 0u;                     /* number of FATs   */
}

int fat_mount(void)
{
    g_mounted = 0;
    g_sec_lba = g_fatsec_lba = 0xFFFFFFFFu;

    if (sd_init() != SD_OK) {
        return FAT_ERR_SD;
    }
    if (read_block(0, g_sec) != FAT_OK) {
        return FAT_ERR_SD;
    }
    if (g_sec[510] != 0x55u || g_sec[511] != 0xAAu) {
        return FAT_ERR_NOFS;
    }

    g_part_lba = 0;
    if (!looks_like_bpb(g_sec)) {
        /* An MBR: take the first partition whose type is a FAT one. */
        int found = 0;
        for (uint32_t i = 0; i < 4u && !found; i++) {
            const uint8_t *pe = g_sec + 446u + 16u * i;
            uint8_t t = pe[4];
            if (t == 0x04u || t == 0x06u || t == 0x0Eu ||     /* FAT16 */
                t == 0x0Bu || t == 0x0Cu) {                   /* FAT32 */
                g_part_lba = le32(pe + 8);
                found = 1;
            }
        }
        if (!found) {
            return FAT_ERR_NOFS;
        }
        if (read_block(g_part_lba, g_sec) != FAT_OK) {
            return FAT_ERR_SD;
        }
        if (!looks_like_bpb(g_sec)) {
            return FAT_ERR_NOFS;
        }
    }

    const uint8_t *b = g_sec;
    uint32_t reserved  = le16(b + 14);
    uint32_t nfats     = b[16];
    uint32_t root_ents = le16(b + 17);
    uint32_t total     = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint32_t fatsz     = le16(b + 22) ? le16(b + 22) : le32(b + 36);

    g_spc          = b[13];
    g_fat_sectors  = fatsz;
    g_fat_lba      = g_part_lba + reserved;
    g_root_lba     = g_fat_lba + nfats * fatsz;
    g_root_sectors = (root_ents * 32u + 511u) / 512u;
    g_data_lba     = g_root_lba + g_root_sectors;

    uint32_t data_sectors = total - (reserved + nfats * fatsz + g_root_sectors);
    g_clusters = data_sectors / g_spc;

    /* fatgen103: the type is the cluster count and nothing else. */
    if (g_clusters < 4085u) {
        return FAT_ERR_TYPE;                        /* FAT12 */
    }
    g_fat32 = (g_clusters >= 65525u);
    if (g_fat32) {
        if (g_root_sectors != 0u) {
            return FAT_ERR_TYPE;
        }
        g_root_cluster = le32(b + 44);
    } else {
        if (g_root_sectors == 0u) {
            return FAT_ERR_TYPE;
        }
        g_root_cluster = 0;
    }
    g_mounted = 1;
    return FAT_OK;
}

static uint32_t clus_lba(uint32_t c)
{
    return g_data_lba + (c - 2u) * g_spc;
}

static int is_eoc(uint32_t v)
{
    return g_fat32 ? (v >= 0x0FFFFFF8u) : (v >= 0xFFF8u);
}

/* The FAT entry for cluster c: the next cluster, or an end marker. Returns 0
 * for a read failure, which is also "free" -- both break a chain, and the
 * caller reports FAT_ERR_CHAIN either way. */
static uint32_t fat_next(uint32_t c)
{
    uint32_t off = c * (g_fat32 ? 4u : 2u);
    uint32_t lba = g_fat_lba + off / 512u;
    if (g_fatsec_lba != lba) {
        g_fatsec_lba = 0xFFFFFFFFu;
        if (read_block(lba, g_fatsec) != FAT_OK) {
            return 0;
        }
        g_fatsec_lba = lba;
    }
    off %= 512u;
    return g_fat32 ? (le32(g_fatsec + off) & 0x0FFFFFFFu) : le16(g_fatsec + off);
}

static int valid_cluster(uint32_t c)
{
    return c >= 2u && c < g_clusters + 2u;
}

/* ---- directories ----------------------------------------------------------- */

static uint8_t sfn_checksum(const uint8_t *n)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) {
        s = (uint8_t)(((s & 1u) ? 0x80u : 0u) + (s >> 1) + n[i]);
    }
    return s;
}

static void dir_start(fat_dir_t *d, uint32_t cluster)
{
    d->cluster   = cluster;
    d->sector    = 0;
    d->entry     = 0;
    d->done      = 0;
    d->lfn_valid = 0;
}

/* The block the iterator is on, or 0 once it has run off the end. */
static int dir_lba(fat_dir_t *d, uint32_t *lba)
{
    if (d->cluster == 0u) {
        if (d->sector >= g_root_sectors) {
            return 0;
        }
        *lba = g_root_lba + d->sector;
        return 1;
    }
    if (d->sector >= g_spc) {
        uint32_t n = fat_next(d->cluster);
        if (is_eoc(n) || !valid_cluster(n)) {
            return 0;
        }
        d->cluster = n;
        d->sector  = 0;
    }
    *lba = clus_lba(d->cluster) + d->sector;
    return 1;
}

static void lfn_put(char *dst, uint32_t pos, uint16_t ch, fat_dir_t *d)
{
    if (ch == 0x0000u || ch == 0xFFFFu) {
        return;                                     /* terminator / padding */
    }
    if (pos >= FAT_NAME_MAX - 1u) {
        d->lfn_valid = 2;                           /* 2 = truncated */
        return;
    }
    dst[pos] = (ch < 0x80u) ? (char)ch : '?';
}

int fat_dir_next(fat_dir_t *d, fat_dirent_t *e)
{
    if (!g_mounted) {
        return FAT_ERR_MOUNT;
    }
    while (!d->done) {
        uint32_t lba;
        if (!dir_lba(d, &lba)) {
            d->done = 1;
            break;
        }
        if (load_sec(lba) != FAT_OK) {
            return FAT_ERR_SD;
        }
        const uint8_t *ent = g_sec + d->entry * 32u;
        if (++d->entry == 16u) {
            d->entry = 0;
            d->sector++;
        }

        if (ent[0] == 0x00u) {                      /* end of directory */
            d->done = 1;
            break;
        }
        if (ent[0] == 0xE5u) {                      /* deleted */
            d->lfn_valid = 0;
            continue;
        }
        if ((ent[11] & 0x3Fu) == 0x0Fu) {           /* a long-name fragment */
            uint32_t ord = ent[0] & 0x3Fu;
            if (ent[0] & 0x40u) {                   /* the LAST fragment comes first */
                for (uint32_t i = 0; i < FAT_NAME_MAX; i++) {
                    d->lfn[i] = 0;
                }
                d->lfn_valid = 1;
                d->lfn_sum   = ent[13];
            } else if (!d->lfn_valid || ent[13] != d->lfn_sum) {
                d->lfn_valid = 0;
                continue;
            }
            if (ord == 0u) {
                d->lfn_valid = 0;
                continue;
            }
            uint32_t base = (ord - 1u) * 13u;
            static const uint8_t at[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
            for (uint32_t i = 0; i < 13u; i++) {
                lfn_put(d->lfn, base + i, le16(ent + at[i]), d);
            }
            continue;
        }
        if (ent[11] & 0x08u) {                      /* volume label */
            d->lfn_valid = 0;
            continue;
        }

        /* A short entry: the file itself. */
        e->attr      = ent[11];
        e->size      = le32(ent + 28);
        e->cluster   = ((uint32_t)le16(ent + 20) << 16) | le16(ent + 26);
        e->truncated = 0;
        if (!g_fat32) {
            e->cluster &= 0xFFFFu;
        }

        if (d->lfn_valid && sfn_checksum(ent) == d->lfn_sum) {
            e->truncated = (d->lfn_valid == 2);
            uint32_t i = 0;
            for (; i < FAT_NAME_MAX - 1u && d->lfn[i]; i++) {
                e->name[i] = d->lfn[i];
            }
            e->name[i] = 0;
        } else {
            /* "SONG    MP3" -> "SONG.MP3". 0x05 stands in for a leading 0xE5. */
            uint32_t k = 0;
            for (uint32_t i = 0; i < 8u && ent[i] != ' '; i++) {
                e->name[k++] = (char)((i == 0u && ent[0] == 0x05u) ? 0xE5u : ent[i]);
            }
            if (ent[8] != ' ') {
                e->name[k++] = '.';
                for (uint32_t i = 8; i < 11u && ent[i] != ' '; i++) {
                    e->name[k++] = (char)ent[i];
                }
            }
            e->name[k] = 0;
        }
        d->lfn_valid = 0;
        return 1;
    }
    return 0;
}

/* Also the 8.3 form, so "SONG~1.MP3" finds a file whose long name is known. */
static int name_eq(const char *a, const char *b, uint32_t blen)
{
    /* "night*" matches by prefix: these names are long, bracketed and full of
     * spaces, and typing them whole over a serial line is how typos happen. */
    int prefix = (blen > 0u && b[blen - 1u] == '*');
    if (prefix) {
        blen--;
    }
    uint32_t i = 0;
    for (; i < blen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') { x = (char)(x - 32); }
        if (y >= 'a' && y <= 'z') { y = (char)(y - 32); }
        if (x != y || !x) {
            return 0;
        }
    }
    return prefix || a[i] == 0;
}

/* Walks `path` from the root. On success *e describes the last component; a
 * path of "" or "/" yields a synthetic root directory entry. */
static int lookup(const char *path, fat_dirent_t *e)
{
    if (!g_mounted) {
        return FAT_ERR_MOUNT;
    }
    e->attr = FAT_ATTR_DIR;
    e->cluster = g_root_cluster;
    e->size = 0;
    e->name[0] = '/';
    e->name[1] = 0;

    while (*path == '/') { path++; }
    while (*path) {
        const char *end = path;
        while (*end && *end != '/') { end++; }
        uint32_t len = (uint32_t)(end - path);

        if (!(e->attr & FAT_ATTR_DIR)) {
            return FAT_ERR_NOTDIR;
        }
        static fat_dir_t d;                         /* ~80 B off the stack */
        dir_start(&d, e->cluster);
        int rc, found = 0;
        while ((rc = fat_dir_next(&d, e)) == 1) {
            if (name_eq(e->name, path, len)) {
                found = 1;
                break;
            }
        }
        if (rc < 0) {
            return rc;
        }
        if (!found) {
            return FAT_ERR_NOENT;
        }
        path = end;
        while (*path == '/') { path++; }
    }
    return FAT_OK;
}

int fat_dir_open(fat_dir_t *d, const char *path)
{
    fat_dirent_t e;
    int rc = lookup(path, &e);
    if (rc) {
        return rc;
    }
    if (!(e.attr & FAT_ATTR_DIR)) {
        return FAT_ERR_NOTDIR;
    }
    /* A ".." entry pointing at the root stores cluster 0 on both types. */
    dir_start(d, e.cluster ? e.cluster : g_root_cluster);
    return FAT_OK;
}

int fat_open(fat_file_t *f, const char *path)
{
    fat_dirent_t e;
    int rc = lookup(path, &e);
    if (rc) {
        return rc;
    }
    if (e.attr & FAT_ATTR_DIR) {
        return FAT_ERR_ISDIR;
    }
    f->first   = e.cluster;
    f->size    = e.size;
    f->pos     = 0;
    f->cluster = e.cluster;
    return FAT_OK;
}

int fat_seek(fat_file_t *f, uint32_t pos)
{
    if (pos > f->size) {
        pos = f->size;
    }
    /* The convention fat_read() keeps: a position exactly on a cluster
     * boundary belongs to the cluster that ENDS there, and the next read
     * follows the chain. So (pos-1)/cbytes hops, not pos/cbytes -- which also
     * means seeking to the end of a file never steps past its last cluster. */
    uint32_t cbytes = g_spc * 512u;
    uint32_t c = f->first;
    for (uint32_t n = pos ? (pos - 1u) / cbytes : 0u; n > 0u; n--) {
        c = fat_next(c);
        if (!valid_cluster(c)) {
            return FAT_ERR_CHAIN;
        }
    }
    f->cluster = c;
    f->pos     = pos;
    return FAT_OK;
}

int32_t fat_read(fat_file_t *f, void *buf, uint32_t n)
{
    if (!g_mounted) {
        return FAT_ERR_MOUNT;
    }
    uint8_t *out = (uint8_t *)buf;
    uint32_t cbytes = g_spc * 512u;
    uint32_t got = 0;

    if (n > f->size - f->pos) {
        n = f->size - f->pos;
    }
    while (got < n) {
        /* Crossing into a new cluster: follow the chain. pos is at a cluster
         * boundary and not at 0, and the cluster still names the old one. */
        if (f->pos != 0u && f->pos % cbytes == 0u && f->cluster != 0u) {
            uint32_t nx = fat_next(f->cluster);
            if (!valid_cluster(nx)) {
                return got ? (int32_t)got : FAT_ERR_CHAIN;
            }
            f->cluster = nx;
            g_read_hops++;
        }
        if (!valid_cluster(f->cluster)) {
            return got ? (int32_t)got : FAT_ERR_CHAIN;
        }
        uint32_t in_c = f->pos % cbytes;
        uint32_t lba  = clus_lba(f->cluster) + in_c / 512u;
        uint32_t off  = in_c % 512u;
        uint32_t k    = 512u - off;
        if (k > n - got) {
            k = n - got;
        }
        if (off == 0u && k == 512u) {
            if (read_block(lba, out + got) != FAT_OK) {    /* straight in */
                return got ? (int32_t)got : FAT_ERR_SD;
            }
        } else {
            if (load_sec(lba) != FAT_OK) {
                return got ? (int32_t)got : FAT_ERR_SD;
            }
            memcpy(out + got, g_sec + off, k);
        }
        got    += k;
        f->pos += k;
        /* Ending exactly on a boundary must not advance the cluster yet: the
         * next read does that, and a file ending there has no next cluster. */
    }
    return (int32_t)got;
}

/* ---- shell ------------------------------------------------------------------ */

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_name(const fat_dirent_t *e)
{
    uart_puts(e->name);
    if (e->attr & FAT_ATTR_DIR) {
        uart_puts("/");
    }
    if (e->truncated) {
        uart_puts(" [name truncated]");
    }
}

static int ensure_mounted(void)
{
    if (g_mounted) {
        return 1;
    }
    int rc = fat_mount();
    if (rc) {
        uart_puts("   mount failed: ");
        uart_puts(fat_strerror(rc));
        uart_puts("\n");
        return 0;
    }
    return 1;
}

static void cmd_info(void)
{
    uart_puts("   FAT");
    uart_puts(g_fat32 ? "32" : "16");
    uart_puts("  volume at block ");
    uart_put_dec(g_part_lba);
    uart_puts("  cluster ");
    uart_put_dec(g_spc * 512u);
    uart_puts(" B  clusters ");
    uart_put_dec(g_clusters);
    uart_puts("\n   fat@");
    uart_put_dec(g_fat_lba);
    uart_puts(" x");
    uart_put_dec(g_fat_sectors);
    uart_puts("  root@");
    uart_put_dec(g_root_sectors ? g_root_lba : clus_lba(g_root_cluster));
    uart_puts("  data@");
    uart_put_dec(g_data_lba);
    uart_puts("\n");
}

static void cmd_ls(const char *path)
{
    static fat_dir_t d;
    fat_dirent_t e;
    int rc = fat_dir_open(&d, path);
    if (rc) {
        uart_puts("   ");
        uart_puts(fat_strerror(rc));
        uart_puts("\n");
        return;
    }
    uint32_t n = 0;
    while ((rc = fat_dir_next(&d, &e)) == 1) {
        uart_puts("   ");
        if (e.attr & FAT_ATTR_DIR) {
            uart_puts("     <dir>  ");
        } else {
            uint32_t s = e.size, w = 1;
            for (uint32_t t = s; t >= 10u; t /= 10u) { w++; }
            for (; w < 10u; w++) { uart_putc(' '); }
            uart_put_dec(s);
            uart_puts("  ");
        }
        put_name(&e);
        uart_puts("\n");
        n++;
    }
    if (rc < 0) {
        uart_puts("   stopped: ");
        uart_puts(fat_strerror(rc));
        uart_puts("\n");
    }
    uart_puts("   ");
    uart_put_dec(n);
    uart_puts(" entries\n");
}

/* Reads a file, or its first `limit` bytes, and reports four things, each
 * answering a different doubt:
 *
 *   KB/s of the READS ALONE -- the CRC and the frame walk are excluded,
 *      because slow checking would otherwise be reported as a slow card
 *   the cluster chain length against what the size says it must be (whole
 *      reads only) -- the right byte count from the wrong clusters passes
 *      every other check
 *   the MPEG frame walk: each header gives the next one's offset, so one
 *      wrong byte or one misplaced cluster breaks it at that frame. The check
 *      on the BYTES that needs no copy of the file to compare against
 *   CRC32, comparable against the same file on a PC (7-Zip, `crc32`) */
static void cmd_read(const char *path, uint32_t limit)
{
    static fat_file_t f;
    static uint8_t buf[2048];
    int rc = fat_open(&f, path);
    if (rc) {
        uart_puts("   ");
        uart_puts(fat_strerror(rc));
        uart_puts("\n");
        return;
    }
    /* SEEK past an ID3v2 tag rather than reading through it. Tags carry the
     * cover art: the first file tested here had 776 KB of it, 45 seconds at
     * the bit-banged bus's speed, before the first note. */
    uint32_t skip = 0;
    if (limit != 0u) {
        uint8_t t[10];
        if (fat_read(&f, t, 10u) == 10) {
            skip = mp3hdr_id3_size(t);
        }
        if (fat_seek(&f, skip) != FAT_OK) {
            uart_puts("   seek past the tag failed: chain broken\n");
            return;
        }
        if (skip) {
            uart_puts("   ID3 tag of ");
            uart_put_dec(skip);
            uart_puts(" bytes skipped by seeking\n");
        }
    }
    if (limit == 0u || limit > f.size - skip) {
        limit = f.size - skip;
    }
    uint32_t crc = 0, total = 0, chk_cc = 0, blocks0 = g_blocks_read;
    uint32_t ms = 0, cc_acc = 0;
    uint8_t head[16];
    uint32_t hops0 = g_read_hops;

    /* Frame walk state. `next` is the absolute offset where the next header
     * must begin; `carry` holds the previous buffer's last 3 bytes, for a
     * header that straddles two reads. */
    uint32_t next = 0, frames = 0, walking = 1, broke_at = 0, synced = 0;
    uint32_t br_min = 0xFFFFu, br_max = 0;
    uint8_t carry[3] = { 0, 0, 0 };
    mp3hdr_t h0 = { 0, 0, 0, 0, 0, 0 };

    while (total < limit) {
        uint32_t want = limit - total;
        if (want > sizeof buf) {
            want = sizeof buf;
        }
        uint32_t t0 = xt_ccount();
        int32_t got = fat_read(&f, buf, want);
        uint32_t t1 = xt_ccount();
        if (got <= 0) {
            uart_puts("   read error: ");
            uart_puts(got < 0 ? fat_strerror(got) : "short file");
            uart_puts("\n");
            break;
        }
        cc_acc += t1 - t0;
        while (cc_acc >= CPU_HZ / 1000u) {      /* ms without 64-bit maths */
            cc_acc -= CPU_HZ / 1000u;
            ms++;
        }

        uint32_t t2 = xt_ccount();
        uint32_t base = skip + total, n = (uint32_t)got;
        if (total == 0u) {
            for (uint32_t i = 0; i < 16u; i++) {
                head[i] = (n > i) ? buf[i] : 0;
            }
            next = skip ? skip : ((n >= 10u) ? mp3hdr_id3_size(buf) : 0u);
        }
        while (walking && next + 4u <= base + n) {
            uint8_t hb[4];
            for (uint32_t i = 0; i < 4u; i++) {
                uint32_t off = next + i;
                hb[i] = (off >= base) ? buf[off - base] : carry[off - (base - 3u)];
            }
            mp3hdr_t h;
            if (mp3hdr_parse(hb, &h)) {
                if (!synced) {
                    h0 = h;
                    synced = 1;
                }
                frames++;
                if (h.bitrate < br_min) { br_min = h.bitrate; }
                if (h.bitrate > br_max) { br_max = h.bitrate; }
                next += h.length;
            } else if (!synced) {
                next++;                         /* junk before the first frame */
            } else {
                walking  = 0;
                broke_at = next;
            }
        }
        if (n >= 3u) {
            carry[0] = buf[n - 3u];
            carry[1] = buf[n - 2u];
            carry[2] = buf[n - 1u];
        }
        crc = crc32_update(crc, buf, n);
        chk_cc += xt_ccount() - t2;
        total += n;
    }

    uart_puts("   ");
    uart_put_dec(total);
    uart_puts(" of ");
    uart_put_dec(f.size);
    uart_puts(" bytes  in ");
    uart_put_dec(ms);
    uart_puts(" ms  = ");
    uart_put_dec(ms ? total / ms : 0u);         /* bytes per ms == KB/s (1000) */
    uart_puts(" KB/s   blocks=");
    uart_put_dec(g_blocks_read - blocks0);
    uart_puts("  (checks took ");
    uart_put_dec(chk_cc / (CPU_HZ / 1000u));
    uart_puts(" ms, excluded)\n");

    if (total == f.size && skip == 0u) {
        uint32_t cbytes = g_spc * 512u;
        uint32_t chain = total ? g_read_hops - hops0 + 1u : 0u;
        uint32_t want_chain = (f.size + cbytes - 1u) / cbytes;
        uart_puts("   clusters followed=");
        uart_put_dec(chain);
        uart_puts(" expected=");
        uart_put_dec(want_chain);
        uart_puts(chain == want_chain ? "  (chain agrees)\n" : "  (CHAIN DISAGREES)\n");
        uart_puts("   crc32=");
        uart_put_hex(crc);
        uart_puts("\n");
    }

    uart_puts("   first bytes:");
    for (int i = 0; i < 16; i++) {
        uart_putc(' ');
        uart_put_hex(head[i]);
    }
    uart_puts("\n");
    if (!synced) {
        uart_puts("   no MPEG Layer III frame found\n");
        return;
    }
    uart_puts("   MPEG-");
    uart_puts(h0.version == 10u ? "1" : h0.version == 20u ? "2" : "2.5");
    uart_puts(" Layer III  ");
    uart_put_dec(h0.rate);
    uart_puts(" Hz  ");
    uart_puts(h0.channels == 2u ? "stereo" : "mono");
    uart_puts("  ");
    uart_put_dec(br_min);
    if (br_max != br_min) {
        uart_puts("-");
        uart_put_dec(br_max);
        uart_puts(" kbps (VBR)");
    } else {
        uart_puts(" kbps");
    }
    uart_puts("\n   frames walked=");
    uart_put_dec(frames);
    uart_puts("  = ");
    uart_put_dec(frames * h0.samples / h0.rate);
    uart_puts(" s of audio");
    if (walking) {
        uart_puts("  -- every header where the previous one said\n");
    } else {
        uart_puts("\n   walk BROKE at byte ");
        uart_put_dec(broke_at);
        uart_puts(f.size - broke_at == 128u ? "  (exactly an ID3v1 tag from the end: fine)\n"
                                           : "  <- not a header where one must be\n");
    }
}

static uint32_t parse_u32(const char *s, const char **end)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s++ - '0');
    }
    *end = s;
    return v;
}

void fat_shell(char *arg)
{
    char *sub = arg;
    char *rest = arg;
    while (*rest && *rest != ' ') { rest++; }
    if (*rest) {
        *rest++ = 0;
        while (*rest == ' ') { rest++; }
    }

    if (!*sub || (sub[0] == 'm' && sub[1] == 'o')) {        /* "" or "mount" */
        g_mounted = 0;
        if (ensure_mounted()) {
            cmd_info();
        }
    } else if (sub[0] == 'l' && sub[1] == 's' && !sub[2]) {
        if (ensure_mounted()) {
            cmd_ls(rest);
        }
    } else if (sub[0] == 'c' && sub[1] == 'a' && sub[2] == 't' && !sub[3]) {
        if (ensure_mounted()) {
            cmd_read(rest, 0);
        }
    } else if (sub[0] == 'h' && sub[1] == 'e' && sub[2] == 'a' && sub[3] == 'd' && !sub[4]) {
        /* "fat head <KB> <file>": the number first, because file names here
         * contain spaces and brackets and a trailing number would be ambiguous. */
        const char *p;
        uint32_t kb = parse_u32(rest, &p);
        while (*p == ' ') { p++; }
        if (!kb || !*p) {
            uart_puts("   fat head <KB> <file>\n");
        } else if (ensure_mounted()) {
            cmd_read(p, kb * 1024u);
        }
    } else {
        uart_puts("   fat               mount and describe the card\n"
                  "   fat ls [dir]      list a directory\n"
                  "   fat cat <file>    read it all: KB/s, chain, frame walk, crc32\n"
                  "   fat head <KB> <f> the first KB only, same report\n"
                  "   a name ending in * matches by prefix: fat cat night*\n");
    }
}
