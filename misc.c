#define FUSE_USE_VERSION 27
#define _XOPEN_SOURCE 500
#define _ATFILE_SOURCE
#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <fuse.h>
#include "blkdev.h"

#include "fsx600.h"		/* only for BLOCK_SIZE */

/*********** DO NOT MODIFY THIS FILE *************/

/* All homework functions are accessed through the operations
 * structure.  
 */
extern struct fuse_operations fs_ops;

struct blkdev *disk;
struct data {
    char *image_name;
    int   part;
    int   cmd_mode;
} _data;
int homework_part;

/*
 * See comments in /usr/include/fuse/fuse_opts.h for details of 
 * FUSE argument processing.
 * 
 *  usage: ./homework -image disk.img [-part #] directory
 *              disk.img  - name of the image file to mount
 *              directory - directory to mount it on
 */
static struct fuse_opt opts[] = {
    {"-image %s", offsetof(struct data, image_name), 0},
    {"-cmdline", offsetof(struct data, cmd_mode), 1},

    {"-part %d", offsetof(struct data, part), 0},
    FUSE_OPT_END
};

char *strmode(char *buf, int mode);

/* Command-line interface, loosely based on FTP. 
 */
static int split(char *p, char **args, int n, char *delim)
{
    char **ap;
    for (ap = args; (*ap = strtok(p, delim)) != NULL; p = NULL)
	if (++ap >= &args[n])
	    break;
    return ap-args;
}


static char cwd[128];
static char paths[16][44];
static int  depth = 0;

static void update_cwd(void)
{
    int i;
    char *p = cwd;

    *p = 0;
    for (i = 0; i < depth; i++)
	p += sprintf(p, "/%s", paths[i]);
}

int do_cd(char *argv[])
{
    char *dir = argv[0];
    int i, nnames;
    char *names[10];
    
    if (!strcmp(dir, "..")) {
	if (depth > 0)
	    depth--;
    }
    else {
	if (dir[0] == '/')
	    depth = 0;
	nnames = split(dir, names, 10, "/");
	for (i = 0; i < nnames; i++, depth++) 
	    strcpy(paths[depth], names[i]);
    }

    update_cwd();
    return 0;
}

char *get_cwd(void)
{
    return depth == 0 ? "/" : cwd;
}

char *fix_path(char *path)
{
    char *p, *q;
    for (p = q = path; *p != 0; p++) {
        if (memcmp(p, "../", 3) == 0)
            p += 2;
        else if (memcmp(p, "//", 2) == 0)
            /* skip */;
        else
            *q++ = *p;
    }
    *q++ = 0;
    return path;
}

int do_pwd(char *argv[])
{
    printf("%s\n", get_cwd());
    return 0;
}

#define DIRENTS_PER_BLOCK (FS_BLOCK_SIZE / sizeof(struct fs_dirent))

char lsbuf[DIRENTS_PER_BLOCK][64];
int  lsi;

void init_ls(void)
{
    lsi = 0;
}

static int filler(void *buf, const char *name, const struct stat *sb, off_t off)
{
    sprintf(lsbuf[lsi++], "%s\n", name);
    return 0;
}

void print_ls(void)
{
    int i;
    qsort(lsbuf, lsi, 64, (void*)strcmp);
    for (i = 0; i < lsi; i++)
	printf("%s", lsbuf[i]);
}

int do_ls0(char *argv[])
{
    init_ls();
    int retval = fs_ops.readdir(get_cwd(), NULL, filler, 0, NULL);
    print_ls();
    return retval;
}

int do_ls1(char *argv[])
{
    char path[128];
    sprintf(path, "%s/%s", get_cwd(), argv[0]);
    init_ls();
    int retval = fs_ops.readdir(fix_path(path), NULL, filler, 0, NULL);
    print_ls();
    return retval;
}

static int dashl_filler(void *buf, const char *name, const struct stat *sb, off_t off)
{
    char mode[16];
    sprintf(lsbuf[lsi++], "%s %s %lld %lld %s",
            name, strmode(mode, sb->st_mode), sb->st_size, sb->st_blocks,
            ctime(&sb->st_mtime));
    return 0;
}

int _lsdashl(char *path)
{
    struct stat sb;
    int retval = fs_ops.getattr(path, &sb);
    if (retval == 0) {
	init_ls();
	if (S_ISDIR(sb.st_mode)) 
	    retval = fs_ops.readdir(path, NULL, dashl_filler, 0, NULL);
	else
	    retval = dashl_filler(NULL, path, &sb, 0);
	print_ls();
    }
    return retval;
}

int do_lsdashl0(char *argv[])
{
    return _lsdashl(get_cwd());
}

int do_lsdashl1(char *argv[])
{
    char path[128];
    sprintf(path, "%s/%s", cwd, argv[0]);
    return _lsdashl(fix_path(path));
}

int do_chmod(char *argv[])
{
    char path[128];
    int mode = strtol(argv[0], NULL, 8);
    sprintf(path, "%s/%s", cwd, argv[1]);
    return fs_ops.chmod(fix_path(path), mode);
}

int do_rename(char *argv[])
{
    char p1[128], p2[128];
    sprintf(p1, "%s/%s", cwd, argv[0]);
    sprintf(p2, "%s/%s", cwd, argv[1]);
    return fs_ops.rename(fix_path(p1), fix_path(p2));
}

int do_mkdir(char *argv[])
{
    char path[128];
    sprintf(path, "%s/%s", cwd, argv[0]);
    return fs_ops.mkdir(fix_path(path), 0777);
}

int do_rmdir(char *argv[])
{
    char path[128];
    sprintf(path, "%s/%s", cwd, argv[0]);
    return fs_ops.rmdir(fix_path(path));
}

int do_rm(char *argv[])
{
    char buf[128], *leaf = argv[0];
    sprintf(buf, "%s/%s", cwd, leaf);
    return fs_ops.unlink(fix_path(buf));
}

int blksiz;
char *blkbuf;

int do_put(char *argv[])
{
    char *outside = argv[0], *inside = argv[1];
    char path[128];
    int len, fd, offset = 0, val;

    if ((fd = open(outside, O_RDONLY, 0)) < 0)
	return fd;

    sprintf(path, "%s/%s", cwd, inside);
    fix_path(path);
    if ((val = fs_ops.mknod(path, 0777 | S_IFREG, 0)) != 0)
	return val;
    
    while ((len = read(fd, blkbuf, blksiz)) > 0) {
	val = fs_ops.write(path, blkbuf, len, offset, NULL);
	if (val != len)
	    break;
	offset += len;
    }
    close(fd);
    return (val >= 0) ? 0 : val;
}

int do_put1(char *argv[])
{
    char *args2[] = {argv[0], argv[0]};
    return do_put(args2);
}

int do_get(char *argv[])
{
    char *inside = argv[0], *outside = argv[1];
    char path[128];
    int len, fd, offset = 0;

    if ((fd = open(outside, O_WRONLY|O_CREAT|O_TRUNC, 0777)) < 0)
	return fd;

    sprintf(path, "%s/%s", cwd, inside);
    fix_path(path);
    while (1) {
        len = fs_ops.read(path, blkbuf, blksiz, offset, NULL);
	if (len > 0)
	    len = write(fd, blkbuf, len);
        if (len <= 0)
	    break;
	offset += len;
    }
    close(fd);
    return (len >= 0) ? 0 : len;
}

int do_get1(char *argv[])
{
    char *args2[] = {argv[0], argv[0]};
    return do_get(args2);
}

int do_show(char *argv[])
{
    char *file = argv[0];
    char path[128];
    int len, offset = 0;

    sprintf(path, "%s/%s", cwd, file);
    fix_path(path);
    while ((len = fs_ops.read(path, blkbuf, blksiz, offset, NULL)) > 0) {
	fwrite(blkbuf, len, 1, stdout);
	offset += len;
    }

    return (len >= 0) ? 0 : len;
}

int do_statfs(char *argv[])
{
    struct statvfs st;
    int retval = fs_ops.statfs("/", &st);
    if (retval == 0)
	printf("max name length: %ld\nblock size: %ld\n",
	       st.f_namemax, st.f_bsize);
    return retval;
}

void _blksiz(int size)
{
    blksiz = size;
    if (blkbuf)
	free(blkbuf);
    blkbuf = malloc(blksiz);
    printf("read/write block size: %d\n", blksiz);
}

int do_blksiz(char *argv[])
{
    _blksiz(atoi(argv[0]));
    return 0;
}

static int do_truncate(char *argv[])
{
    char path[128];
    sprintf(path, "%s/%s", cwd, argv[0]);
    return fs_ops.truncate(fix_path(path), 0);
}

static int do_utime(char *argv[])
{
    struct utimbuf ut;
    char path[128];
    sprintf(path, "%s/%s", cwd, argv[0]);
    ut.actime = time(NULL);
    ut.modtime = time(NULL);
    return fs_ops.utime(fix_path(path), &ut);
}
    
struct {
    char *name;
    int   nargs;
    int   (*f)(char *args[]);
    char  *help;
} cmds[] = {
    {"cd", 1, do_cd, "cd <path> - change directory"},
    {"pwd", 0, do_pwd, "pwd - display current directory"},
    {"ls", 0, do_ls0, "ls - list files in current directory"},
    {"ls", 1, do_ls1, "ls <dir> - list specified directory"},
    {"ls-l", 0, do_lsdashl0, "ls-l - display detailed file listing"},
    {"ls-l", 1, do_lsdashl1, "ls-l <file> - display detailed file info"},
    {"chmod", 2, do_chmod, "chmod <mode> <file> - change permissions"},
    {"rename", 2, do_rename, "rename <oldname> <newname> - rename file"},
    {"mkdir", 1, do_mkdir, "mkdir <dir> - create directory"},
    {"rmdir", 1, do_rmdir, "rmdir <dir> - remove directory"},
    {"rm", 1, do_rm, "rm <file> - remove file"},
    {"put", 2, do_put, "put <outside> <inside> - copy a file from localdir into file system"},
    {"put", 1, do_put1, "put <name> - ditto, but keep the same name"},
    {"get", 2, do_get, "get <inside> <outside> - retrieve a file from file system to local directory"},
    {"get", 1, do_get1, "get <name> - ditto, but keep the same name"},
    {"show", 1, do_show, "show <file> - retrieve and print a file"},
    {"statfs", 0, do_statfs, "statfs - print file system info"},
    {"blksiz", 1, do_blksiz, "blksiz - set read/write block size"},
    {"truncate", 1, do_truncate, "truncate <file> - truncate to zero length"},
    {"utime", 1, do_utime, "utime <file> - set modified time to current time"},
    {0, 0, 0}
};

int cmdloop(void)
{
    char line[128];

    update_cwd();
    
    while (1) {
	printf("cmd> "); fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL)
	    break;

	if (!isatty(0))
	    printf("%s", line);

	if (line[0] == '#')	/* comment lines */
	    continue;
	
	char *args[10];
	int i, nargs = split(line, args, 10, " \t\n");

	if (nargs == 0)
	    continue;
	if (!strcmp(args[0], "quit") || !strcmp(args[0], "exit"))
	    break;
	if (!strcmp(args[0], "help")) {
	    for (i = 0; cmds[i].name != NULL; i++)
		printf("%s\n", cmds[i].help);
	    continue;
	}
	for (i = 0; cmds[i].name != NULL; i++) 
	    if (!strcmp(args[0], cmds[i].name) && nargs == cmds[i].nargs+1) 
		break;
	if (cmds[i].name == NULL) {
	    if (nargs > 0)
		printf("bad command: %s\n", args[0]);
	}
	else {
	    int err = cmds[i].f(&args[1]);
	    if (err != 0)
		printf("error: %s\n", strerror(-err));
	}
    }
    return 0;
}

/* Utility functions
 */

/* strmode - translate a numeric mode into a string
 */
char *strmode(char *buf, int mode)
{
    int mask = 0400;
    char *str = "rwxrwxrwx", *retval = buf;
    *buf++ = S_ISDIR(mode) ? 'd' : '-';
    for (mask = 0400; mask != 0; str++, mask = mask >> 1) 
	*buf++ = (mask & mode) ? *str : '-';
    *buf++ = 0;
    return retval;
}

/**************/

int main(int argc, char **argv)
{
    /* Argument processing and checking
     */
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &_data, opts, NULL) == -1)
	exit(1);

    char *file = _data.image_name;
    if (strcmp(file+strlen(file)-4, ".img") != 0) {
        printf("bad image file (must end in .img): %s\n", file);
        exit(1);
    }
    if ((disk = image_create(file)) == NULL) {
        printf("cannot open image file '%s': %s\n", file, strerror(errno));
        exit(1);
    }

    homework_part = _data.part;

    if (_data.cmd_mode) {
        fs_ops.init(NULL);
        _blksiz(1000);
        cmdloop();
        return 0;
    }

    return fuse_main(args.argc, args.argv, &fs_ops, NULL);
}
