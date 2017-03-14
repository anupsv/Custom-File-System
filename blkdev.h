#ifndef __BLKDEV_H__
#define __BLKDEV_H__

#define BLOCK_SIZE 1024

struct blkdev {
    struct blkdev_ops *ops;
    void *private;
};

struct blkdev_ops {
    int  (*num_blocks)(struct blkdev *dev);
    int  (*read)(struct blkdev *dev, int first_blk, int num_blks, void *buf);
    int  (*write)(struct blkdev *dev, int first_blk, int num_blks, void *buf);
    int  (*flush)(struct blkdev *dev, int first_blk, int num_blks);
    void (*close)(struct blkdev *dev);
};

enum {SUCCESS = 0, E_BADADDR = -1, E_UNAVAIL = -2, E_SIZE = -3};

extern struct blkdev *image_create(char *path);

#endif
