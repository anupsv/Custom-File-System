

#define _GNU_SOURCE
#define FUSE_USE_VERSION 27

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fuse.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "fsx600.h"
#include "blkdev.h"


extern int homework_part;       /* set by '-part n' command-line option */

/*
 * disk access - the global variable 'disk' points to a blkdev
 * structure which has been initialized to access the image file.
 *
 * NOTE - blkdev access is in terms of 1024-byte blocks
 */
extern struct blkdev *disk;

/* by defining bitmaps as 'fd_set' pointers, you can use existing
 * macros to handle them.
 *   FD_ISSET(##, inode_map);
 *   FD_CLR(##, block_map);
 *   FD_SET(##, block_map);
 */
fd_set *inode_map;              /* = malloc(sb.inode_map_size * FS_BLOCK_SIZE); */
fd_set *block_map;

#define MAX_ENTRIES_DIR (FS_BLOCK_SIZE / sizeof(struct fs_dirent))
#define MAX_LENGTH_OF_DIR_NAME 26
#define INDIRECT_BOUND (N_DIRECT + ADDR_PER_BLOCK)
#define SIZE_DOUBLE_INDIRECT (INDIRECT_BOUND + (ADDR_PER_BLOCK * ADDR_PER_BLOCK))
#define ADDR_PER_BLOCK (FS_BLOCK_SIZE / sizeof(uint32_t))

struct fs_inode *inodes;
int max_num_blocks, inode_map_sz, start_block, block_map_sz, inode_block_sz, inode_reg_sz;


/* init - this is called once by the FUSE framework at startup. Ignore
 * the 'conn' argument.
 * recommended actions:
 *   - read superblock
 *   - allocate memory, read bitmaps and inodes
 */
void *fs_init(struct fuse_conn_info *conn) {
    struct fs_super sb;
    if (disk->ops->read(disk, 0, 1, &sb) < 0) {
        exit(1);
    }

    /* your code here */
    int start_blk = 1;
    inode_map = (fd_set *) malloc(sb.inode_map_sz * FS_BLOCK_SIZE);
    block_map = (fd_set *) malloc(sb.block_map_sz * FS_BLOCK_SIZE);
    disk->ops->read(disk, start_blk, sb.inode_map_sz, inode_map);

    start_blk += sb.inode_map_sz;

    disk->ops->read(disk, start_blk, sb.block_map_sz, block_map);

    start_blk += sb.block_map_sz;
    inodes = (struct fs_inode *) malloc(sb.inode_region_sz * FS_BLOCK_SIZE);
    disk->ops->read(disk, start_blk, sb.inode_region_sz, inodes);

    /* set the global variables which will be used in various calculations */
    inode_map_sz = sb.inode_map_sz;
    block_map_sz = sb.block_map_sz;
    max_num_blocks = sb.num_blocks;
    inode_reg_sz = sb.inode_region_sz;
    start_block = start_blk + inode_reg_sz;

    return NULL;
}

/* Note on path translation errors:
 * In addition to the method-specific errors listed below, almost
 * every method can return one of the following errors if it fails to
 * locate a file or directory corresponding to a specified path.
 *
 * ENOENT - a component of the path is not present.
 * ENOTDIR - an intermediate component of the path (e.g. 'b' in
 *           /a/b/c) is not a directory
 */

/* note on splitting the 'path' variable:
 * the value passed in by the FUSE framework is declared as 'const',
 * which means you can't modify it. The standard mechanisms for
 * splitting strings in C (strtok, strsep) modify the string in place,
 * so you have to copy the string and then free the copy when you're
 * done. One way of doing this:
 *
 *    char *_path = strdup(path);
 *    int inum = translate_path_to_inum(_path);
 *    free(_path);
 */


int translate_path_to_inum(char *path) {
    int inum = 1;
    int i = 0;


    char *token = NULL;
    char *tokens[64] = {NULL};
    const char delim[2] = "/";

    /* tokenize the string */
    token = strtok(path, delim);
    while (token) {
        tokens[i++] = token;
        token = strtok(NULL, delim);
    }

    i = 0;
    void *block = NULL;

    token = tokens[i];
    struct fs_inode temp;
    struct fs_dirent *fd;
    block = malloc(FS_BLOCK_SIZE);
    while (token != NULL) {
        temp = inodes[inum];
        disk->ops->read(disk, temp.direct[0], 1, block);
        fd = block;
        int j = 0;
        for (j = 0; j < MAX_ENTRIES_DIR; j++) {
            if (!strcmp(fd->name, token) && fd->valid) {
                if (tokens[i + 1] != NULL && !(fd->isDir)) {
                    free(block);
                    return -ENOTDIR;
                }
                inum = fd->inode;
                break;
            }
            fd++;
        }
        if (j == MAX_ENTRIES_DIR) {
            free(block);
            return -ENOENT;
        }
        token = tokens[++i];
    }

    if (block) {
        free(block);
    }

    return inum;
}


void fs_set_superbock_attrs(struct fs_inode *inode, struct stat *sb, int inum) {
    sb->st_ino = inum;
    sb->st_blocks = (inode->size - 1) / FS_BLOCK_SIZE + 1;
    sb->st_mode = inode->mode;
    sb->st_size = inode->size;
    sb->st_uid = inode->uid;
    sb->st_gid = inode->gid;
    // st_atime, st_ctime not set, so now setting to same as  st_mtime
    // as instructed.
    sb->st_ctime = inode->ctime;
    sb->st_mtime = inode->mtime;
    sb->st_atime = sb->st_mtime;
    sb->st_nlink = 1;
}

/* getattr - get file or directory attributes. For a description of
 *  the fields in 'struct stat', see 'man lstat'.
 *
 * Note - fields not provided in fsx600 are:
 *    st_nlink - always set to 1
 *    st_atime, st_ctime - set to same value as st_mtime
 *
 * errors - path translation, ENOENT
 */
static int fs_getattr(const char *path, struct stat *sb) {
    char *_path = strdupa(path);
    int inum = translate_path_to_inum(_path);

    if (inum == -ENOENT || inum == -ENOTDIR) {
        return inum;
    }

    struct fs_inode inode = inodes[inum];
    fs_set_superbock_attrs(&inode, sb, inum);
    return 0;
}

/* readdir - get directory contents.
 *
 * for each entry in the directory, invoke the 'filler' function,
 * which is passed as a function pointer, as follows:
 *     filler(buf, <name>, <statbuf>, 0)
 * where <statbuf> is a struct stat, just like in getattr.
 *
 * Errors - path resolution, ENOTDIR, ENOENT
 */
static int fs_readdir(const char *path, void *ptr, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi) {
    struct stat sb;
    int ret = fs_getattr(path, &sb);
    // returning any error from getattr function
    if (ret == -ENOENT || ret == -ENOTDIR) {
        return ret;
    }

    struct fs_inode inode = inodes[sb.st_ino];
    // checking if the inode a directory or not
    if (!S_ISDIR(inode.mode)) {
        return -ENOTDIR;
    }

    void *block = malloc(FS_BLOCK_SIZE);
    disk->ops->read(disk, inode.direct[0], 1, block);
    struct fs_dirent *fd = block;

    int i;
    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (fd->valid) {
            memset(&sb, 0, sizeof(sb));
            inode = inodes[fd->inode];
            fs_set_superbock_attrs(&inode, &sb, fd->inode);
            filler(ptr, fd->name, &sb, 0);
        }
        fd++;
    }

    if (block) {
        free(block);
    }
    return 0;
}

/* see description of Part 2. In particular, you can save information
 * in fi->fh. If you allocate memory, free it in fs_releasedir.
 */
static int fs_opendir(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int fs_releasedir(const char *path, struct fuse_file_info *fi) {
    return 0;
}

/* mknod - create a new file with permissions (mode & 01777)
 *
 * Errors - path resolution, EEXIST
 *          in particular, for mknod("/a/b/c") to succeed,
 *          "/a/b" must exist, and "/a/b/c" must not.
 *
 * If a file or directory of this name already exists, return -EEXIST.
 * If this would result in >32 entries in a directory, return -ENOSPC
 * if !S_ISREG(mode) return -EINVAL [i.e. 'mode' specifies a device special
 * file or other non-file object]
 */

char *get_last_of_split(char *path) {

    char *token = NULL;
    char *tokens[64] = {NULL};
    int i = 0;
    /* tokenize the string */
    token = strtok(path, "/");
    while (token) {
        tokens[i++] = token;
        token = strtok(NULL, "/");
    }

    return tokens[--i];
}

// given the complete path, traverse it, find last file/dir and get its
// inum if exists.
int get_parent_inum(char *path, char *str) {
    int inum = 1;
    int i = 0;
    const char delim[2] = "/";
    void *block = NULL;
    char *token = NULL;
    char *tokens[64] = {NULL};

    /* tokenize the string */
    token = strtok(path, delim);
    while (token) {
        tokens[i++] = token;
        token = strtok(NULL, delim);
    }

    /* copy last token to str */
    memset(str, 0, MAX_LENGTH_OF_DIR_NAME);
    if (i > 0) {
        strncpy(str, tokens[i - 1], strlen(tokens[i - 1]));
        //tokens[i-1] = NULL;
    } else {
        return -EINVAL;
    }

    i = 0;
    token = tokens[i];
    struct fs_inode temp;
    struct fs_dirent *fd;
    block = malloc(FS_BLOCK_SIZE);
    while (tokens[i + 1] != NULL) {
        temp = inodes[inum];
        disk->ops->read(disk, temp.direct[0], 1, block);
        fd = block;
        int j = 0;
        for (j = 0; j < MAX_ENTRIES_DIR; j++) {
            if (!strcmp(fd->name, token) && fd->valid) {
                if (tokens[i + 1] != NULL && !(fd->isDir)) {
                    free(block);
                    // return not a dir
                    return -ENOTDIR;
                }
                inum = fd->inode;
                break;
            }
            fd++;
        }
        if (j == MAX_ENTRIES_DIR) {
            free(block);
            // return no entry
            return -ENOENT;
        }
        token = tokens[++i];
    }

    if (block) {
        free(block);
    }

    // return inum
    return inum;
}

// write the complete inode map to disk
void write_inode_map() {
    disk->ops->write(disk, 1, inode_map_sz, inode_map);
}

// free a given block
void free_a_block(int bit) {
    if (bit >= start_block) {
        FD_CLR(bit, block_map);
    }
}

// find and return a free block
int get_free_block() {
    int i = 0;
    for (i = start_block; i < max_num_blocks; i++) {
        if (!FD_ISSET(i, block_map)) {
            FD_SET(i, block_map);
            return i;
        }
    }
    return -ENOSPC;
}

// find and return a free inode
int get_free_inode() {
    int i = 0;
    while (i < 64) {
        if (!FD_ISSET(i, inode_map)) {
            FD_SET(i, inode_map);
            return i;
        }
        i++;
    }
    return -ENOSPC;
}

// write the complete contents to disk
void write_block_map() {
    disk->ops->write(disk, 1 + inode_map_sz, block_map_sz, block_map);
}

// write all the inodes to the disk
void write_all_inodes() {
    disk->ops->write(disk, (1 + inode_map_sz + block_map_sz), inode_reg_sz, inodes);
}

static int fs_mknod(const char *path, mode_t mode, dev_t dev) {
    char dir_name[MAX_LENGTH_OF_DIR_NAME];

    /* get the child inode number */
    char *_path = strdupa(path);
    int child_inum = translate_path_to_inum(_path);

    /* get the parent inode number */
    _path = strdupa(path);
    int parent_inum = get_parent_inum(_path, dir_name);

    /* If parent path contains invalid files other than directories
     * or the parent path is not present
     */
    if (parent_inum == -ENOTDIR || parent_inum == -ENOENT) {
        return parent_inum;
    }

    /* If parent is not a directory */
    struct fs_inode p_inode = inodes[parent_inum];
    if (!S_ISDIR(p_inode.mode)) {
        return -ENOTDIR;
    }

    /* If a file already exists with the given name */
    if (child_inum > 0) {
        return -EEXIST;
    }

    int new_inum = get_free_inode();
    /* If the inode_bitmap is full and no free inode available */
    if (new_inum == -ENOSPC) {
        return -ENOSPC;
    }

    void *block = malloc(FS_BLOCK_SIZE);
    disk->ops->read(disk, p_inode.direct[0], 1, block);
    struct fs_dirent *dirent_parent = block;
    struct fs_inode new_inode = inodes[new_inum];

    int i = 0;
    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (!dirent_parent->valid) {
            memset(dirent_parent, 0, sizeof(struct fs_dirent));
            strncpy(dirent_parent->name, dir_name, strlen(dir_name));
            dirent_parent->valid = 1;
            dirent_parent->isDir = S_ISDIR(mode);
            dirent_parent->inode = new_inum;

            /* create inode, write to disk and update in-memory ds */

            struct fuse_context *context = fuse_get_context();
            int i = 0;
            time_t mytime;
            mytime = time(NULL);

            new_inode.mode = mode;
            new_inode.uid = context->uid;
            new_inode.gid = context->gid;

            new_inode.ctime = mytime;
            new_inode.mtime = mytime;
            new_inode.size = 0;
            for (; i < 6; i++)
                new_inode.direct[i] = 0;
            new_inode.indir_1 = 0;
            new_inode.indir_2 = 0;

            if (S_ISDIR(mode)) {
                int block_num = get_free_block();
                if (block_num == -ENOSPC) {
                    FD_CLR(new_inum, inode_map);
                    write_inode_map();
                    if (block) {
                        free(block);
                    }
                    return -ENOSPC;
                }
                new_inode.direct[0] = block_num;
                write_block_map();

                /* create empty block for direct[0] and write to disk */
                /* block number is same as obtained above */
                void *block_dir = malloc(FS_BLOCK_SIZE);
                memset(block_dir, 0, FS_BLOCK_SIZE);
                disk->ops->write(disk, block_num, 1, block_dir);
                free(block_dir);
            }

            /* write inode_map to disk */
            write_inode_map();

            /* write inode_region to disk */
            inodes[new_inum] = new_inode;
            write_all_inodes();

            /* write the parent directory to disk */
            disk->ops->write(disk, p_inode.direct[0], 1, block);
            break;
        }
        dirent_parent++;
    }

    /* Incase directory already contains 32 entries, return
     * ENOSPC error after clearing the newly allocated bit
     * in inode bitmap.
     */
    if (i == MAX_ENTRIES_DIR) {
        if (block) {
            free(block);
        }
        if (FD_ISSET(new_inum, inode_map)) {
            FD_CLR(new_inum, inode_map);
            write_inode_map();
        }
        return -ENOSPC;
    }

    /* Free the allocated memory blocks */
    if (block) {
        free(block);
    }

    return 0;
}

/* mkdir - create a directory with the given mode.
 * Errors - path resolution, EEXIST
 * Conditions for EEXIST are the same as for create.
 * If this would result in >32 entries in a directory, return -ENOSPC
 *
 * Note that you may want to combine the logic of fs_mknod and
 * fs_mkdir.
 */
static int fs_mkdir(const char *path, mode_t mode) {
    mode = mode | S_IFDIR;
    int ret = fs_mknod(path, mode, 0);
    return ret;
}

/* truncate - truncate file to exactly 'len' bytes
 * Errors - path resolution, ENOENT, EISDIR, EINVAL
 *    return EINVAL if len > 0.
 */
static int fs_truncate(const char *path, off_t len) {
    /* you can cheat by only implementing this for the case of len==0,
     * and an error otherwise.
     */
    if (len != 0)
        return -EINVAL;        /* invalid argument */
    int total_direct_blocks = 6;
    struct stat sb;
    int ret = fs_getattr(path, &sb);

    // checking for path translation and errors
    if (ret == -ENOENT || ret == -ENOTDIR) {
        return ret;
    }


    struct fs_inode inode = inodes[sb.st_ino];

    // now checking if its a directory and not a file

    if (S_ISDIR(inode.mode)) {
        return -EISDIR;
    }
    int address_per_block = (FS_BLOCK_SIZE / sizeof(uint32_t));
    int *blocks = NULL;
    void *indirect_block_1 = malloc(FS_BLOCK_SIZE);


    // free the direct blocks first

    int i = 0;
    for (i = 0; i < total_direct_blocks; i++) {

        if (!inode.direct[i]) {
            break;
        }
        inode.direct[i] = 0;
        free_a_block(inode.direct[i]);
    }

    // now to free the indirect blocks
    if (inode.indir_1 >= start_block) {
        disk->ops->read(disk, inode.indir_1, 1, indirect_block_1);
        blocks = indirect_block_1;
        for (i = 0; i < address_per_block; i++) {
            if (!*blocks) {
                break;
            }
            free_a_block(*blocks);
            blocks++;
        }
    }

    // now freeing 2nd indirect blocks.
    void *indirect_block_2 = malloc(FS_BLOCK_SIZE);
    if (inode.indir_2 >= start_block) {
        disk->ops->read(disk, inode.indir_2, 1, indirect_block_2);
        blocks = indirect_block_2;
        for (i = 0; i < address_per_block; i++) {

            if (!*blocks) {
                break;
            }

            if (*blocks >= start_block) {
                disk->ops->read(disk, *blocks, 1, indirect_block_1);
                int j = 0;
                int *innerloop = indirect_block_1;
                for (j = 0; j < address_per_block; j++) {
                    if (!*innerloop)
                        break;
                    free_a_block(*innerloop);
                    innerloop++;
                }
                free_a_block(*blocks);
                blocks++;
            }
        }
    }

    // free the indirect blocks
    if (indirect_block_1) {
        free(indirect_block_1);
    }
    if (indirect_block_2) {
        free(indirect_block_2);
    }

    write_block_map();

    /* change the file size to zero */
    inode.size = 0;
    inodes[sb.st_ino] = inode;
    write_all_inodes();

    return 0;
}

/* unlink - delete a file
 *  Errors - path resolution, ENOENT, EISDIR
 * Note that you have to delete (i.e. truncate) all the data.
 */

static int fs_unlink(const char *path) {
    int i = 0;
    int inum = fs_truncate(path, 0);
    if (inum < 0)
        return inum;

    char dir_name[MAX_LENGTH_OF_DIR_NAME];
    char *_path = strdupa(path);
    int parent_inum = get_parent_inum(_path, dir_name);
    _path = strdupa(path);
    char *last;

    last = get_last_of_split(_path);
    //free(_path);

    struct fs_inode parent_dir = inodes[parent_inum];
    struct fs_dirent *block = (struct fs_dirent *) malloc(FS_BLOCK_SIZE);
    disk->ops->read(disk, parent_dir.direct[0], 1, block);


    /* find the inode entry and clear it */
    for (i = 0; i < 32; i++) {

        if (block[i].valid == 0) {
            continue;
        }

        if (strcmp(block[i].name, last) == 0) {

            if (block[i].isDir) {
                return -EISDIR;
            }
            int file_node_num = block[i].inode;
            struct fs_inode fileinode = inodes[file_node_num];
            if (FD_ISSET(file_node_num, inode_map)) {
                FD_CLR(file_node_num, inode_map);
                fileinode.size = 0;
                fileinode.mtime = time(NULL);
                block[i].valid = 0;
                inodes[file_node_num] = fileinode;
                break;
            }
        }
    }


    if (i > 31) {
        return -ENOENT;
    }

    disk->ops->write(disk, parent_dir.direct[0], 1, block);
    write_all_inodes();
    //write_block_map();
    write_inode_map();
    free(block);

    return 0;

}

/* rmdir - remove a directory
 *  Errors - path resolution, ENOENT, ENOTDIR, ENOTEMPTY
 */
static int fs_rmdir(const char *path) {
    char dir_name[MAX_LENGTH_OF_DIR_NAME];
    char *_path = strdupa(path);
    int child_inum = translate_path_to_inum(_path);
    _path = strdupa(path);
    int parent_inum = get_parent_inum(_path, dir_name);

    /* If path contains invalid files other than directories
     * or the path is not present
     */
    if (child_inum == -ENOTDIR || child_inum == -ENOENT)
        return child_inum;

    /* If child is not a directory */
    struct fs_inode c_inode = inodes[child_inum];
    if (!S_ISDIR(c_inode.mode)) {
        return -ENOTDIR;
    }

    void *block = malloc(FS_BLOCK_SIZE);
    disk->ops->read(disk, c_inode.direct[0], 1, block);
    struct fs_dirent *entry = block;

    int i = 0;
    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (entry->valid) {
            if (block) {
                free(block);
            }
            return -ENOTEMPTY;
        }
        entry++;
    }

    free_a_block(c_inode.direct[0]);
    write_block_map();

    memset(&c_inode, 0, sizeof(struct fs_inode));
    inodes[child_inum] = c_inode;

    struct fs_inode p_inode = inodes[parent_inum];
    disk->ops->read(disk, p_inode.direct[0], 1, block);
    entry = block;

    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (entry->inode == child_inum) {
            memset(entry, 0, sizeof(struct fs_dirent));
            FD_CLR(child_inum, inode_map);
            write_inode_map();
            break;
        }
        entry++;
    }
    disk->ops->write(disk, p_inode.direct[0], 1, block);

    inodes[parent_inum] = p_inode;
    write_all_inodes();

    if (block) {
        free(block);
    }

    return 0;
}

/* rename - rename a file or directory
 * Errors - path resolution, ENOENT, EINVAL, EEXIST
 *
 * ENOENT - source does not exist
 * EEXIST - destination already exists
 * EINVAL - source and destination are not in the same directory
 *
 * Note that this is a simplified version of the UNIX rename
 * functionality - see 'man 2 rename' for full semantics. In
 * particular, the full version can move across directories, replace a
 * destination file, and replace an empty directory with a full one.
 */
static int fs_rename(const char *src_path, const char *dst_path) {

    char the_old_name[MAX_LENGTH_OF_DIR_NAME];
    char *tmp_path = strdupa(src_path);
    int prev_pinum = get_parent_inum(tmp_path, the_old_name);

    tmp_path = strdupa(src_path);
    int curr_inum = translate_path_to_inum(tmp_path);

    char the_new_name[MAX_LENGTH_OF_DIR_NAME];
    tmp_path = strdupa(dst_path);
    int new_pinum = get_parent_inum(tmp_path, the_new_name);

    if (curr_inum == -ENOTDIR || curr_inum == -ENOENT) {
        return curr_inum;
    }

    if (prev_pinum != new_pinum) {
        return -EINVAL;
    }

    struct fs_inode parent_inode = inodes[prev_pinum];
    void *block = malloc(FS_BLOCK_SIZE);
    disk->ops->read(disk, parent_inode.direct[0], 1, block);
    struct fs_dirent *entry = block;

    /* to check if destination is not present */
    int i = 0;
    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (!strcmp(entry->name, the_new_name)) {
            if (block) {
                free(block);
            }
            return -EEXIST;
        }
        entry++;
    }

    /* update name of matching inode */
    entry = block;
    for (i = 0; i < MAX_ENTRIES_DIR; i++) {
        if (entry->inode == curr_inum) {
            strncpy(entry->name, the_new_name, strlen(the_new_name));
            struct fs_inode inode = inodes[curr_inum];
            inode.ctime = time(NULL);
            inodes[curr_inum] = inode;
            write_all_inodes();
            break;
        }
        entry++;
    }

    /* write back to disk */
    disk->ops->write(disk, parent_inode.direct[0], 1, block);

    /* free previously allocated memory */
    if (block) {
        free(block);
    }

    return 0;

}

/* chmod - change file permissions
 * utime - change access and modification times
 *         (for definition of 'struct utimebuf', see 'man utime')
 *
 * Errors - path resolution, ENOENT.
 */
static int fs_chmod(const char *path, mode_t mode) {
    struct stat sb;
    int ret = fs_getattr(path, &sb);

    // check if there was any error when the path is being resolved.
    if (ret == -ENOENT || ret == -ENOTDIR) {
        return ret;
    }

    struct fs_inode inode = inodes[sb.st_ino];

    // update the mode of the directory or the file.
    if (S_ISDIR(inode.mode)) {
        inode.mode = (S_IFDIR | mode);
    } else if (S_ISREG(inode.mode)) {
        inode.mode = (S_IFREG | mode);
    }

    // update the ctime for the inode
    inode.ctime = time(NULL);

    // finally write to the disk
    inodes[sb.st_ino] = inode;
    write_all_inodes();
    return 0;
}

int fs_utime(const char *path, struct utimbuf *ut) {
    struct stat sb;
    int ret = fs_getattr(path, &sb);

    // check if there was any error when the path is being resolved.
    if (ret == -ENOENT || ret == -ENOTDIR) {
        return ret;
    }

    struct fs_inode inode = inodes[sb.st_ino];

    // the modification time updated for the directory or file type.
    inode.mtime = ut->modtime;

    // finally write to disk for persistance
    inodes[sb.st_ino] = inode;
    write_all_inodes();
    return 0;
}

/* read - read data from an open file.
 * should return exactly the number of bytes requested, except:
 *   - if offset >= file len, return 0
 *   - if offset+len > file len, return bytes from offset to EOF
 *   - on error, return <0
 * Errors - path resolution, ENOENT, EISDIR
 */



static int fs_read(const char *path, char *buf, size_t len, off_t offset,
                   struct fuse_file_info *fi) {

    int num_copied = 0;

    struct stat sb;
    int ret = fs_getattr(path, &sb);

    /* error-checking for path resolution */
    if (ret == -ENOENT || ret == -ENOTDIR) {
        return ret;
    }

    /* if offset >= file len */
    int n_blocks = offset / FS_BLOCK_SIZE;
    if (n_blocks >= SIZE_DOUBLE_INDIRECT) {
        return 0;
    }

    struct fs_inode inode = inodes[sb.st_ino];

    /* if given path is a directory instead of file */
    if (S_ISDIR(inode.mode)) {
        return -EISDIR;
    }

    void *block = malloc(FS_BLOCK_SIZE);
    void *indirect_1 = malloc(FS_BLOCK_SIZE);
    void *indirect_2 = malloc(FS_BLOCK_SIZE);

    int i = 0;
    int cnt_offset = 0;

    // handling in different modular blocks as professor mentioned
    if (n_blocks < 6) {
        goto handle_direct_blocks;
    } else if (n_blocks >= 6 && n_blocks < 262) {
        goto handle_indirect_blocks;
    } else if (n_blocks >= 262 && n_blocks < SIZE_DOUBLE_INDIRECT) {
        goto handle_double_indirect_blocks;
    }

// this handles the direct block writing.
    handle_direct_blocks:
    i = n_blocks;
    cnt_offset = n_blocks * FS_BLOCK_SIZE;
    for (; i < 6; i++) {
        int j = 0;
        disk->ops->read(disk, inode.direct[i], 1, block);
        char *string = block;
        for (j = 0; j < FS_BLOCK_SIZE; j++) {
            if (cnt_offset < offset)
                cnt_offset++;
            else {
                if (len > 0 && (offset + num_copied) < inode.size) {
                    // add data to the buffer from str
                    *buf = *(string + j);
                    num_copied++;
                    len--;
                } else {
                    goto cleanup;
                }
                buf++;
            }
        }
    }

// this handles indirect level 1 blocks
    handle_indirect_blocks:
    i = 0;
    disk->ops->read(disk, inode.indir_1, 1, indirect_1);
    int *blocks = indirect_1;
    if (n_blocks >= 6) {
        i = n_blocks - 6;
        blocks += i;
    }
    cnt_offset = (i + 6) * FS_BLOCK_SIZE;
    for (; i < ADDR_PER_BLOCK; i++) {
        int j = 0;
        disk->ops->read(disk, *blocks, 1, block);
        char *string = block;
        for (j = 0; j < FS_BLOCK_SIZE; j++) {
            if (cnt_offset < offset)
                cnt_offset++;
            else {
                if (len > 0 && (offset + num_copied) < inode.size) {
                    *buf = *(string + j);
                    num_copied++;
                    len--;
                } else {
                    goto cleanup;
                }
                buf++;
            }
        }
        blocks++;
    }

// this handles indirect level 2 blocks
    handle_double_indirect_blocks:
    i = 0;
    int j = 0;
    disk->ops->read(disk, inode.indir_2, 1, indirect_2);
    int *indirect_level1_blocks = indirect_2;
    if (n_blocks >= 262) {
        i = (n_blocks - 262) / ADDR_PER_BLOCK;
        indirect_level1_blocks += i;
        j = (n_blocks - 262) % ADDR_PER_BLOCK;

    }
    cnt_offset = (262 + (i * ADDR_PER_BLOCK)) * FS_BLOCK_SIZE;
    for (; i < ADDR_PER_BLOCK; i++) {
        disk->ops->read(disk, *indirect_level1_blocks, 1, indirect_1);
        int *indirect_level2_blocks = indirect_1;
        indirect_level2_blocks += j;
        cnt_offset += j * FS_BLOCK_SIZE;
        for (; j < ADDR_PER_BLOCK; j++) {
            int k = 0;
            disk->ops->read(disk, *indirect_level2_blocks, 1, block);
            char *string = block;
            for (k = 0; k < FS_BLOCK_SIZE; k++) {
                if (cnt_offset < offset)
                    cnt_offset++;
                else {
                    if (len > 0 && (offset + num_copied) < inode.size) {
                        *buf = *(string + k);
                        num_copied++;
                        len--;
                    } else {
                        goto cleanup;
                    }
                    buf++;
                }
            }
            indirect_level2_blocks++;
        }
        j = 0;
        indirect_level1_blocks++;
    }

// this is the cleanup. free's the blocks and additional variables.
    cleanup:
    *buf = '\0';
    if (block) {
        free(block);
    }

    if (indirect_1) {
        free(indirect_1);
    }

    if (indirect_2) {
        free(indirect_2);
    }

    return num_copied;
}


/* write - write data to a file
 * It should return exactly the number of bytes requested, except on
 * error.
 * Errors - path resolution, ENOENT, EISDIR
 *  return EINVAL if 'offset' is greater than current file length.
 *  (POSIX semantics support the creation of files with "holes" in them,
 *   but we don't)
 */
static int fs_write(const char *path, const char *buf, size_t len,
                    off_t offset, struct fuse_file_info *fi) {

    struct stat sb;
    struct utimbuf ut;
    ut.modtime = time(NULL);
    int ret = fs_getattr(path, &sb);

    /* error-checking for path resolution */
    if (ret == -ENOENT || ret == -ENOTDIR)
        return ret;

    struct fs_inode inode = inodes[sb.st_ino];

    /* if given path is a directory instead of file */
    if (S_ISDIR(inode.mode)) {
        return -EISDIR;
    }

    /* if offset > current file length */
    if (offset > inode.size)
        return -EINVAL;

    int number_of_blocks = offset / FS_BLOCK_SIZE;

    int total_bytes_written = 0;
    int total_bytes_in_block = 0;
    void *block = malloc(FS_BLOCK_SIZE);
    void *indirect_1 = malloc(FS_BLOCK_SIZE);
    void *indirect_2 = malloc(FS_BLOCK_SIZE);
    int i = 0;
    if (number_of_blocks < 6) {
        goto handle_writing_direct_blocks;
    } else if (number_of_blocks >= 6 && number_of_blocks < 262) {
        goto handle_writing_indirect_blocks_level1;
    } else if (number_of_blocks >= 262 && number_of_blocks < SIZE_DOUBLE_INDIRECT) {
        goto handle_writing_indirect_level2_blocks;
    }

// this handles writing to direct blocks
    handle_writing_direct_blocks:
    i = number_of_blocks;

    for (; i < 6; i++) {
        if (!inode.direct[i]) {
            ret = get_free_block();
            if (ret == -ENOSPC) {
                total_bytes_written = -ENOSPC;
                goto cleanup;
            }
            write_block_map();
            disk->ops->read(disk, ret, 1, block);
            memset(block, 0, FS_BLOCK_SIZE);
            inode.direct[i] = ret;
            total_bytes_in_block = 0;
        } else {
            disk->ops->read(disk, inode.direct[i], 1, block);
            total_bytes_in_block = offset % FS_BLOCK_SIZE;
        }
        char *start = block + total_bytes_in_block;
        if (total_bytes_in_block == 0)
            offset = 0;

        /* fill the current block */
        int remaining_bytes_of_block = 0;
        remaining_bytes_of_block = FS_BLOCK_SIZE - total_bytes_in_block;
        if (len > remaining_bytes_of_block)
            len = len - remaining_bytes_of_block;
        else {
            remaining_bytes_of_block = len;
            len = 0;
        }
        memcpy(start, buf, remaining_bytes_of_block);
        buf += remaining_bytes_of_block;
        total_bytes_written += remaining_bytes_of_block;
        start += remaining_bytes_of_block;
        inode.size += remaining_bytes_of_block;
        /* write block and inode back to disk */
        inodes[sb.st_ino] = inode;
        write_all_inodes();
        disk->ops->write(disk, inode.direct[i], 1, block);
        if (len == 0) {
            goto cleanup;
        }
    }
    offset = 0;
    number_of_blocks = 6;

// this handles writing to level 1 indirect blocks
    handle_writing_indirect_blocks_level1:
    i = offset / FS_BLOCK_SIZE;

    if (!inode.indir_1) {
        ret = get_free_block();
        if (ret == -ENOSPC) {
            total_bytes_written = -ENOSPC;
            goto cleanup;
        }
        write_block_map();
        disk->ops->read(disk, ret, 1, block);
        memset(block, 0, FS_BLOCK_SIZE);
        inode.indir_1 = ret;
        inodes[sb.st_ino] = inode;
        write_all_inodes();
        disk->ops->write(disk, inode.indir_1, 1, block);
    }

    disk->ops->read(disk, inode.indir_1, 1, indirect_1);
    int *blocks = indirect_1;
    if (number_of_blocks >= 6) {
        i = number_of_blocks - 6;
        blocks += i;
    }

    for (; i < ADDR_PER_BLOCK; i++) {
        if (!*blocks) {
            ret = get_free_block();
            if (ret == -ENOSPC) {
                total_bytes_written = -ENOSPC;
                goto cleanup;
            }
            write_block_map();
            disk->ops->read(disk, ret, 1, block);
            memset(block, 0, FS_BLOCK_SIZE);
            *blocks = ret;
            total_bytes_in_block = 0;
        } else {
            disk->ops->read(disk, *blocks, 1, block);
            total_bytes_in_block = (offset % FS_BLOCK_SIZE);
        }

        char *start = block + total_bytes_in_block;
        if (total_bytes_in_block == 0) {
            offset = 0;
        }

        /* fill the current block */
        int remaining_bytes_of_block = 0;
        remaining_bytes_of_block = FS_BLOCK_SIZE - total_bytes_in_block;
        if (len > remaining_bytes_of_block) {
            len = len - remaining_bytes_of_block;
        } else {
            remaining_bytes_of_block = len;
            len = 0;
        }
        memcpy(start, buf, remaining_bytes_of_block);
        buf += remaining_bytes_of_block;
        total_bytes_written += remaining_bytes_of_block;
        start += remaining_bytes_of_block;
        inode.size += remaining_bytes_of_block;
        /* write block and inode back to disk */
        inodes[sb.st_ino] = inode;
        write_all_inodes();
        disk->ops->write(disk, inode.indir_1, 1, indirect_1);
        disk->ops->write(disk, *blocks, 1, block);
        if (len == 0) {
            goto cleanup;
        }
        blocks++;
    }
    offset = 0;
    number_of_blocks = 262;

// this handles writing to indirect level2 blocks.
    handle_writing_indirect_level2_blocks:

    if (!inode.indir_2) {
        ret = get_free_block();
        if (ret == -ENOSPC) {
            // setting this as this var gets returned in cleanup
            total_bytes_written = -ENOSPC;
            goto cleanup;
        }
        write_block_map();
        disk->ops->read(disk, ret, 1, block);
        memset(block, 0, FS_BLOCK_SIZE);
        inode.indir_2 = ret;
        inodes[sb.st_ino] = inode;
        write_all_inodes();
        disk->ops->write(disk, inode.indir_2, 1, block);
    }

    disk->ops->read(disk, inode.indir_2, 1, indirect_2);
    int *outer = indirect_2;
    i = offset / FS_BLOCK_SIZE;
    int j = 0;

    if (number_of_blocks >= 262) {
        i = (number_of_blocks - 262) / ADDR_PER_BLOCK;
        outer += i;

        j = (number_of_blocks - 262) % ADDR_PER_BLOCK;

    }

    for (i = i; i < ADDR_PER_BLOCK; i++) {
        if (!*outer) {
            ret = get_free_block();
            if (ret == -ENOSPC) {
                total_bytes_written = -ENOSPC;
                goto cleanup;
            }
            write_block_map();
            disk->ops->read(disk, ret, 1, indirect_1);
            memset(indirect_1, 0, FS_BLOCK_SIZE);
            *outer = ret;
            disk->ops->write(disk, inode.indir_2, 1, indirect_2);
        }

        disk->ops->read(disk, *outer, 1, indirect_1);
        int *inner = indirect_1;

        inner += j;
        for (j = j; j < ADDR_PER_BLOCK; j++) {
            if (!*inner) {
                ret = get_free_block();
                if (ret == -ENOSPC) {
                    total_bytes_written = -ENOSPC;
                    goto cleanup;
                }
                write_block_map();
                disk->ops->read(disk, ret, 1, block);
                memset(block, 0, FS_BLOCK_SIZE);
                *inner = ret;
                total_bytes_in_block = 0;
            } else {
                disk->ops->read(disk, *inner, 1, block);
                total_bytes_in_block = (offset % FS_BLOCK_SIZE);
            }
            char *start = block + total_bytes_in_block;

            /* fill the current block */
            int remaining_bytes_of_block = 0;
            remaining_bytes_of_block = FS_BLOCK_SIZE - total_bytes_in_block;
            if (len > remaining_bytes_of_block) {
                len = len - remaining_bytes_of_block;
            } else {
                remaining_bytes_of_block = len;
                len = 0;
            }
            memcpy(start, buf, remaining_bytes_of_block);
            buf += remaining_bytes_of_block;
            total_bytes_written += remaining_bytes_of_block;
            start += remaining_bytes_of_block;
            inode.size += remaining_bytes_of_block;
            /* write block and inode back to disk */
            inodes[sb.st_ino] = inode;
            write_all_inodes();
            disk->ops->write(disk, *inner, 1, block);
            disk->ops->write(disk, *outer, 1, indirect_1);
            if (len == 0) {
                goto cleanup;
            }
            inner++;
        }
        j = 0;
        outer++;
    }


    cleanup:

    // updating modified time.
    ut.modtime = time(NULL);
    fs_utime(path, &ut);

    if (block) {
        free(block);
    }

    if (indirect_1) {
        free(indirect_1);
    }

    if (indirect_2) {
        free(indirect_2);
    }

    return total_bytes_written;
}


static int fs_open(const char *path, struct fuse_file_info *fi) {
    return 0;
}

static int fs_release(const char *path, struct fuse_file_info *fi) {
    return 0;
}

/* statfs - get file system statistics
 * see 'man 2 statfs' for description of 'struct statvfs'.
 * Errors - none.
 */
static int fs_statfs(const char *path, struct statvfs *st) {
    /* needs to return the following fields (set others to zero):
     *   f_bsize = BLOCK_SIZE
     *   f_blocks = total image - metadata
     *   f_bfree = f_blocks - blocks used
     *   f_bavail = f_bfree
     *   f_namelen = <whatever your max namelength is>
     *
     * this should work fine, but you may want to add code to
     * calculate the correct values later.
     */

    int i, cnt_offset;
    cnt_offset = 0;
    for (i = 1; i < block_map_sz * FS_BLOCK_SIZE; i++) {
        if (!FD_ISSET(i, block_map)) {
            cnt_offset++;
        }
    }

    st->f_bsize = FS_BLOCK_SIZE;
    st->f_blocks = max_num_blocks - (1 + block_map_sz + inode_map_sz + inode_reg_sz);           /* probably want to */
    st->f_bfree = st->f_blocks - cnt_offset;            /* change these */
    st->f_bavail = st->f_bfree;           /* values */
    st->f_namemax = MAX_LENGTH_OF_DIR_NAME + 1;

    return 0;
}

/* operations vector. Please don't rename it, as the skeleton code in
 * misc.c assumes it is named 'fs_ops'.
 */
struct fuse_operations fs_ops = {
        .init = fs_init,
        .getattr = fs_getattr,
        .opendir = fs_opendir,
        .readdir = fs_readdir,
        .releasedir = fs_releasedir,
        .mknod = fs_mknod,
        .mkdir = fs_mkdir,
        .unlink = fs_unlink,
        .rmdir = fs_rmdir,
        .rename = fs_rename,
        .chmod = fs_chmod,
        .utime = fs_utime,
        .truncate = fs_truncate,
        .open = fs_open,
        .read = fs_read,
        .write = fs_write,
        .release = fs_release,
        .statfs = fs_statfs,
};

