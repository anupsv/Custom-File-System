## Custom-File-System

A simple derivative of the Unix FFS file system. I used the FUSE toolkit in Linux to implement the file system as a user-space process; instead of a physical disk I will use a data file accessed through a block device interface
specified at the end of this file. (the blkdev pointer can be found in the global variable 'disk').

## STRUCTURES:
```
File System Format:
+-------------+--------------+--------------+---------+-------------+
| SUPER BLOCK | INODE BITMAP | BLOCK BITMAP | INODES  | DATA BLOCKS |
+-------------+--------------+--------------+---------+-------------+

Inode Structure:
+----------------------+-----------+
|     Description      |   Usage   |
+----------------------+-----------+
| File Owner           | uid       |
|                      |           |
| Group                | gid       |
|                      |           |
| Permissions and type | mode      |
|                      |           |
| TimeStamps           | ctime     |
|                      | mtime     |
|                      |           |
| Size of file         | size      |
|                      |           |
| 6 blocks in file     | direct[6] |
|                      |           |
| indirect pointer 1   | indir_1   |
|                      |           |
| indirect pointer 2   | indir_2   |
+----------------------+-----------+

Directories:
+-------------+----------+
| Description |   Size   |
+-------------+----------+
| valid       | 1-bit    |
|             |          |
| isDir       | 1-bit    |
|             |          |
| inode       | 30-bits  |
|             |          |
| name        | 28-bytes |
+-------------+----------+
```
