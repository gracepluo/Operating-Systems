#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include "wfs.h" 

#define MAX_DISKS 10
#define INODE_SIZE 512
#define BITS_PER_BYTE 8

// Helper functions
int get_bit(char *bitmap, int index) {
    return (bitmap[index / 8] >> (index % 8)) & 1;
}

void set_bit(char *bitmap, int index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

void clear_bit(char *bitmap, int index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

// Function to generate a unique disk ID
void generate_disk_id(int index, char *disk_id) {
    // Format: DISK_0001, DISK_0002, etc.
    snprintf(disk_id, MAX_NAME, "DISK_%04d", index + 1);
}

int round_up_blocks(int num_blocks) {
    int remainder = num_blocks % 32;
    if (remainder != 0) {
        num_blocks += (32 - remainder);
    }
    return num_blocks;
}

int main(int argc, char *argv[]) {
    int opt;
    int raid_mode = -1;
    char *disk_files[MAX_DISKS];
    int num_disks = 0;
    int num_inodes = -1;
    int num_data_blocks = -1;

    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch (opt) {
            case 'r':
                if (strcmp(optarg, "0") == 0)
                    raid_mode = 0;
                else if (strcmp(optarg, "1") == 0)
                    raid_mode = 1;
                else if (strcmp(optarg, "1v") == 0)
                    raid_mode = 2; // Use 2 to represent RAID 1v
                else {
                    fprintf(stderr, "Invalid RAID mode.\n");
                    return 1;
                }
                break;
            case 'd':
                if (num_disks >= MAX_DISKS) {
                    fprintf(stderr, "Too many disks specified.\n");
                    return 1;
                }
                disk_files[num_disks++] = optarg;
                break;
            case 'i':
                num_inodes = atoi(optarg);
                if (num_inodes <= 0) {
                    fprintf(stderr, "Invalid number of inodes.\n");
                    return 1;
                }
                break;
            case 'b':
                num_data_blocks = atoi(optarg);
                if (num_data_blocks <= 0) {
                    fprintf(stderr, "Invalid number of data blocks.\n");
                    return 1;
                }
                break;
            default:
                fprintf(stderr, "Usage: %s -r [0|1|1v] -d disk1 -d disk2 ... -i num_inodes -b num_blocks\n", argv[0]);
                return 1;
        }
    }

    // Validate required arguments
    if (raid_mode == -1) {
        fprintf(stderr, "Error: No RAID mode specified.\n");
        return 1;
    }
    if (num_disks == 0) {
        fprintf(stderr, "Error: No disks specified.\n");
        return 1;
    }
    if (num_inodes == -1) {
        fprintf(stderr, "Error: Number of inodes not specified.\n");
        return 1;
    }
    if (num_data_blocks == -1) {
        fprintf(stderr, "Error: Number of data blocks not specified.\n");
        return 1;
    }

    // Determine minimum disks required
    int min_disks_required = 0;

    switch (raid_mode) {
        case 0: // RAID 0
            min_disks_required = 2;
            break;
        case 1: // RAID 1
        case 2: // RAID 1v
            min_disks_required = 2;
            break;
        default:
            fprintf(stderr, "Invalid RAID mode.\n");
            return 1;
    }

    // Check if sufficient disks are provided
    if (num_disks < min_disks_required) {
        fprintf(stderr, "Error: Not enough disks.\n");
        return 1;
    }

    // Round up inodes
    num_inodes = round_up_blocks(num_inodes);

    // Round up data blocks
    num_data_blocks = round_up_blocks(num_data_blocks);

    // Calculate sizes
    size_t superblock_size = sizeof(struct wfs_sb);
    size_t offset = 0;

    // Superblock
    off_t superblock_offset = offset;
    offset += superblock_size; // size of superblock

    // Inode bitmap
    off_t i_bitmap_ptr = offset;
    size_t inode_bitmap_bits = num_inodes;
    size_t i_bitmap_size = (inode_bitmap_bits + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    offset += i_bitmap_size;

    // Data bitmap
    off_t d_bitmap_ptr = offset;
    size_t data_bitmap_bits = num_data_blocks;
    size_t d_bitmap_size = (data_bitmap_bits + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    offset += d_bitmap_size;

    // Pad to next multiple of BLOCK_SIZE (512 bytes) for inodes
    if (offset % BLOCK_SIZE != 0) {
        size_t padding = BLOCK_SIZE - (offset % BLOCK_SIZE);
        offset += padding;
    }

    // Inode region
    off_t i_blocks_ptr = offset;
    size_t inode_region_size = num_inodes * INODE_SIZE;
    offset += inode_region_size;

    // Data blocks region
    off_t d_blocks_ptr = offset;
    size_t data_region_size = num_data_blocks * BLOCK_SIZE;
    offset += data_region_size;

    size_t fs_size = offset;

    // Map disks
    char *disk_maps[MAX_DISKS];
    int fds[MAX_DISKS];

    for (int i = 0; i < num_disks; i++) {
        int fd = open(disk_files[i], O_RDWR);
        if (fd == -1) {
            perror("open");
            return 1;
        }

        // Get file size
        struct stat st;
        if (fstat(fd, &st) == -1) {
            perror("fstat");
            return 1;
        }

        if (st.st_size < fs_size) {
            fprintf(stderr, "Error: Disk image %s is too small.\n", disk_files[i]);
            return -1; // Change this to return -1
        }

        // Map the disk image into memory
        char *disk_map = mmap(NULL, fs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_map == MAP_FAILED) {
            perror("mmap");
            return 1;
        }

        disk_maps[i] = disk_map;
        fds[i] = fd;
    }

    // Initialize superblock
    struct wfs_sb superblock;
    memset(&superblock, 0, sizeof(struct wfs_sb));

    superblock.num_inodes = num_inodes;
    superblock.num_data_blocks = num_data_blocks;
    superblock.i_bitmap_ptr = i_bitmap_ptr;
    superblock.d_bitmap_ptr = d_bitmap_ptr;
    superblock.i_blocks_ptr = i_blocks_ptr;
    superblock.d_blocks_ptr = d_blocks_ptr;
    superblock.raid_mode = raid_mode;
    superblock.num_disks = num_disks;

    // **Add Initialization of disk_order with Unique Disk IDs**
    for (int i = 0; i < num_disks; i++) {
        generate_disk_id(i, superblock.disk_order[i]);
        // Ensure null-termination
        superblock.disk_order[i][MAX_NAME - 1] = '\0';
    }

    // Write superblock to all disks
    for (int i = 0; i < num_disks; i++) {
        memcpy(disk_maps[i] + superblock_offset, &superblock, sizeof(struct wfs_sb));
    }

    // After writing the superblock, read back from disk to verify (optional)
    struct wfs_sb superblock_check;
    memcpy(&superblock_check, disk_maps[0] + superblock_offset, sizeof(struct wfs_sb));
    // You can add verification code here if needed

    // Initialize inode bitmap
    for (int i = 0; i < num_disks; i++) {
        char *inode_bitmap = disk_maps[i] + i_bitmap_ptr;
        memset(inode_bitmap, 0, i_bitmap_size);
        inode_bitmap[0] |= 0x01; // Mark inode 0 (root inode) as used
    }

    // Initialize data bitmap
    // for (int i = 0; i < num_disks; i++) {
    //     char *data_bitmap = disk_maps[i] + d_bitmap_ptr;
    //     memset(data_bitmap, 0, d_bitmap_size);
        
    //     // **Mark block 0 as used, reserved for root directory**
    //    // set_bit(data_bitmap, 0);
    // }

    // Initialize root inode
    struct wfs_inode root_inode;
    memset(&root_inode, 0, sizeof(struct wfs_inode));

    root_inode.num = 0;
    root_inode.mode = S_IFDIR | 0755; // Directory with permissions 755
    root_inode.uid = getuid();
    root_inode.gid = getgid();
    root_inode.size = 0; // Initially empty
    root_inode.nlinks = 2; // '.' and '..' links

    root_inode.atim = root_inode.mtim = root_inode.ctim = time(NULL);

    // Initialize blocks (no data blocks allocated yet)
    memset(root_inode.blocks, 0, sizeof(root_inode.blocks));

    // Write root inode to all disks
    off_t inode0_offset = i_blocks_ptr + 0 * INODE_SIZE;

    for (int i = 0; i < num_disks; i++) {
        char *inode_ptr = disk_maps[i] + inode0_offset;
        memcpy(inode_ptr, &root_inode, sizeof(struct wfs_inode));
    }

    // Clean up
    for (int i = 0; i < num_disks; i++) {
        munmap(disk_maps[i], fs_size);
        close(fds[i]);
    }

    return 0;
}
