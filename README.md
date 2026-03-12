# bfind
bfind, a breadth-first search version of the UNIX find utility

Owen Marcione & Jack Stewart

Question 1: A hard link is a direct directory entry pointing to an inode, while a symlink is a file containing a path string to another location. The OS forbids hard links to directories (except . and ..) to prevent cycles, so traversal following hard links is always clean without cycles. Symlinks have no such restriction and can point to ancestor directories, creating infinite loops when followed.

Question 2: Inode numbers are only unique within a single filesystem, so two files on different filesystems can share the same st_ino by coincidence. If you only checked st_ino, visiting a file with and inode 11 on /dev/sdb after seeing inode 11 on /dev/sda would falsely look like a cycle and skip a real directory. Using (st_dev, st_ino) together uniquely identifies any file on the system.

Question 3: When the kernel mounts a filesystem at a directory like /proc, it assigns it a new unique st_dev value. When bfind stats a child directory and sees its st_dev differs from the starting path's st_dev, it knows a filesystem boundary was crossed and skips it. -xdev works by comparing st_dev values from struct stat.

Question 4: The VFS is an abstraction layer in the kernel that provides a single unified interface (open, read, stat, etc.) over many different underlying filesystem drivers. Without it, every user program would need to know which filesystem it's talking to and call driver-specific code directly. VFS lets user programs interact with any filesystem identically, with the kernel handling the translation to the correct driver underneath.
