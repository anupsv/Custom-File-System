

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/select.h>
#include <ctype.h>
#include <time.h>
#include <assert.h>

#include "fsx600.h"

char *disk;

/* handle K/M
 */
int parseint(char *s)
{
    int n = strtol(s, &s, 0);
    if (tolower(*s) == 'k')
        return n * 1024;
    if (tolower(*s) == 'm')
        return n * 1024 * 1024;
    return n;
}

#define DIV_ROUND_UP(n, m) ((n) + (m) - 1) / (m)

/* usage: mkfs-x6 [-size #] file.img
 * If file doesn't exist, create with size '#' (K and M suffixes allowed)
 */
int main(int argc, char **argv)
{
    int i, fd = -1, size = 0;
    if (!strcmp(argv[1], "-size") && argc >= 3) {
        size = parseint(argv[2]);
        argv += 2;
        argc -= 2;
    }

    if (argc == 2) {
        fd = open(argv[1], O_WRONLY | O_CREAT, 0777);
        if (fd >= 0 && size == 0) {
            struct stat sb;
            fstat(fd, &sb);
            size = sb.st_size;
        }
    }
    if (fd < 0) {
        printf("usage: mkfs-x6 [-size #] file.img\n");
        exit(1);
    }

    if (size % FS_BLOCK_SIZE != 0)
        printf("WARNING: disk size not a multiple of block size: %d (0x%x)\n",
               size, size);
    int n_blks = size / FS_BLOCK_SIZE;
    int n_map_blks = DIV_ROUND_UP(n_blks, 8*FS_BLOCK_SIZE);
    int n_inos = n_blks / 4;
    int n_ino_map_blks = DIV_ROUND_UP(n_inos, 8*FS_BLOCK_SIZE);
    int n_ino_blks = DIV_ROUND_UP(n_inos*sizeof(struct fs_inode),
                                  FS_BLOCK_SIZE);

    disk = malloc(n_blks * FS_BLOCK_SIZE);
    memset(disk, 0, n_blks * FS_BLOCK_SIZE);

    struct fs_super *sb = (void*)disk;

    int inode_map_base = 1;
    fd_set *inode_map = (void*)(disk + inode_map_base*FS_BLOCK_SIZE);

    int block_map_base = inode_map_base + n_ino_map_blks;
    fd_set *block_map = (void*)(disk + block_map_base*FS_BLOCK_SIZE);
    
    int inode_base = block_map_base + n_map_blks;
    struct fs_inode *inodes = (void*)(disk + inode_base*FS_BLOCK_SIZE);

    int rootdir_base = inode_base + n_ino_blks;
    struct fs_dirent *de = (void*)(disk + rootdir_base*FS_BLOCK_SIZE);

    /* superblock */
    *sb = (struct fs_super){.magic = FS_MAGIC, .inode_map_sz = n_ino_map_blks,
                            .inode_region_sz = n_ino_blks,
                            .block_map_sz = n_map_blks,
                            .num_blocks = n_blks, .root_inode = 1};

    /* bitmaps */
    FD_SET(0, inode_map);
    FD_SET(1, inode_map);
    for (i = 0; i <= rootdir_base; i++)
        FD_SET(i, block_map);

    int t  = time(NULL);
    inodes[1] = (struct fs_inode){.uid = 1001, .gid = 125, .mode = 0040777, 
                                  .ctime = t, .mtime = t, .size = 1024,
                                  .direct = {rootdir_base, 0, 0, 0, 0, 0},
                                  .indir_1 = 0, .indir_2 = 0};

    /* remember (from /usr/include/i386-linux-gnu/bits/stat.h)
     *    S_IFDIR = 0040000 - directory
     *    S_IFREG = 0100000 - regular file
     */
    /* block 0 - superblock  [layout for 1MB file]
     *       1 - inode map
     *       2 - block map
     *       3,4,5,6 - inodes
     *       7 - root directory (inode 1)
     */
                      

    assert(size == n_blks* FS_BLOCK_SIZE);
    write(fd, disk, size);
    close(fd);

    return 0;
}
