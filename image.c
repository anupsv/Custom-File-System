
#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "blkdev.h"

struct image_dev {
    char *path;
    int   fd;
    int   nblks;
};


/* The blkdev operations - num_blocks, read, write, and close.
 */
static int image_num_blocks(struct blkdev *dev)
{
    struct image_dev *im = dev->private;
    return im->nblks;
}

static int image_read(struct blkdev *dev, int offset, int len, void *buf)
{
    struct image_dev *im = dev->private;

    /* to fail a disk we close its file descriptor and set it to -1 */
    if (im->fd == -1)
        return E_UNAVAIL;

    assert(offset >= 0 && offset+len <= im->nblks);

    int result = pread(im->fd, buf, len*BLOCK_SIZE, offset*BLOCK_SIZE);

    /* Since I'm not asking for the code that calls this to handle
     * errors other than E_BADADDR and E_UNAVAIL, we report errors and
     * then exit. Since we already checked the address, this shouldn't
     * happen very often.
     */
    if (result < 0) {
        fprintf(stderr, "read error on %s: %s\n", im->path, strerror(errno));
        assert(0);
    }
    if (result != len*BLOCK_SIZE) {
        fprintf(stderr, "short read on %s: %s\n", im->path, strerror(errno));
        assert(0);
    }
    
    return SUCCESS;
}

static int image_write(struct blkdev * dev, int offset, int len, void *buf)
{
    struct image_dev *im = dev->private;

    if (offset == 0)
        printf("ERROR? write to sector 0\n");
    
    /* to fail a disk we close its file descriptor and set it to -1 */
    if (im->fd == -1)
        return E_UNAVAIL;

     assert(offset >= 0 && offset+len <= im->nblks);
    
    int result = pwrite(im->fd, buf, len*BLOCK_SIZE, offset*BLOCK_SIZE);

    /* again, report the error and then exit with an assert
     */
    if (result != len*BLOCK_SIZE) {
        fprintf(stderr, "write error on %s: %s\n", im->path, strerror(errno));
        assert(0);
    }

    return SUCCESS;
}

static int image_flush(struct blkdev * dev, int offset, int len)
{
    return SUCCESS;
}

void image_close(struct blkdev *dev)
{
    struct image_dev *im = dev->private;

    if (im->fd != -1)
        close(im->fd);
    free(im);
    dev->private = NULL;        /* crash any attempts to access */
    free(dev);
}

struct blkdev_ops image_ops = {
    .num_blocks = image_num_blocks,
    .read = image_read,
    .write = image_write,
    .flush = image_flush,
    .close = image_close
};

/* create an image blkdev reading from a specified image file.
 */
struct blkdev *image_create(char *path)
{
    struct blkdev *dev = malloc(sizeof(*dev));
    struct image_dev *im = malloc(sizeof(*im));

    if (dev == NULL || im == NULL)
        return NULL;

    im->path = strdup(path);    /* save a copy for error reporting */
    
    im->fd = open(path, O_RDWR);
    if (im->fd < 0) {
        fprintf(stderr, "can't open image %s: %s\n", path, strerror(errno));
        return NULL;
    }
    struct stat sb;
    if (fstat(im->fd, &sb) < 0) {
        fprintf(stderr, "can't access image %s: %s\n", path, strerror(errno));
        return NULL;
    }

    /* print a warning if file is not a multiple of the block size -
     * this isn't a fatal error, as extra bytes beyond the last full
     * block will be ignored by read and write.
     */
    if (sb.st_size % BLOCK_SIZE != 0)
        fprintf(stderr, "warning: file %s not a multiple of %d bytes\n",
                path, BLOCK_SIZE);
    
    im->nblks = sb.st_size / BLOCK_SIZE;
    dev->private = im;
    dev->ops = &image_ops;

    return dev;
}

/* force an image blkdev into failure. after this any further access
 * to that device will return E_UNAVAIL.
 */
void image_fail(struct blkdev *dev)
{
    struct image_dev *im = dev->private;

    if (im->fd != -1)
        close(im->fd);
    im->fd = -1;
}
