# Custom-File-System

A simple derivative of the Unix FFS file system. I used the FUSE toolkit in Linux to implement the file system as a user-space process; instead of a physical disk I will use a data file accessed through a block device interface
specified at the end of this file. (the blkdev pointer can be found in the global variable 'disk').
