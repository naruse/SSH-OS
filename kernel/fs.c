/* The SSH OS disk: where settings and other small files are kept.
 *
 * It is a FAT16 volume named SSH OS on a SCSI disk (on the real machine, an
 * image on the BlueSCSI's card). FAT16 because every other computer can
 * read it: the image opens on a modern Mac, where the files can be looked
 * at and edited. The volume may fill the whole disk or be the first
 * partition of a disk with a PC partition table.
 *
 * Kept simple on purpose: files live in the root folder only, have short
 * names (8 characters, a dot, 3), and are read and written whole.
 *
 * Two rules protect what is on the disks:
 *
 *   - Only a volume named SSH OS is ever written to, and fs_format() only
 *     touches a disk that is blank, an SSH OS scratch disk, or an SSH OS
 *     volume already. A disk holding anything else is left alone.
 *   - A file is replaced by writing the new contents to free space first
 *     and changing the folder entry last, in one block. Switching the
 *     machine off in the middle leaves the old file or the new one, never
 *     half of each. (It can leave some space marked as used that is not,
 *     and the spare copy of the table a block behind the first; any disk
 *     checker puts both right.)
 */
#include "kernel.h"

#define ROOT_ENTRIES    512
#define ENTRY           32
#define END_OF_CHAIN    0xFFFF
#define MIN_CLUSTERS    4085UL          /* fewer would make it FAT12 */
#define MAX_CLUSTERS    65524UL

static struct {
    u8  mounted, id;
    u8  per_cluster;                    /* blocks in a cluster */
    u32 start;                          /* first block of the volume on the disk */
    u32 fat, fat_blocks;                /* first copy of the table; there are copies of it */
    u8  fats;
    u32 root, root_entries;
    u32 data;                           /* block of cluster 2 */
    u32 clusters;
    u32 hint;                           /* where to look for free space next */
} vol;

static u8 fatbuf[512], secbuf[512];
static u32 fat_block = 0xFFFFFFFF;      /* which block of the table fatbuf holds */
static u8 fat_dirty;

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static int rd(u32 block, void *buf)
{
    return scsi_read(vol.id, vol.start + block, 1, buf) ? FS_ERR_DISK : 0;
}

static int wr(u32 block, const void *buf)
{
    return scsi_write(vol.id, vol.start + block, 1, buf) ? FS_ERR_DISK : 0;
}

const char *fs_strerror(int err)
{
    switch (err) {
    case 0:                 return "no error";
    case FS_ERR_NO_DISK:    return "there is no SSH OS disk";
    case FS_ERR_DISK:       return "the disk did not answer or reported an error";
    case FS_ERR_NOT_FOUND:  return "there is no file of that name";
    case FS_ERR_FULL:       return "the disk is full";
    case FS_ERR_NAME:       return "names are up to 8 letters or digits, a dot, and up to 3";
    case FS_ERR_DAMAGED:    return "the disk's contents make no sense";
    case FS_ERR_TOO_BIG:    return "the file is too big";
    case FS_ERR_IN_USE:     return "that disk holds something else";
    case FS_ERR_TOO_SMALL:  return "the disk is too small (it needs about 3 MB) or has odd blocks";
    default:                return "unknown error";
    }
}

/* -------------------------------------------------- the table of chains -- */

static int fat_flush(void)
{
    u32 i;
    int err;

    if (!fat_dirty)
        return 0;
    for (i = 0; i < vol.fats; i++)
        if ((err = wr(vol.fat + i * vol.fat_blocks + fat_block, fatbuf)) != 0)
            return err;
    fat_dirty = 0;
    return 0;
}

static int fat_load(u32 cluster)
{
    u32 block = cluster / 256;
    int err;

    if (block == fat_block)
        return 0;
    if ((err = fat_flush()) != 0)
        return err;
    fat_block = 0xFFFFFFFF;
    if ((err = rd(vol.fat + block, fatbuf)) != 0)
        return err;
    fat_block = block;
    return 0;
}

static int fat_get(u32 cluster, u32 *value)
{
    int err = fat_load(cluster);

    if (err == 0)
        *value = le16(fatbuf + (cluster % 256) * 2);
    return err;
}

static int fat_set(u32 cluster, u32 value)
{
    int err = fat_load(cluster);

    if (err == 0) {
        put16(fatbuf + (cluster % 256) * 2, value);
        fat_dirty = 1;
    }
    return err;
}

static int valid(u32 cluster)
{
    return cluster >= 2 && cluster < vol.clusters + 2;
}

/* Give back a chain of clusters. */
static int chain_free(u32 cluster)
{
    u32 next, n = 0;
    int err;

    while (valid(cluster) && n++ < vol.clusters) {
        if ((err = fat_get(cluster, &next)) != 0 || (err = fat_set(cluster, 0)) != 0)
            return err;
        cluster = next;
    }
    return fat_flush();
}

/* A cluster nobody is using, marked as the end of a chain. */
static int cluster_take(u32 *out)
{
    u32 n, c, value;
    int err;

    for (n = 0, c = vol.hint; n < vol.clusters; n++, c++) {
        if (!valid(c))
            c = 2;
        if ((err = fat_get(c, &value)) != 0)
            return err;
        if (value == 0) {
            vol.hint = c + 1;
            *out = c;
            return fat_set(c, END_OF_CHAIN);
        }
    }
    return FS_ERR_FULL;
}

/* ------------------------------------------------------------ the folder -- */

/* "settings.txt" as the 11 characters of a folder entry: SETTINGSTXT. */
static int make_name(u8 *out, const char *name)
{
    int n = 0, limit = 8, dot = 0;
    u8 c;

    memset(out, ' ', 11);
    for (; (c = (u8)*name) != 0; name++) {
        if (c == '.') {
            if (dot || n == 0)
                return FS_ERR_NAME;
            dot = 1;
            n = 8;
            limit = 11;
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c = (u8)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ||
            n >= limit)
            return FS_ERR_NAME;
        out[n++] = c;
    }
    return out[0] == ' ' ? FS_ERR_NAME : 0;
}

static void show_name(char *out, const u8 *e)
{
    int i, n = 0;

    for (i = 0; i < 8 && e[i] != ' '; i++)
        out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (char)e[i];
    }
    out[n] = 0;
}

/* An entry that is a plain file: not erased, not a folder, not the volume's
 * name, not part of a long name. */
static int is_file(const u8 *e)
{
    return e[0] != 0xE5 && e[0] != 0 && !(e[11] & 0x18);
}

/* Look through the folder, leaving the block that holds the entry in
 * secbuf. With name NULL, finds the first unused entry instead. */
static int dir_find(const u8 *name, u32 *block, u32 *offset)
{
    u32 b, o;
    int err;

    for (b = 0; b < vol.root_entries / 16; b++) {
        if ((err = rd(vol.root + b, secbuf)) != 0)
            return err;
        for (o = 0; o < 512; o += ENTRY) {
            const u8 *e = secbuf + o;

            if (name ? (is_file(e) && memcmp(e, name, 11) == 0) : (e[0] == 0 || e[0] == 0xE5)) {
                *block = b;
                *offset = o;
                return 0;
            }
            if (e[0] == 0 && name)              /* nothing is in use after this one */
                return FS_ERR_NOT_FOUND;
        }
    }
    return name ? FS_ERR_NOT_FOUND : FS_ERR_FULL;
}

/* Now, as the time and date fields of a folder entry, from the clock's
 * seconds since 1904. */
static void stamp(u8 *time, u8 *date)
{
    static const u8 month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    u32 secs = machine.rom_time + ticks * 100 / 6015, days = secs / 86400, year = 1904, len, m;

    secs %= 86400;
    while (days >= (len = (year % 4 == 0) ? 366 : 365)) {      /* right until 2100 */
        days -= len;
        year++;
    }
    for (m = 0; m < 11; m++) {
        len = month_days[m] + (m == 1 && year % 4 == 0);
        if (days < len)
            break;
        days -= len;
    }
    if (year < 1980) {                          /* the clock was never set */
        year = 1980;
        m = 0;
        days = 0;
        secs = 0;
    }
    put16(time, (secs / 3600 << 11) | (secs % 3600 / 60 << 5) | (secs % 60 / 2));
    put16(date, ((year - 1980) << 9) | ((m + 1) << 5) | (days + 1));
}

/* --------------------------------------------------------------- mounting -- */

/* Does this block start a FAT16 volume with 512-byte blocks? Fills in vol
 * (all but id and start) if so. */
static int read_layout(const u8 *b)
{
    u32 reserved = le16(b + 14), total = le16(b + 19), root_blocks, data_blocks;

    if (b[510] != 0x55 || b[511] != 0xAA || le16(b + 11) != 512 || b[13] == 0 ||
        (b[13] & (b[13] - 1)) || reserved == 0 || b[16] == 0 || le16(b + 22) == 0)
        return 0;
    if (total == 0)
        total = le32(b + 32);
    vol.per_cluster = b[13];
    vol.fats = b[16];
    vol.fat = reserved;
    vol.fat_blocks = le16(b + 22);
    vol.root_entries = le16(b + 17);
    root_blocks = (vol.root_entries + 15) / 16;
    vol.root = reserved + vol.fats * vol.fat_blocks;
    vol.data = vol.root + root_blocks;
    if (vol.root_entries == 0 || (vol.root_entries & 15) || total <= vol.data)
        return 0;
    data_blocks = total - vol.data;
    vol.clusters = data_blocks / vol.per_cluster;
    if (vol.clusters < MIN_CLUSTERS || vol.clusters > MAX_CLUSTERS ||
        vol.fat_blocks < (vol.clusters + 2 + 255) / 256)
        return 0;
    vol.hint = 2;
    return 1;
}

static int named_ours(const u8 *b)
{
    /* The second is what these disks were named before the system was. */
    static const char want[2][12] = {"SSH OS     ", "JUANOS     "};
    int i, k;

    if (b[38] != 0x29)                  /* no name in this block */
        return 0;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 11; i++) {
            u8 c = b[43 + i];

            if (c >= 'a' && c <= 'z')
                c = (u8)(c - 32);
            if (c != (u8)want[k][i])
                break;
        }
        if (i == 11)
            return 1;
    }
    return 0;
}

/* Is the disk at this ID an SSH OS disk? Leaves vol filled in if so. */
static int try_disk(u8 id)
{
    u8 inq[36];
    u32 first;

    if (scsi_inquiry(id, inq) != 0 || (inq[0] & 0x1F) != 0)
        return 0;
    vol.id = id;
    vol.start = 0;
    if (rd(0, secbuf) != 0)
        return 0;
    if (!read_layout(secbuf)) {
        /* Perhaps a PC partition table, with a FAT16 partition first. */
        u8 kind = secbuf[446 + 4];

        first = le32(secbuf + 446 + 8);
        if (secbuf[510] != 0x55 || secbuf[511] != 0xAA ||
            (kind != 0x04 && kind != 0x06 && kind != 0x0E) || first == 0)
            return 0;
        vol.start = first;
        if (rd(0, secbuf) != 0 || !read_layout(secbuf))
            return 0;
    }
    return named_ours(secbuf);
}

int fs_mount(void)
{
    const char *where = config_get("disk");
    u8 id;

    vol.mounted = 0;
    fat_block = 0xFFFFFFFF;
    fat_dirty = 0;
    /* The setting "disk" names the SCSI ID and saves looking at them all. */
    if (where && where[0] >= '0' && where[0] <= '6' && !where[1]) {
        vol.mounted = (u8)try_disk((u8)(where[0] - '0'));
    } else {
        for (id = 0; id < 7 && !vol.mounted; id++)
            if (id != (u8)eth_adapter_id())
                vol.mounted = (u8)try_disk(id);
    }
    return vol.mounted ? 0 : FS_ERR_NO_DISK;
}

int fs_mounted(void)
{
    return vol.mounted;
}

int fs_disk_id(void)
{
    return vol.mounted ? vol.id : -1;
}

/* ------------------------------------------------------------------ files -- */

int fs_read(const char *name, void *buf, u32 max, u32 *len)
{
    u8 want[11], *out = buf;
    u32 block, offset, cluster, size, left, i, n = 0;
    int err;

    *len = 0;
    if (!vol.mounted)
        return FS_ERR_NO_DISK;
    if ((err = make_name(want, name)) != 0 || (err = dir_find(want, &block, &offset)) != 0)
        return err;
    cluster = le16(secbuf + offset + 26);
    size = le32(secbuf + offset + 28);
    if (size > max)
        return FS_ERR_TOO_BIG;
    for (left = size; left; ) {
        if (!valid(cluster) || n++ >= vol.clusters)
            return FS_ERR_DAMAGED;
        for (i = 0; i < vol.per_cluster && left; i++) {
            u32 part = left < 512 ? left : 512;

            if ((err = rd(vol.data + (cluster - 2) * vol.per_cluster + i, secbuf)) != 0)
                return err;
            memcpy(out, secbuf, part);
            out += part;
            left -= part;
        }
        if ((err = fat_get(cluster, &cluster)) != 0)
            return err;
    }
    *len = size;
    return 0;
}

int fs_write(const char *name, const void *buf, u32 len)
{
    u8 want[11];
    const u8 *in = buf;
    u32 block, offset, first = 0, last = 0, cluster, old = 0, left, i;
    int err, exists;

    if (!vol.mounted)
        return FS_ERR_NO_DISK;
    if ((err = make_name(want, name)) != 0)
        return err;
    err = dir_find(want, &block, &offset);
    if (err && err != FS_ERR_NOT_FOUND)
        return err;
    exists = err == 0;
    if (!exists && (err = dir_find(NULL, &block, &offset)) != 0)
        return err;

    /* The new contents go to free space, linked up as they are written. */
    for (left = len; left && err == 0; ) {
        if ((err = cluster_take(&cluster)) != 0)
            break;
        if (last)
            err = fat_set(last, cluster);
        else
            first = cluster;
        last = cluster;
        for (i = 0; i < vol.per_cluster && left && err == 0; i++) {
            u32 part = left < 512 ? left : 512;

            memset(secbuf, 0, 512);
            memcpy(secbuf, in, part);
            err = wr(vol.data + (cluster - 2) * vol.per_cluster + i, secbuf);
            in += part;
            left -= part;
        }
    }
    if (err == 0)
        err = fat_flush();
    if (err) {
        if (first)
            chain_free(first);
        else
            fat_flush();
        return err;
    }

    /* Now the folder entry, in one block: from here on the new file is the file. */
    if ((err = rd(vol.root + block, secbuf)) != 0) {
        chain_free(first);
        return err;
    }
    if (exists) {
        old = le16(secbuf + offset + 26);
    } else {
        memset(secbuf + offset, 0, ENTRY);
        memcpy(secbuf + offset, want, 11);
        secbuf[offset + 11] = 0x20;             /* "changed since the last backup" */
        stamp(secbuf + offset + 14, secbuf + offset + 16);      /* when it was made */
    }
    stamp(secbuf + offset + 22, secbuf + offset + 24);          /* and last changed */
    put16(secbuf + offset + 18, le16(secbuf + offset + 24));    /* last used: the date */
    put16(secbuf + offset + 20, 0);
    put16(secbuf + offset + 26, first);
    put32(secbuf + offset + 28, len);
    if ((err = wr(vol.root + block, secbuf)) != 0) {
        chain_free(first);
        return err;
    }
    return old ? chain_free(old) : 0;
}

int fs_delete(const char *name)
{
    u8 want[11];
    u32 block, offset, cluster;
    int err;

    if (!vol.mounted)
        return FS_ERR_NO_DISK;
    if ((err = make_name(want, name)) != 0 || (err = dir_find(want, &block, &offset)) != 0)
        return err;
    cluster = le16(secbuf + offset + 26);
    secbuf[offset] = 0xE5;
    if ((err = wr(vol.root + block, secbuf)) != 0)
        return err;
    return chain_free(cluster);
}

/* The index-th file: its name (up to 12 characters) and size. FS_ERR_NOT_FOUND
 * after the last one. */
int fs_list(int index, char *name, u32 *size)
{
    u32 b, o;
    int err;

    if (!vol.mounted)
        return FS_ERR_NO_DISK;
    for (b = 0; b < vol.root_entries / 16; b++) {
        if ((err = rd(vol.root + b, secbuf)) != 0)
            return err;
        for (o = 0; o < 512; o += ENTRY) {
            if (secbuf[o] == 0)
                return FS_ERR_NOT_FOUND;
            if (is_file(secbuf + o) && index-- == 0) {
                show_name(name, secbuf + o);
                *size = le32(secbuf + o + 28);
                return 0;
            }
        }
    }
    return FS_ERR_NOT_FOUND;
}

/* Room on the volume: all of it and what is free, in KB. */
int fs_space(u32 *total_kb, u32 *free_kb)
{
    u32 c, value, free = 0;
    int err;

    if (!vol.mounted)
        return FS_ERR_NO_DISK;
    for (c = 2; c < vol.clusters + 2; c++) {
        if ((err = fat_get(c, &value)) != 0)
            return err;
        free += value == 0;
    }
    *total_kb = vol.clusters * vol.per_cluster / 2;
    *free_kb = free * vol.per_cluster / 2;
    return 0;
}

/* ------------------------------------------------------------- formatting -- */

/* What is on the disk at this ID: FS_BLANK, FS_SCRATCH, FS_OURS (all of
 * which may be formatted), or FS_ERR_IN_USE or another error. */
int fs_look(u8 id)
{
    static const char scratch[] = "SSHOS-SCRATCH-DISK";
    u8 inq[36];
    u32 b, i;

    if (scsi_inquiry(id, inq) != 0 || (inq[0] & 0x1F) != 0)
        return FS_ERR_NO_DISK;
    if (try_disk(id)) {
        vol.mounted = 0;                /* try_disk filled vol in; mount again to use it */
        return FS_OURS;
    }
    vol.mounted = 0;
    if (scsi_read(id, 0, 1, secbuf) != 0)
        return FS_ERR_DISK;
    if (memcmp(secbuf, scratch, sizeof scratch - 1) == 0)
        return FS_SCRATCH;
    /* Blank means blank: a Mac disk can have nothing in its first two
     * blocks and its catalogue after them. */
    for (b = 0; b < 64; b++) {
        if (b && scsi_read(id, b, 1, secbuf) != 0)
            return FS_ERR_DISK;
        for (i = 0; i < 512; i++)
            if (secbuf[i])
                return FS_ERR_IN_USE;
    }
    return FS_BLANK;
}

/* Make the disk at this ID an empty SSH OS volume. Everything on it is lost;
 * it refuses unless fs_look() says the disk may be used. */
int fs_format(u8 id)
{
    u32 blocks, size, per, fat_blocks, clusters, b, serial, root_blocks = ROOT_ENTRIES / 16;
    int kind = fs_look(id), err;

    if (kind < 0)
        return kind;
    if (scsi_capacity(id, &blocks, &size) != 0)
        return FS_ERR_DISK;
    if (size != 512)
        return FS_ERR_TOO_SMALL;
    /* Clusters as small as will do, on at most the first 2 GB. Each
     * cluster takes its own blocks and two bytes in each of the two tables:
     * a 128th of a block. */
    if (blocks > 1 + 2 * 256 + root_blocks + MAX_CLUSTERS * 64)
        blocks = 1 + 2 * 256 + root_blocks + MAX_CLUSTERS * 64;
    if (blocks < 64)
        return FS_ERR_TOO_SMALL;
    for (per = 1; ; per *= 2) {
        clusters = (blocks - 1 - root_blocks) * 128 / (per * 128 + 1);
        if (clusters <= MAX_CLUSTERS || per == 64)
            break;
    }
    if (clusters > MAX_CLUSTERS)
        clusters = MAX_CLUSTERS;
    fat_blocks = (clusters + 2 + 255) / 256;
    while (clusters && 1 + 2 * fat_blocks + root_blocks + clusters * per > blocks)
        clusters--;
    blocks = 1 + 2 * fat_blocks + root_blocks + clusters * per;
    if (clusters < MIN_CLUSTERS)
        return FS_ERR_TOO_SMALL;

    vol.mounted = 0;
    vol.id = id;
    vol.start = 0;
    fat_block = 0xFFFFFFFF;
    fat_dirty = 0;

    /* The tables and the folder first, the block that names the volume
     * last: until that is written the disk still counts as what it was. */
    memset(secbuf, 0, 512);
    for (b = 1; b < 1 + 2 * fat_blocks + ROOT_ENTRIES / 16; b++) {
        if (b == 1 + 2 * fat_blocks) {                  /* the folder: the volume's name */
            memcpy(secbuf, "SSH OS     ", 11);
            secbuf[11] = 0x08;
            stamp(secbuf + 22, secbuf + 24);
        } else if (b == 1 || b == 1 + fat_blocks) {     /* the start of each table */
            secbuf[0] = 0xF8;
            secbuf[1] = secbuf[2] = secbuf[3] = 0xFF;
        }
        if ((err = wr(b, secbuf)) != 0)
            return err;
        if (secbuf[0])
            memset(secbuf, 0, 512);
    }

    serial = machine.rom_time + ticks;
    memset(secbuf, 0, 512);
    secbuf[0] = 0xEB;                   /* what a PC expects to find first */
    secbuf[1] = 0x3C;
    secbuf[2] = 0x90;
    memcpy(secbuf + 3, "SSH OS  ", 8);
    put16(secbuf + 11, 512);
    secbuf[13] = (u8)per;
    put16(secbuf + 14, 1);
    secbuf[16] = 2;
    put16(secbuf + 17, ROOT_ENTRIES);
    if (blocks < 65536)
        put16(secbuf + 19, blocks);
    else
        put32(secbuf + 32, blocks);
    secbuf[21] = 0xF8;
    put16(secbuf + 22, fat_blocks);
    put16(secbuf + 24, 32);
    put16(secbuf + 26, 64);
    secbuf[36] = 0x80;
    secbuf[38] = 0x29;
    put32(secbuf + 39, serial);
    memcpy(secbuf + 43, "SSH OS     ", 11);
    memcpy(secbuf + 54, "FAT16   ", 8);
    secbuf[510] = 0x55;
    secbuf[511] = 0xAA;
    if ((err = wr(0, secbuf)) != 0)
        return err;
    return 0;
}
