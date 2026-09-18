/* nat-os — read-only FAT16/FAT32 on the SD card.
 *
 * sd.c reads numbered 512-byte blocks and nothing more; a card filled on a
 * desktop is a filesystem, not a list of block numbers. This is the smallest
 * layer that turns "SONG.MP3" into the blocks that hold it: find the
 * partition, parse its boot sector, walk directories, follow cluster chains.
 *
 * ---- what it handles, and what it refuses ---------------------------------
 *
 *   FAT16 and FAT32, with or without an MBR (a "superfloppy" has its boot
 *   sector at block 0). The FAT type is decided by CLUSTER COUNT, which is
 *   the only rule the specification recognises -- the "FAT16   " string in the
 *   boot sector is a label anyone can write and the spec says not to trust it.
 *
 *   Long filenames are read, because a card filled from Windows has them
 *   whether or not anyone asked, and each fragment is checked against its
 *   short entry's checksum -- an orphaned fragment left by a deleted file is
 *   discarded rather than glued onto the next name. Characters outside ASCII
 *   come back as '?'.
 *
 *   FAT12 is refused (floppies; no SD card is formatted that way), exFAT is
 *   refused (cards over 32 GB), and nothing is ever written.
 *
 * ---- concurrency -----------------------------------------------------------
 *
 * One mutex around every public entry point (fat.c, "the lock"), added when
 * the player became a second caller. sd.c beneath it is still unlocked;
 * callers that bypass FAT use fat_lock()/fat_unlock().
 */

#ifndef NATOS_FAT_H
#define NATOS_FAT_H

#include <stdint.h>

#define FAT_NAME_MAX 64u            /* longer names are truncated, and say so */

typedef enum {
    FAT_OK        =  0,
    FAT_ERR_SD    = -1,             /* sd_init or a block read failed        */
    FAT_ERR_NOFS  = -2,             /* no FAT boot sector where one belongs  */
    FAT_ERR_TYPE  = -3,             /* FAT12, exFAT, or a geometry we refuse */
    FAT_ERR_NOENT = -4,             /* path component not found              */
    FAT_ERR_NOTDIR = -5,
    FAT_ERR_ISDIR = -6,
    FAT_ERR_CHAIN = -7,             /* cluster chain ends early / hits free  */
    FAT_ERR_MOUNT = -8,             /* nothing mounted                       */
} fat_err_t;

#define FAT_ATTR_DIR    0x10u

typedef struct {
    char     name[FAT_NAME_MAX];
    uint32_t size;
    uint32_t cluster;
    uint8_t  attr;
    uint8_t  truncated;             /* name did not fit FAT_NAME_MAX */
} fat_dirent_t;

typedef struct {
    uint32_t cluster;               /* 0 = FAT16 fixed root region */
    uint32_t sector;                /* within the cluster, or within the root */
    uint32_t entry;                 /* within the sector */
    int      done;
    /* Long-name assembly across entries. */
    char     lfn[FAT_NAME_MAX];
    uint8_t  lfn_sum;
    uint8_t  lfn_valid;
} fat_dir_t;

typedef struct {
    uint32_t first;                 /* first cluster */
    uint32_t size;
    uint32_t pos;
    uint32_t cluster;               /* cluster holding `pos` */
} fat_file_t;

/* Initialises the card and mounts the first FAT partition. */
int fat_mount(void);
int fat_mounted(void);

/* Iterates a directory. `path` of "" or "/" is the root. */
int fat_dir_open(fat_dir_t *d, const char *path);
int fat_dir_next(fat_dir_t *d, fat_dirent_t *e);   /* 1 entry, 0 end, <0 error */

/* Opens a file by path, components matched case-insensitively against the
 * long name and the 8.3 name both. */
int fat_open(fat_file_t *f, const char *path);

/* Sequential read. Returns bytes read (0 at end of file) or a negative
 * fat_err_t. Whole sectors go straight into `buf`. */
int32_t fat_read(fat_file_t *f, void *buf, uint32_t n);

/* Moves the read position, walking the chain from the start. */
int fat_seek(fat_file_t *f, uint32_t pos);

const char *fat_strerror(int rc);

/* The lock every function above takes. Exported for callers that go to sd.c
 * directly and must not do so while the player is reading. Recursive. */
void fat_lock(void);
void fat_unlock(void);

void fat_shell(char *arg);

#endif /* NATOS_FAT_H */
