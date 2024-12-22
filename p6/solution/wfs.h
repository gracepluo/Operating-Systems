#ifndef WFS_H
#define WFS_H


#include <time.h>
#include <sys/stat.h>
#include <stdint.h>


#define BLOCK_SIZE (512)
#define MAX_NAME   (28)
#define MAX_DISKS 10


#define D_BLOCK    (6)
#define IND_BLOCK  (D_BLOCK+1)
#define N_BLOCKS   (IND_BLOCK+1)

#define INDIRECT_BLOCK_ENTRIES (BLOCK_SIZE / sizeof(off_t)) // 64 entries
/*
  The fields in the superblock should reflect the structure of the filesystem.
  `mkfs` writes the superblock to offset 0 of the disk image. 
  The disk image will have this format:

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0    ^                   ^
i_bitmap_ptr        i_blocks_ptr

*/

// Superblock
#include <stdint.h>

struct wfs_sb {
    uint64_t num_inodes;       // 8 bytes
    uint64_t num_data_blocks;  // 8 bytes
    uint64_t i_bitmap_ptr;     // 8 bytes
    uint64_t d_bitmap_ptr;     // 8 bytes
    uint64_t i_blocks_ptr;     // 8 bytes
    uint64_t d_blocks_ptr;     // 8 bytes
    // Extend after this line
    int32_t raid_mode;         // 4 bytes
    int32_t num_disks;         // 4 bytes
    // Add padding if necessary to align to 8-byte boundary
    int32_t padding[2];        // 8 bytes total padding
    char disk_order[10][MAX_NAME]; 
};


// Inode
struct wfs_inode {
    int     num;      /* Inode number */
    mode_t  mode;     /* File type and mode */
    uid_t   uid;      /* User ID of owner */
    gid_t   gid;      /* Group ID of owner */
    off_t   size;     /* Total size, in bytes */
    int     nlinks;   /* Number of links */

    time_t atim;      /* Time of last access */
    time_t mtim;      /* Time of last modification */
    time_t ctim;      /* Time of last status change */

    off_t blocks[N_BLOCKS];
};

// Directory entry
struct wfs_dentry {
    char name[MAX_NAME];
    int num;
};

#endif // WFS_H