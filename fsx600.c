

#ifndef __CSX600_H__
#define __CSX600_H__

#define FS_BLOCK_SIZE 1024
#define FS_MAGIC 0x37363030

/* Entry in a directory
 */
struct fs_dirent {
    uint32_t valid : 1;
    uint32_t isDir : 1;
    uint32_t inode : 30;
    char name[28];              /* with trailing NUL */
};

/* Superblock - holds file system parameters. 
 */
struct fs_super {
    uint32_t magic;
    uint32_t inode_map_sz;       /* in blocks */
    uint32_t inode_region_sz;    /* in blocks */
    uint32_t block_map_sz;       /* in blocks */
    uint32_t num_blocks;         /* total, including SB, bitmaps, inodes */
    uint32_t root_inode;        /* always inode 1 */

    /* pad out to an entire block */
    char pad[FS_BLOCK_SIZE - 6 * sizeof(uint32_t)]; 
};

#define N_DIRECT 6
struct fs_inode {
    uint16_t uid;
    uint16_t gid;
    uint32_t mode;
    uint32_t ctime;
    uint32_t mtime;
     int32_t size;
    uint32_t direct[N_DIRECT];
    uint32_t indir_1;
    uint32_t indir_2;
    uint32_t pad[3];            /* 64 bytes per inode */
};

enum {INODES_PER_BLK = FS_BLOCK_SIZE / sizeof(struct fs_inode)};

#endif



