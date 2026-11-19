/* The SSH OS file system (kernel/fs.c) built for this Mac, with a disk
 * image file in place of the SCSI disk, so tests/test_fs.py can drive it in
 * milliseconds and under the address and undefined-behaviour checkers.
 *
 *     fs_host IMAGE
 *
 * then one command a line on standard input, one answer a line on standard
 * output ("ok ..." or "err N"):
 *
 *     mount | look | format | space | list
 *     write NAME HOSTFILE | read NAME HOSTFILE | delete NAME
 *     die N       the Nth block written from now on is not written, nor any
 *                 after it: the machine was switched off
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host_types.h"
#include "../kernel/kernel.h"

struct machine machine = {.rom_time = 3878000000u};     /* some day in 2026 */
volatile u32 ticks;
static int image = -1;
static long die_after = -1;
static u8 filebuf[1 << 20];

const char *config_get(const char *name)
{
    (void)name;
    return NULL;
}

int eth_adapter_id(void)
{
    return -1;
}

int scsi_inquiry(u8 id, u8 *buf36)
{
    if (id != 0)
        return SCSI_ERR_NO_DEVICE;
    memset(buf36, 0, 36);
    return 0;
}

int scsi_capacity(u8 id, u32 *blocks, u32 *block_size)
{
    struct stat st;

    if (id != 0 || fstat(image, &st) != 0)
        return SCSI_ERR_NO_DEVICE;
    *blocks = (u32)(st.st_size / 512);
    *block_size = 512;
    return 0;
}

int scsi_read(u8 id, u32 block, u16 count, void *buf)
{
    if (id != 0)
        return SCSI_ERR_NO_DEVICE;
    return pread(image, buf, count * 512, (off_t)block * 512) == count * 512 ? 0 : SCSI_ERR_CHECK;
}

int scsi_write(u8 id, u32 block, u16 count, const void *buf)
{
    if (id != 0)
        return SCSI_ERR_NO_DEVICE;
    if (die_after == 0)
        return SCSI_ERR_TIMEOUT;
    if (die_after > 0)
        die_after--;
    return pwrite(image, buf, count * 512, (off_t)block * 512) == count * 512 ? 0 : SCSI_ERR_CHECK;
}

static void answer(int r)
{
    if (r < 0)
        printf("err %d\n", r);
    else
        printf("ok %d\n", r);
}

int main(int argc, char **argv)
{
    char line[600], name[300], path[300];
    u32 len, total, free_kb;
    int r, i;
    FILE *f;

    if (argc != 2 || (image = open(argv[1], O_RDWR)) < 0) {
        fprintf(stderr, "usage: fs_host IMAGE\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        name[0] = path[0] = 0;
        if (strncmp(line, "mount", 5) == 0) {
            answer(fs_mount());
        } else if (strncmp(line, "look", 4) == 0) {
            answer(fs_look(0));
        } else if (strncmp(line, "format", 6) == 0) {
            answer(fs_format(0));
        } else if (strncmp(line, "space", 5) == 0) {
            r = fs_space(&total, &free_kb);
            if (r)
                answer(r);
            else
                printf("ok %u %u\n", total, free_kb);
        } else if (strncmp(line, "list", 4) == 0) {
            for (i = 0; (r = fs_list(i, name, &len)) == 0; i++)
                printf("file %s %u\n", name, len);
            answer(r == FS_ERR_NOT_FOUND ? i : r);
        } else if (sscanf(line, "write %299s %299s", name, path) == 2) {
            if (!(f = fopen(path, "rb")))
                return 2;
            len = (u32)fread(filebuf, 1, sizeof filebuf, f);
            fclose(f);
            answer(fs_write(name, filebuf, len));
        } else if (sscanf(line, "read %299s %299s", name, path) == 2) {
            r = fs_read(name, filebuf, sizeof filebuf, &len);
            if (r == 0) {
                if (!(f = fopen(path, "wb")))
                    return 2;
                fwrite(filebuf, 1, len, f);
                fclose(f);
            }
            answer(r);
        } else if (sscanf(line, "delete %299s", name) == 1) {
            answer(fs_delete(name));
        } else if (sscanf(line, "die %ld", &die_after) == 1) {
            answer(0);
        } else {
            printf("err what\n");
        }
    }
    return 0;
}
