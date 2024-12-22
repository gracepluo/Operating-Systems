#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "wfs.h"
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libgen.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <inttypes.h>

#define INODE_SIZE 512
#define BITS_PER_BYTE 8

static struct wfs_sb superblock;
static char *disk_maps[MAX_DISKS];
static int num_disks = 0;
static int raid_mode = -1;
static uint64_t num_inodes = 0;
static uint64_t num_data_blocks = 0;
static size_t fs_size = 0;
static int fd_disks[MAX_DISKS];

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

// Debug print methods
void print_superblock() {
    printf("[DEBUG] Superblock Information:\n");
    printf("Raid Mode: %d\n", superblock.raid_mode);
    printf("Number of Inodes: %" PRIu64 "\n", superblock.num_inodes);
    printf("Number of Data Blocks: %" PRIu64 "\n", superblock.num_data_blocks);
    printf("Number of Disks: %d\n", superblock.num_disks);
    printf("Inode Bitmap Pointer: %" PRIu64 "\n", superblock.i_bitmap_ptr);
    printf("Data Bitmap Pointer: %" PRIu64 "\n", superblock.d_bitmap_ptr);
    printf("Inode Blocks Pointer: %" PRIu64 "\n", superblock.i_blocks_ptr);
    printf("Data Blocks Pointer: %" PRIu64 "\n", superblock.d_blocks_ptr);
}

void dump_data_bitmap_comparison() {
    char *data_bitmap0 = disk_maps[0] + superblock.d_bitmap_ptr;
    int total_blocks = superblock.num_data_blocks;

    for (int d = 1; d < num_disks; d++) {
        char *data_bitmap_d = disk_maps[d] + superblock.d_bitmap_ptr;
        for (int i = 0; i < total_blocks; i++) {
            if (get_bit(data_bitmap0, i) != get_bit(data_bitmap_d, i)) {
                fprintf(stderr, "[ERROR] dump_data_bitmap_comparison: Mismatch at block %d between disk 0 and disk %d\n", i, d);
            }
        }
    }
    printf("[DEBUG] dump_data_bitmap_comparison: Completed\n");
}

// RAID functions
ssize_t raid_read(void *buf, off_t block_number, size_t size) {
    if (raid_mode == 0) {
        // RAID 0
        int stripe_index = block_number / num_disks;
        int disk_idx = block_number % num_disks;
        off_t disk_offset = superblock.d_blocks_ptr + stripe_index * BLOCK_SIZE;
        memcpy(buf, disk_maps[disk_idx] + disk_offset, size);
    } else if (raid_mode == 1) {
        // RAID 1
        memcpy(buf, disk_maps[0] + superblock.d_blocks_ptr + block_number * BLOCK_SIZE, size);
    } else if (raid_mode == 2) {
        // RAID 1v (Majority Voting)
        char temp_buf[MAX_DISKS][BLOCK_SIZE];
        int counts[MAX_DISKS] = {0};
        for (int i = 0; i < num_disks; i++) {
            memcpy(temp_buf[i], disk_maps[i] + superblock.d_blocks_ptr + block_number * BLOCK_SIZE, size);
        }
        // Majority voting
        for (int i = 0; i < num_disks; i++) {
            for (int j = i + 1; j < num_disks; j++) {
                if (memcmp(temp_buf[i], temp_buf[j], size) == 0) {
                    counts[i]++;
                    counts[j]++;
                }
            }
        }
        // Find the data with the highest count
        int max_idx = 0;
        for (int i = 1; i < num_disks; i++) {
            if (counts[i] > counts[max_idx]) {
                max_idx = i;
            }
        }
        memcpy(buf, temp_buf[max_idx], size);
    }
    return size;
}

ssize_t raid_write(void *buf, off_t block_number, size_t size) {
    if (raid_mode == 0) {
        // RAID 0
        int stripe_index = block_number / num_disks;
        int disk_idx = block_number % num_disks;
        off_t disk_offset = superblock.d_blocks_ptr + stripe_index * BLOCK_SIZE;
        memcpy(disk_maps[disk_idx] + disk_offset, buf, size);
    } else if (raid_mode == 1 || raid_mode == 2) {
        // RAID 1 and RAID 1v
        for (int i = 0; i < num_disks; i++) {
            memcpy(disk_maps[i] + superblock.d_blocks_ptr + block_number * BLOCK_SIZE, buf, size);
        }
    }
    return size;
}

// Inode operations
int load_inode(int inode_num, struct wfs_inode *inode) {
    off_t inode_offset = superblock.i_blocks_ptr + inode_num * INODE_SIZE;
    memcpy(inode, disk_maps[0] + inode_offset, sizeof(struct wfs_inode));
    fprintf(stderr, "[DEBUG] load_inode: Loaded inode %d at offset %ld\n", inode_num, inode_offset);
    return 0;
}

void print_directory_entries(int dir_inode_num) {
    struct wfs_inode dir_inode;
    load_inode(dir_inode_num, &dir_inode);

    printf("[DEBUG] Directory Entries for inode %d:\n", dir_inode_num);

    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    for (int i = 0; i < N_BLOCKS; i++) {
        if (dir_inode.blocks[i] == 0) continue;
        char block_buf[BLOCK_SIZE];
        raid_read(block_buf, dir_inode.blocks[i], BLOCK_SIZE);
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;

        for (int j = 0; j < entries_per_block; j++) {
            if (strlen(entries[j].name) == 0) continue;
            printf("[DEBUG] Entry: Name='%s', Inode=%d\n", entries[j].name, entries[j].num);
        }
    }
}

int store_inode(int inode_num, struct wfs_inode *inode) {
    off_t inode_offset = superblock.i_blocks_ptr + inode_num * INODE_SIZE;
    for (int i = 0; i < num_disks; i++) {
        memcpy(disk_maps[i] + inode_offset, inode, sizeof(struct wfs_inode));
    }
    fprintf(stderr, "[DEBUG] store_inode: Stored inode %d at offset %ld on all disks\n", inode_num, inode_offset);
    return 0;
}

int allocate_inode(void) {
    char *inode_bitmap = disk_maps[0] + superblock.i_bitmap_ptr;
    int total_inodes = superblock.num_inodes;
    for (int i = 0; i < total_inodes; i++) {
        if (!get_bit(inode_bitmap, i)) {
            set_bit(inode_bitmap, i);
            // Mirror the bitmap to other disks
            for (int j = 1; j < num_disks; j++) {
                set_bit(disk_maps[j] + superblock.i_bitmap_ptr, i);
            }
            fprintf(stderr, "[DEBUG] allocate_inode: Allocated inode %d\n", i);
            return i;
        }
    }
    fprintf(stderr, "[ERROR] allocate_inode: No free inodes available\n");
    return -ENOSPC;
}

void free_inode(int inode_num) {
    clear_bit(disk_maps[0] + superblock.i_bitmap_ptr, inode_num);
    for (int i = 1; i < num_disks; i++) {
        clear_bit(disk_maps[i] + superblock.i_bitmap_ptr, inode_num);
    }
    fprintf(stderr, "[DEBUG] free_inode: Freed inode %d\n", inode_num);
}

// Data block operations
int allocate_data_block(void) {
    char *data_bitmap = disk_maps[0] + superblock.d_bitmap_ptr;
    int total_blocks = superblock.num_data_blocks;
    for (int i = 1; i < total_blocks; i++) { // Start from block 1
        if (!get_bit(data_bitmap, i)) {
            set_bit(data_bitmap, i);
            // Mirror the bitmap to other disks (RAID 1 and RAID 1v)
            if (raid_mode == 1 || raid_mode == 2) {
                for (int j = 1; j < num_disks; j++) {
                    set_bit(disk_maps[j] + superblock.d_bitmap_ptr, i);
                }
            }
            fprintf(stderr, "[DEBUG] allocate_data_block: Allocated data block %d\n", i);
            
            // Dump the data bitmap and verify mirroring
            dump_data_bitmap_comparison();
            
            return i;
        }
    }
    fprintf(stderr, "[ERROR] allocate_data_block: No free data blocks available\n");
    return -ENOSPC;
}

void free_data_block(int block_num) {
    clear_bit(disk_maps[0] + superblock.d_bitmap_ptr, block_num);
    if (raid_mode == 1 || raid_mode == 2) {
        for (int i = 1; i < num_disks; i++) {
            clear_bit(disk_maps[i] + superblock.d_bitmap_ptr, block_num);
        }
    }
    fprintf(stderr, "[DEBUG] free_data_block: Freed data block %d\n", block_num);
}

// Indirect Block Helper Functions

int read_indirect_pointers(struct wfs_inode *inode, off_t *indirect_pointers) {
    if (inode->blocks[IND_BLOCK] == 0) {
        return -ENOENT; // Indirect block not allocated
    }

    char buf[BLOCK_SIZE];
    ssize_t res = raid_read(buf, inode->blocks[IND_BLOCK], BLOCK_SIZE);
    if (res != BLOCK_SIZE) {
        fprintf(stderr, "[ERROR] read_indirect_pointers: Failed to read indirect block %ld\n", inode->blocks[IND_BLOCK]);
        return -EIO; // I/O error
    }

    memcpy(indirect_pointers, buf, BLOCK_SIZE);
    return 0;
}

int write_indirect_pointers(struct wfs_inode *inode, off_t *indirect_pointers) {
    if (inode->blocks[IND_BLOCK] == 0) {
        return -ENOENT; // Indirect block not allocated
    }

    char buf[BLOCK_SIZE];
    memcpy(buf, indirect_pointers, BLOCK_SIZE);
    ssize_t res = raid_write(buf, inode->blocks[IND_BLOCK], BLOCK_SIZE);
    if (res != BLOCK_SIZE) {
        fprintf(stderr, "[ERROR] write_indirect_pointers: Failed to write indirect block %ld\n", inode->blocks[IND_BLOCK]);
        return -EIO; // I/O error
    }

    return 0;
}

int allocate_indirect_block(struct wfs_inode *inode) {
    if (inode->blocks[IND_BLOCK] != 0) {
        // Indirect block already allocated
        return 0;
    }

    int block_num = allocate_data_block();
    if (block_num < 0) {
        return block_num; // Propagate error
    }

    inode->blocks[IND_BLOCK] = block_num;

    // Initialize the indirect block with zeros
    char zero_block[BLOCK_SIZE];
    memset(zero_block, 0, BLOCK_SIZE);
    ssize_t res = raid_write(zero_block, block_num, BLOCK_SIZE);
    if (res != BLOCK_SIZE) {
        fprintf(stderr, "[ERROR] allocate_indirect_block: Failed to initialize indirect block %d\n", block_num);
        free_data_block(block_num); // Free allocated block on failure
        inode->blocks[IND_BLOCK] = 0;
        return -EIO; // I/O error
    }

    // Persist the updated inode
    store_inode(inode->num, inode);
    fprintf(stderr, "[DEBUG] allocate_indirect_block: Allocated indirect block %d for inode %d\n", block_num, inode->num);

    return 0;
}

int allocate_indirect_data_block(struct wfs_inode *inode, int indirect_index) {
    if (indirect_index >= INDIRECT_BLOCK_ENTRIES) {
        fprintf(stderr, "[ERROR] allocate_indirect_data_block: Indirect index %d out of range\n", indirect_index);
        return -EFBIG; // File too large
    }

    off_t indirect_pointers[INDIRECT_BLOCK_ENTRIES];
    int res = read_indirect_pointers(inode, indirect_pointers);
    if (res != 0) {
        return res; // Propagate error
    }

    if (indirect_pointers[indirect_index] != 0) {
        // Data block already allocated
        return indirect_pointers[indirect_index];
    }

    // Allocate a new data block
    int block_num = allocate_data_block();
    if (block_num < 0) {
        return block_num; // Propagate error
    }

    // Set the pointer in the indirect block
    indirect_pointers[indirect_index] = block_num;

    // Write back the updated indirect block
    res = write_indirect_pointers(inode, indirect_pointers);
    if (res != 0) {
        free_data_block(block_num); // Free allocated block on failure
        indirect_pointers[indirect_index] = 0;
        return res;
    }

    fprintf(stderr, "[DEBUG] allocate_indirect_data_block: Allocated indirect data block %d for inode %d at indirect index %d\n",
            block_num, inode->num, indirect_index);

    return block_num;
}

int free_indirect_blocks(struct wfs_inode *inode) {
    if (inode->blocks[IND_BLOCK] == 0) {
        return 0; // No indirect block to free
    }

    off_t indirect_pointers[INDIRECT_BLOCK_ENTRIES];
    int res = read_indirect_pointers(inode, indirect_pointers);
    if (res != 0) {
        return res; // Propagate error
    }

    // Free each data block referenced by the indirect block
    for (int i = 0; i < INDIRECT_BLOCK_ENTRIES; i++) {
        if (indirect_pointers[i] != 0) {
            free_data_block(indirect_pointers[i]);
            indirect_pointers[i] = 0;
        }
    }

    // Write back the zeroed indirect block
    char zero_block[BLOCK_SIZE];
    memset(zero_block, 0, BLOCK_SIZE);
    res = raid_write(zero_block, inode->blocks[IND_BLOCK], BLOCK_SIZE);
    if (res != BLOCK_SIZE) {
        fprintf(stderr, "[ERROR] free_indirect_blocks: Failed to zero indirect block %ld\n", inode->blocks[IND_BLOCK]);
        return -EIO; // I/O error
    }

    // Free the indirect block itself
    free_data_block(inode->blocks[IND_BLOCK]);
    fprintf(stderr, "[DEBUG] free_indirect_blocks: Freed indirect block %ld for inode %d\n", inode->blocks[IND_BLOCK], inode->num);
    inode->blocks[IND_BLOCK] = 0;

    // Persist the updated inode
    store_inode(inode->num, inode);

    return 0;
}

// Directory operations
int find_dentry(struct wfs_inode *dir_inode, const char *name, struct wfs_dentry *dentry) {
    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);

    for (int i = 0; i < N_BLOCKS; i++) {
        if (dir_inode->blocks[i] == 0) continue;
        char block_buf[BLOCK_SIZE];
        raid_read(block_buf, dir_inode->blocks[i], BLOCK_SIZE);
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;

        for (int j = 0; j < entries_per_block; j++) {
            if (strlen(entries[j].name) == 0) continue;
            if (strcmp(entries[j].name, name) == 0) {
                if (dentry != NULL) {
                    *dentry = entries[j];
                }
                fprintf(stderr, "[DEBUG] find_dentry: Found dentry '%s' (inode %d) in directory inode %d\n", name, entries[j].num, dir_inode->num);
                return 0; // Found
            }
        }
    }
    // If not found, log the error
    fprintf(stderr, "[ERROR] find_dentry: '%s' not found in directory inode %d\n", name, dir_inode->num);
    return -ENOENT; // Not found
}

int add_dentry(struct wfs_inode *dir_inode, const char *name, int inode_num) {
    struct wfs_dentry new_entry;
    memset(&new_entry, 0, sizeof(struct wfs_dentry));
    strncpy(new_entry.name, name, MAX_NAME - 1);
    new_entry.name[MAX_NAME - 1] = '\0';
    new_entry.num = inode_num;

    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    int total_entries = dir_inode->size / sizeof(struct wfs_dentry);
    int block_idx = total_entries / entries_per_block;
    int entry_idx = total_entries % entries_per_block;

    fprintf(stderr, "[DEBUG] add_dentry: total_entries=%d, block_idx=%d, entry_idx=%d\n", total_entries, block_idx, entry_idx);

    if (block_idx >= N_BLOCKS) {
        fprintf(stderr, "[ERROR] add_dentry: No space to add '%s' in directory inode %d\n", name, dir_inode->num);
        return -ENOSPC;
    }

    if (dir_inode->blocks[block_idx] == 0) { // Check if block is allocated
        int block_num = allocate_data_block();
        if (block_num < 0) {
            fprintf(stderr, "[ERROR] add_dentry: Failed to allocate data block for '%s'\n", name);
            return block_num;
        }
        dir_inode->blocks[block_idx] = block_num;
        fprintf(stderr, "[DEBUG] add_dentry: Allocated block %d for directory inode %d\n", block_num, dir_inode->num);
    }

    char block_buf[BLOCK_SIZE];
    raid_read(block_buf, dir_inode->blocks[block_idx], BLOCK_SIZE);
    struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;

    entries[entry_idx] = new_entry;
    raid_write(block_buf, dir_inode->blocks[block_idx], BLOCK_SIZE);
    fprintf(stderr, "[DEBUG] add_dentry: Wrote dentry '%s' to block_idx=%d, entry_idx=%d\n", name, block_idx, entry_idx);

    // Increment size by the size of one directory entry
    dir_inode->size += sizeof(struct wfs_dentry);
    fprintf(stderr, "[DEBUG] add_dentry: Updated directory inode %d size to %ld\n", dir_inode->num, dir_inode->size);

    // Persist parent's updated inode (with possibly new block and updated size)
    store_inode(dir_inode->num, dir_inode);
    fprintf(stderr, "[DEBUG] add_dentry: Added dentry '%s' (inode %d) to directory inode %d\n", name, inode_num, dir_inode->num);

    // Print directory entries for verification
    print_directory_entries(dir_inode->num);

    return 0;
}

int remove_dentry(struct wfs_inode *dir_inode, const char *name) {
    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);

    for (int i = 0; i < N_BLOCKS; i++) {
        if (dir_inode->blocks[i] == 0) continue;
        char block_buf[BLOCK_SIZE];
        raid_read(block_buf, dir_inode->blocks[i], BLOCK_SIZE);
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;

        for (int j = 0; j < entries_per_block; j++) {
            if (strlen(entries[j].name) == 0) continue;
            if (strcmp(entries[j].name, name) == 0) {
                // Remove the entry
                memset(&entries[j], 0, sizeof(struct wfs_dentry));
                raid_write(block_buf, dir_inode->blocks[i], BLOCK_SIZE);
                fprintf(stderr, "[DEBUG] remove_dentry: Removed dentry '%s' from directory inode %d\n", name, dir_inode->num);
                return 0;
            }
        }
    }
    // If not found, log the error
    fprintf(stderr, "[ERROR] remove_dentry: '%s' not found in directory inode %d\n", name, dir_inode->num);
    return -ENOENT;
}

// Path traversal
int traverse_path(const char *path, struct wfs_inode *inode, int *inode_num) {
    if (strcmp(path, "/") == 0) {
        load_inode(0, inode);
        if (inode_num) *inode_num = 0;
        return 0;
    }

    char *path_copy = strdup(path);
    if (!path_copy) {
        fprintf(stderr, "[ERROR] traverse_path: strdup failed for path '%s'\n", path);
        return -ENOMEM;
    }

    char *rest = path_copy;
    char *token;

    // Start from root inode
    struct wfs_inode current_inode;
    int current_inode_num = 0;
    load_inode(current_inode_num, &current_inode);

    while ((token = strsep(&rest, "/")) != NULL) {
        if (strlen(token) == 0) continue;

        if ((current_inode.mode & S_IFDIR) == 0) {
            fprintf(stderr, "[ERROR] traverse_path: '%s' is not a directory in path '%s'\n", token, path);
            free(path_copy);
            return -ENOTDIR;
        }
        struct wfs_dentry dentry;
        int res = find_dentry(&current_inode, token, &dentry);
        if (res != 0) {
            fprintf(stderr, "[ERROR] traverse_path: '%s' not found in path '%s'\n", token, path);
            free(path_copy);
            return -ENOENT;
        }
        current_inode_num = dentry.num;
        load_inode(current_inode_num, &current_inode);
    }

    if (inode) *inode = current_inode;
    if (inode_num) *inode_num = current_inode_num;
    fprintf(stderr, "[DEBUG] traverse_path: Successfully traversed to path '%s' (inode %d)\n", path, current_inode_num);
    free(path_copy);
    return 0;
}

// FUSE initialization function
static void *wfs_init(struct fuse_conn_info *conn) {
    (void) conn;
    fprintf(stderr, "[DEBUG] init: Called\n");
    
    struct wfs_inode root_inode;
    load_inode(0, &root_inode);
    
    if (!(root_inode.mode & S_IFDIR) || root_inode.size < sizeof(struct wfs_dentry) * 2) {
        fprintf(stderr, "[DEBUG] init: Root inode not properly initialized. Initializing now.\n");
        // Initialize root inode as directory
        root_inode.mode = S_IFDIR | 0755;
        root_inode.nlinks = 2; // '.' and '..'
        root_inode.uid = getuid();
        root_inode.gid = getgid();
        root_inode.size = sizeof(struct wfs_dentry) * 2; // For '.' and '..'
        root_inode.atim = root_inode.mtim = root_inode.ctim = time(NULL);
        memset(root_inode.blocks, 0, sizeof(root_inode.blocks));
        
        // Allocate block 0 for root directory
        int block_num = 0; // Allocate block 0
        root_inode.blocks[0] = block_num;
        
        // Initialize '.' and '..' entries
        char block_buf[BLOCK_SIZE];
        memset(block_buf, 0, BLOCK_SIZE);
        
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;
        
        // '.' entry
        strncpy(entries[0].name, ".", MAX_NAME - 1);
        entries[0].name[MAX_NAME - 1] = '\0';
        entries[0].num = 0; // Root inode number
        
        // '..' entry
        strncpy(entries[1].name, "..", MAX_NAME - 1);
        entries[1].name[MAX_NAME - 1] = '\0';
        entries[1].num = 0; // Root inode number
        
        raid_write(block_buf, block_num, BLOCK_SIZE);
        
        // Store the updated root inode
        store_inode(0, &root_inode);
        fprintf(stderr, "[DEBUG] init: Root inode initialized as directory with inode number 0\n");
        
        // Verify by dumping directory entries
        print_directory_entries(0);
    } else {
        fprintf(stderr, "[DEBUG] init: Root inode already properly initialized\n");
        // Optionally, verify directory entries
        print_directory_entries(0);
    }
    
    return NULL;
}

// FUSE operations
static int wfs_getattr(const char *path, struct stat *stbuf) {
    fprintf(stderr, "[DEBUG] getattr called for path: '%s'\n", path);
    memset(stbuf, 0, sizeof(struct stat));

    struct wfs_inode inode;
    int res = traverse_path(path, &inode, NULL);
    if (res != 0) {
        fprintf(stderr, "[DEBUG] getattr error: traverse_path failed for path '%s' with error %d\n", path, res);
        return res;
    }

    // Log retrieved inode information
    fprintf(stderr, "[DEBUG] getattr: Retrieved inode for path '%s': mode=%o, nlinks=%d, uid=%d, gid=%d, size=%ld, atim=%ld, mtim=%ld, ctim=%ld\n",
            path, inode.mode, inode.nlinks, inode.uid, inode.gid, inode.size,
            inode.atim, inode.mtim, inode.ctim);

    stbuf->st_mode = inode.mode;
    stbuf->st_nlink = inode.nlinks;
    stbuf->st_uid = inode.uid;
    stbuf->st_gid = inode.gid;
    stbuf->st_size = inode.size;
    stbuf->st_atime = inode.atim;
    stbuf->st_mtime = inode.mtim;
    stbuf->st_ctime = inode.ctim;
    stbuf->st_blocks = (inode.size + 511) / 512;
    stbuf->st_blksize = 512;

    fprintf(stderr, "[DEBUG] getattr: Completed for path '%s'\n", path);
    return 0;
}

int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void) dev; // Unused parameter

    fprintf(stderr, "[DEBUG] wfs_mknod: Called with path='%s', mode=%o\n", path, mode);

    char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        fprintf(stderr, "[ERROR] wfs_mknod: strdup failed for path '%s'\n", path);
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }
    char *dir_name = dirname(path_copy1);
    char *base_name = basename(path_copy2);

    char dir_path[strlen(dir_name) + 1];
    strcpy(dir_path, dir_name);

    fprintf(stderr, "[DEBUG] wfs_mknod: Directory path='%s', Base name='%s'\n", dir_path, base_name);

    struct wfs_inode parent_inode;
    int parent_inode_num;
    int res = traverse_path(dir_path, &parent_inode, &parent_inode_num);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_mknod: Failed to traverse to parent directory '%s' with error %d\n", dir_path, res);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    if ((parent_inode.mode & S_IFDIR) == 0) {
        fprintf(stderr, "[ERROR] wfs_mknod: Parent path '%s' is not a directory\n", dir_path);
        free(path_copy1);
        free(path_copy2);
        return -ENOTDIR;
    }

    // Check if file already exists
    res = find_dentry(&parent_inode, base_name, NULL);
    if (res == 0) {
        fprintf(stderr, "[ERROR] wfs_mknod: File '%s' already exists in directory inode %d\n", base_name, parent_inode.num);
        free(path_copy1);
        free(path_copy2);
        return -EEXIST;
    }

    // Allocate new inode
    int new_inode_num = allocate_inode();
    if (new_inode_num < 0) {
        fprintf(stderr, "[ERROR] wfs_mknod: Failed to allocate inode for '%s'\n", base_name);
        free(path_copy1);
        free(path_copy2);
        return new_inode_num;
    }

    struct wfs_inode new_inode;
    memset(&new_inode, 0, sizeof(struct wfs_inode));
    new_inode.num = new_inode_num;
    new_inode.mode = mode;
    new_inode.uid = getuid();
    new_inode.gid = getgid();
    new_inode.size = 0; // **Set size to 0 for lazy allocation**
    new_inode.atim = new_inode.mtim = new_inode.ctim = time(NULL);

    if (mode & S_IFDIR) {
        // Directory-specific initialization
        new_inode.nlinks = 2;  // '.' and '..'
        fprintf(stderr, "[DEBUG] wfs_mknod: Initialized directory inode %d with nlinks=%d\n", new_inode_num, new_inode.nlinks);
    } else {
        // Regular file
        new_inode.nlinks = 1;
        fprintf(stderr, "[DEBUG] wfs_mknod: Initialized file inode %d with nlinks=%d\n", new_inode_num, new_inode.nlinks);
    }

    // Store new inode
    store_inode(new_inode_num, &new_inode);

    // Add entry to parent directory
    res = add_dentry(&parent_inode, base_name, new_inode_num);
    if (res != 0) {
        // If we failed to add the directory entry, free the inode
        fprintf(stderr, "[ERROR] wfs_mknod: Failed to add dentry for '%s' with error %d\n", base_name, res);
        free_inode(new_inode_num);
        // If it was a directory and we incremented parent's link count, revert it
        if (mode & S_IFDIR) {
            parent_inode.nlinks--;
            store_inode(parent_inode_num, &parent_inode);
            fprintf(stderr, "[DEBUG] wfs_mknod: Reverted parent's link count for inode %d\n", parent_inode_num);
        }
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    // Update parent inode times
    parent_inode.mtim = parent_inode.ctim = time(NULL);
    store_inode(parent_inode_num, &parent_inode);
    fprintf(stderr, "[DEBUG] wfs_mknod: Updated parent inode %d's mtim and ctim\n", parent_inode_num);

    free(path_copy1);
    free(path_copy2);
    fprintf(stderr, "[DEBUG] wfs_mknod: Successfully created '%s' (inode %d)\n", path, new_inode_num);
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    fprintf(stderr, "[DEBUG] wfs_mkdir: Called with path='%s', mode=%o\n", path, mode);
    int res = wfs_mknod(path, mode | S_IFDIR, 0);
    if (res == 0) {
        fprintf(stderr, "[DEBUG] wfs_mkdir: Successfully created directory '%s'\n", path);
    } else {
        fprintf(stderr, "[ERROR] wfs_mkdir: Failed to create directory '%s' with error %d\n", path, res);
    }
    return res;
}

static int wfs_unlink(const char *path) {
    fprintf(stderr, "[DEBUG] wfs_unlink: Called with path='%s'\n", path);

    char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        fprintf(stderr, "[ERROR] wfs_unlink: strdup failed for path '%s'\n", path);
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }
    char *dir_name = dirname(path_copy1);
    char *base_name = basename(path_copy2);

    char dir_path[strlen(dir_name) + 1];
    strcpy(dir_path, dir_name);

    fprintf(stderr, "[DEBUG] wfs_unlink: Directory path='%s', Base name='%s'\n", dir_path, base_name);

    struct wfs_inode parent_inode;
    int parent_inode_num;
    int res = traverse_path(dir_path, &parent_inode, &parent_inode_num);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_unlink: Failed to traverse to parent directory '%s' with error %d\n", dir_path, res);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    struct wfs_dentry dentry;
    res = find_dentry(&parent_inode, base_name, &dentry);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_unlink: File '%s' not found in directory inode %d\n", base_name, parent_inode.num);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    // Load inode to be unlinked
    struct wfs_inode target_inode;
    load_inode(dentry.num, &target_inode);

    if ((target_inode.mode & S_IFDIR) != 0) {
        fprintf(stderr, "[ERROR] wfs_unlink: '%s' is a directory, not a file\n", base_name);
        free(path_copy1);
        free(path_copy2);
        return -EISDIR;
    }

    // Remove dentry from parent directory
    res = remove_dentry(&parent_inode, base_name);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_unlink: Failed to remove dentry for '%s'\n", base_name);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    // Free direct data blocks
    for (int i = 0; i < D_BLOCK; i++) {
        if (target_inode.blocks[i] != 0) {
            free_data_block(target_inode.blocks[i]);
            target_inode.blocks[i] = 0;
        }
    }

    // Free indirect blocks
    res = free_indirect_blocks(&target_inode);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_unlink: Failed to free indirect blocks for inode %d\n", target_inode.num);
        // Proceeding even if indirect blocks failed to free
    }

    // Free inode
    free_inode(target_inode.num);
    fprintf(stderr, "[DEBUG] wfs_unlink: Freed inode %d and its data blocks\n", target_inode.num);

    // Update parent inode times
    parent_inode.mtim = parent_inode.ctim = time(NULL);
    store_inode(parent_inode_num, &parent_inode);
    fprintf(stderr, "[DEBUG] wfs_unlink: Updated parent inode %d's mtim and ctim\n", parent_inode_num);

    free(path_copy1);
    free(path_copy2);
    fprintf(stderr, "[DEBUG] wfs_unlink: Successfully unlinked '%s'\n", path);
    return 0;
}

static int wfs_rmdir(const char *path) {
    fprintf(stderr, "[DEBUG] wfs_rmdir: Called with path='%s'\n", path);

    char *path_copy1 = strdup(path);
    char *path_copy2 = strdup(path);
    if (!path_copy1 || !path_copy2) {
        fprintf(stderr, "[ERROR] wfs_rmdir: strdup failed for path '%s'\n", path);
        free(path_copy1);
        free(path_copy2);
        return -ENOMEM;
    }
    char *dir_name = dirname(path_copy1);
    char *base_name = basename(path_copy2);

    char dir_path[strlen(dir_name) + 1];
    strcpy(dir_path, dir_name);

    fprintf(stderr, "[DEBUG] wfs_rmdir: Directory path='%s', Base name='%s'\n", dir_path, base_name);

    struct wfs_inode parent_inode;
    int parent_inode_num;
    int res = traverse_path(dir_path, &parent_inode, &parent_inode_num);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_rmdir: Failed to traverse to parent directory '%s' with error %d\n", dir_path, res);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    struct wfs_dentry dentry;
    res = find_dentry(&parent_inode, base_name, &dentry);
    if (res != 0) {
        fprintf(stderr, "[ERROR] wfs_rmdir: Directory '%s' not found in directory inode %d\n", base_name, parent_inode.num);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    // Load inode to be removed
    struct wfs_inode target_inode;
    load_inode(dentry.num, &target_inode);

    if ((target_inode.mode & S_IFDIR) == 0) {
        fprintf(stderr, "[ERROR] wfs_rmdir: '%s' is not a directory\n", base_name);
        free(path_copy1);
        free(path_copy2);
        return -ENOTDIR;
    }

    // Check if directory is empty
    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);
    int is_empty = 1;
    for (int i = 0; i < N_BLOCKS; i++) {
        if (target_inode.blocks[i] == 0) continue;
        char block_buf[BLOCK_SIZE];
        raid_read(block_buf, target_inode.blocks[i], BLOCK_SIZE);
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;
        for (int j = 0; j < entries_per_block; j++) {
            if (strlen(entries[j].name) != 0) {
                // Allow only '.' and '..' if they exist
                if (strcmp(entries[j].name, ".") != 0 && strcmp(entries[j].name, "..") != 0) {
                    is_empty = 0;
                    break;
                }
            }
        }
        if (!is_empty) break;
    }
    if (!is_empty) {
        fprintf(stderr, "[ERROR] wfs_rmdir: Directory '%s' is not empty\n", base_name);
        free(path_copy1);
        free(path_copy2);
        return -ENOTEMPTY;
    }

    // Remove dentry from parent directory
    res = remove_dentry(&parent_inode, base_name);
    if (res != 0) {
       //fprintf(stderr, "[ERROR] wfs_rmdir: Failed to remove dentry for directory '%s'\n", base_name, res);
        free(path_copy1);
        free(path_copy2);
        return res;
    }

    // Decrement parent's link count
    parent_inode.nlinks--;
    fprintf(stderr, "[DEBUG] wfs_rmdir: Decremented parent inode %d's nlinks to %d\n", parent_inode_num, parent_inode.nlinks);

    // Free data blocks of directory
    for (int i = 0; i < N_BLOCKS; i++) {
        if (target_inode.blocks[i] != 0) {
            free_data_block(target_inode.blocks[i]);
            target_inode.blocks[i] = 0;
        }
    }

    // Free inode
    free_inode(target_inode.num);
    fprintf(stderr, "[DEBUG] wfs_rmdir: Freed inode %d and its data blocks\n", target_inode.num);

    // Update parent inode times
    parent_inode.mtim = parent_inode.ctim = time(NULL);
    store_inode(parent_inode_num, &parent_inode);
    fprintf(stderr, "[DEBUG] wfs_rmdir: Updated parent inode %d's mtim and ctim\n", parent_inode_num);

    free(path_copy1);
    free(path_copy2);
    fprintf(stderr, "[DEBUG] wfs_rmdir: Successfully removed directory '%s'\n", path);
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter
    fprintf(stderr, "[DEBUG] wfs_read: Called with path='%s', size=%zu, offset=%ld\n", path, size, offset);

    struct wfs_inode inode;
    int res = traverse_path(path, &inode, NULL);
    if (res != 0) {
        fprintf(stderr, "[DEBUG] wfs_read error: traverse_path failed for path '%s' with error %d\n", path, res);
        return res;
    }

    if ((inode.mode & S_IFREG) == 0) {
        fprintf(stderr, "[ERROR] wfs_read: '%s' is not a regular file\n", path);
        return -EISDIR;
    }

    if (offset >= inode.size) {
        fprintf(stderr, "[DEBUG] wfs_read: Offset %ld >= file size %ld, returning 0 bytes\n", offset, inode.size);
        return 0;
    }

    if (offset + size > inode.size) {
        size = inode.size - offset;
    }

    size_t bytes_read = 0;
    while (size > 0) {
        int block_index = offset / BLOCK_SIZE;
        int block_offset = offset % BLOCK_SIZE;

        if (block_index < D_BLOCK) {
            // Handle direct blocks
            if (inode.blocks[block_index] == 0) {
                fprintf(stderr, "[DEBUG] wfs_read: Direct block %d not allocated\n", block_index);
                break;
            }

            char block_buf[BLOCK_SIZE];
            raid_read(block_buf, inode.blocks[block_index], BLOCK_SIZE);

            size_t to_read = BLOCK_SIZE - block_offset;
            if (to_read > size) {
                to_read = size;
            }

            memcpy(buf + bytes_read, block_buf + block_offset, to_read);

            size -= to_read;
            offset += to_read;
            bytes_read += to_read;

        } else if (block_index < D_BLOCK + INDIRECT_BLOCK_ENTRIES) {
            // Handle indirect blocks
            int indirect_index = block_index - D_BLOCK;

            if (inode.blocks[IND_BLOCK] == 0) {
                fprintf(stderr, "[DEBUG] wfs_read: Indirect block not allocated\n");
                break;
            }

            // Read indirect pointers
            off_t indirect_pointers[INDIRECT_BLOCK_ENTRIES];
            res = read_indirect_pointers(&inode, indirect_pointers);
            if (res != 0) {
                break;
            }

            if (indirect_pointers[indirect_index] == 0) {
                fprintf(stderr, "[DEBUG] wfs_read: Indirect data block %d not allocated\n", indirect_index);
                break;
            }

            int data_block_num = indirect_pointers[indirect_index];
            char data_block_buf[BLOCK_SIZE];
            raid_read(data_block_buf, data_block_num, BLOCK_SIZE);

            size_t to_read = BLOCK_SIZE - block_offset;
            if (to_read > size) {
                to_read = size;
            }

            memcpy(buf + bytes_read, data_block_buf + block_offset, to_read);

            size -= to_read;
            offset += to_read;
            bytes_read += to_read;

        } else {
            // Exceeds supported blocks (direct + single indirect)
            fprintf(stderr, "[ERROR] wfs_read: Exceeds maximum file size for '%s'\n", path);
            return bytes_read; // Alternatively, return -EFBIG
        }
    }

    fprintf(stderr, "[DEBUG] wfs_read: Read %zu bytes from '%s'\n", bytes_read, path);
    return bytes_read;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter
    fprintf(stderr, "[DEBUG] wfs_write: Called with path='%s', size=%zu, offset=%ld\n", path, size, offset);

    struct wfs_inode inode;
    int res = traverse_path(path, &inode, NULL);
    if (res != 0) {
        fprintf(stderr, "[DEBUG] wfs_write error: traverse_path failed for path '%s' with error %d\n", path, res);
        return res;
    }

    if ((inode.mode & S_IFREG) == 0) {
        fprintf(stderr, "[ERROR] wfs_write: '%s' is not a regular file\n", path);
        return -EISDIR;
    }

    size_t bytes_written = 0;
    while (size > 0) {
        int block_index = offset / BLOCK_SIZE;
        int block_offset = offset % BLOCK_SIZE;

        if (block_index < D_BLOCK) {
            // Handle direct blocks
            if (inode.blocks[block_index] == 0) {
                int block_num = allocate_data_block();
                if (block_num < 0) {
                    fprintf(stderr, "[ERROR] wfs_write: Failed to allocate data block for '%s'\n", path);
                    break;
                }
                inode.blocks[block_index] = block_num;
                fprintf(stderr, "[DEBUG] wfs_write: Allocated direct block %d for file inode %d\n", block_num, inode.num);
            }

            char block_buf[BLOCK_SIZE];
            raid_read(block_buf, inode.blocks[block_index], BLOCK_SIZE);

            size_t to_write = BLOCK_SIZE - block_offset;
            if (to_write > size) {
                to_write = size;
            }

            memcpy(block_buf + block_offset, buf + bytes_written, to_write);
            raid_write(block_buf, inode.blocks[block_index], BLOCK_SIZE);

            size -= to_write;
            offset += to_write;
            bytes_written += to_write;

        } else if (block_index < D_BLOCK + INDIRECT_BLOCK_ENTRIES) {
            // Handle indirect blocks
            int indirect_index = block_index - D_BLOCK;

            // Allocate the indirect block if not already allocated
            res = allocate_indirect_block(&inode);
            if (res != 0) {
                fprintf(stderr, "[ERROR] wfs_write: Failed to allocate indirect block for '%s'\n", path);
                break;
            }

            // Allocate the data block via indirect block
            int data_block_num = allocate_indirect_data_block(&inode, indirect_index);
            if (data_block_num < 0) {
                fprintf(stderr, "[ERROR] wfs_write: Failed to allocate indirect data block for '%s' at indirect index %d\n", path, indirect_index);
                break;
            }

            char data_block_buf[BLOCK_SIZE];
            raid_read(data_block_buf, data_block_num, BLOCK_SIZE);

            size_t to_write = BLOCK_SIZE - block_offset;
            if (to_write > size) {
                to_write = size;
            }

            memcpy(data_block_buf + block_offset, buf + bytes_written, to_write);
            raid_write(data_block_buf, data_block_num, BLOCK_SIZE);

            size -= to_write;
            offset += to_write;
            bytes_written += to_write;

        } else {
            // Exceeds supported blocks (direct + single indirect)
            fprintf(stderr, "[ERROR] wfs_write: Exceeds maximum file size for '%s'\n", path);
            return -EFBIG;
        }
    }

    // Update inode size if necessary
    if (offset > inode.size) {
        fprintf(stderr, "[DEBUG] wfs_write: Updating inode %d size from %ld to %ld\n", inode.num, inode.size, offset);
        inode.size = offset;
    }
    inode.mtim = inode.ctim = time(NULL);
    store_inode(inode.num, &inode);
    fprintf(stderr, "[DEBUG] wfs_write: Updated inode %d's size to %ld\n", inode.num, inode.size);

    fprintf(stderr, "[DEBUG] wfs_write: Wrote %zu bytes to '%s'\n", bytes_written, path);
    return bytes_written;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;
    fprintf(stderr, "[DEBUG] wfs_readdir: Called with path='%s'\n", path);

    struct wfs_inode dir_inode;
    int res = traverse_path(path, &dir_inode, NULL);
    if (res != 0) {
        fprintf(stderr, "[DEBUG] wfs_readdir error: traverse_path failed for path '%s' with error %d\n", path, res);
        return res;
    }

    if ((dir_inode.mode & S_IFDIR) == 0) {
        fprintf(stderr, "[ERROR] wfs_readdir: '%s' is not a directory\n", path);
        return -ENOTDIR;
    }

    // Add . and ..
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    fprintf(stderr, "[DEBUG] wfs_readdir: Added '.' and '..'\n");

    int entries_per_block = BLOCK_SIZE / sizeof(struct wfs_dentry);

    for (int i = 0; i < N_BLOCKS; i++) {
        if (dir_inode.blocks[i] == 0) continue;
        char block_buf[BLOCK_SIZE];
        raid_read(block_buf, dir_inode.blocks[i], BLOCK_SIZE);
        struct wfs_dentry *entries = (struct wfs_dentry *)block_buf;

        for (int j = 0; j < entries_per_block; j++) {
            if (strlen(entries[j].name) == 0) continue;
            if (strcmp(entries[j].name, ".") == 0 || strcmp(entries[j].name, "..") == 0) continue;
            filler(buf, entries[j].name, NULL, 0);
            fprintf(stderr, "[DEBUG] wfs_readdir: Added entry '%s'\n", entries[j].name);
        }
    }

    fprintf(stderr, "[DEBUG] wfs_readdir: Completed for path '%s'\n", path);
    return 0;
}

static const struct fuse_operations wfs_oper = {
    .init       = wfs_init,
    .getattr    = wfs_getattr,
    .mknod      = wfs_mknod,
    .mkdir      = wfs_mkdir,
    .unlink     = wfs_unlink,
    .rmdir      = wfs_rmdir,
    .read       = wfs_read,
    .write      = wfs_write,
    .readdir    = wfs_readdir,
    .destroy    = NULL, 
};

// Helper function to find the index of a disk based on its unique ID
int find_disk_index_by_id(const char *disk_id) {
    for (int i = 0; i < superblock.num_disks; i++) {
        if (strncmp(superblock.disk_order[i], disk_id, MAX_NAME) == 0) {
            return i;
        }
    }
    return -1; // Not found
}

// Function to map disks based on unique disk IDs
int map_disks_based_on_ids(char *disk_ids[MAX_DISKS], char *ordered_disk_maps[MAX_DISKS]) {
    for (int i = 0; i < superblock.num_disks; i++) {
        int idx = find_disk_index_by_id(disk_ids[i]);
        if (idx == -1) {
            fprintf(stderr, "[ERROR] map_disks_based_on_ids: Disk with ID '%s' not found in superblock's disk_order array.\n", disk_ids[i]);
            return -EINVAL;
        }
        ordered_disk_maps[i] = disk_maps[idx];
    }
    return 0;
}

// Cleanup function
static void wfs_destroy(void *private_data) {
    (void) private_data; // Unused parameter
    fprintf(stderr, "[DEBUG] wfs_destroy: Called\n");

    for (int i = 0; i < num_disks; i++) {
        munmap(disk_maps[i], fs_size);
        close(fd_disks[i]);
        fprintf(stderr, "[DEBUG] wfs_destroy: Unmapped and closed disk %d\n", i);
    }
    fprintf(stderr, "[DEBUG] wfs_destroy: Cleanup completed\n");
}

// Main function
int main(int argc, char *argv[]) {
    if (argc < 4) { // At least two disks, FUSE options, and mount point
        fprintf(stderr, "Usage: %s disk1 [disk2 ...] [FUSE options] mount_point\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Extract disk images from argv
    int disk_argc = 0;
    while (disk_argc + 1 < argc && argv[disk_argc + 1][0] != '-') {
        disk_argc++;
    }

    if (disk_argc == 0) {
        fprintf(stderr, "[ERROR] main: No disks specified.\n");
        exit(EXIT_FAILURE);
    }

    num_disks = disk_argc;
    if (num_disks > MAX_DISKS) {
        fprintf(stderr, "[ERROR] main: Too many disks specified. Max allowed is %d.\n", MAX_DISKS);
        exit(EXIT_FAILURE);
    }

    size_t superblock_size = sizeof(struct wfs_sb);
    for (int i = 0; i < num_disks; i++) {
        fd_disks[i] = open(argv[i + 1], O_RDWR);
        if (fd_disks[i] == -1) {
            fprintf(stderr, "[ERROR] main: Failed to open disk '%s': %s\n", argv[i + 1], strerror(errno));
            exit(EXIT_FAILURE);
        }
        // fprintf(stderr, "[DEBUG] main: Opened disk '%s'\n", argv[i + 1]);

        // Get file size
        struct stat st;
        if (fstat(fd_disks[i], &st) == -1) {
            fprintf(stderr, "[ERROR] main: fstat failed for disk '%s': %s\n", argv[i + 1], strerror(errno));
            exit(EXIT_FAILURE);
        }

        fs_size = st.st_size;
        // fprintf(stderr, "[DEBUG] main: Disk '%s' size=%ld bytes\n", argv[i + 1], fs_size);

        disk_maps[i] = mmap(NULL, fs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_disks[i], 0);
        if (disk_maps[i] == MAP_FAILED) {
            fprintf(stderr, "[ERROR] main: mmap failed for disk '%s': %s\n", argv[i + 1], strerror(errno));
            exit(EXIT_FAILURE);
        }
        // fprintf(stderr, "[DEBUG] main: Mapped disk '%s' into memory\n", argv[i + 1]);

        // Read superblock from first disk
        if (i == 0) {
            memcpy(&superblock, disk_maps[0], superblock_size);
            raid_mode = superblock.raid_mode;
            num_inodes = superblock.num_inodes;
            num_data_blocks = superblock.num_data_blocks;
            // fprintf(stderr, "[DEBUG] main: Loaded superblock from disk '%s'\n", argv[i + 1]);
            // fprintf(stderr, "[DEBUG] main: raid_mode=%d, num_inodes=%" PRIu64 ", num_data_blocks=%" PRIu64 ", num_disks=%d\n",
            //         raid_mode, num_inodes, num_data_blocks, superblock.num_disks);
            //print_superblock();
        } else {
            // Verify that superblocks are consistent across disks
            struct wfs_sb temp_sb;
            memcpy(&temp_sb, disk_maps[i], superblock_size);
            if (memcmp(&temp_sb, &superblock, superblock_size) != 0) {
                fprintf(stderr, "[ERROR] main: Superblocks do not match across disks.\n");
                exit(EXIT_FAILURE);
            }
            // fprintf(stderr, "[DEBUG] main: Verified superblock consistency for disk '%s'\n", argv[i + 1]);
        }
    }

    // Verify number of disks
    if (num_disks != superblock.num_disks) {
        fprintf(stderr, "[ERROR] main: Incorrect number of disks provided. Expected %d, got %d.\n", superblock.num_disks, num_disks);
        exit(EXIT_FAILURE);
    }
    // fprintf(stderr, "[DEBUG] main: Number of disks verified as %d\n", num_disks);

    // Read unique disk IDs from each disk's superblock
    char *disk_ids[MAX_DISKS];
    for (int i = 0; i < num_disks; i++) {
        disk_ids[i] = (char *)malloc(MAX_NAME * sizeof(char));
        if (!disk_ids[i]) {
            fprintf(stderr, "[ERROR] main: Memory allocation failed for disk_ids[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        // Assume that disk_order[i] contains the unique ID for disk i
        strncpy(disk_ids[i], disk_maps[i] + offsetof(struct wfs_sb, disk_order[i]), MAX_NAME);
        disk_ids[i][MAX_NAME - 1] = '\0'; // Ensure null-termination
        // fprintf(stderr, "[DEBUG] main: Disk %d ID: %s\n", i, disk_ids[i]);
    }

    // Rearrange disk_maps based on disk_order in superblock
    char *ordered_disk_maps[MAX_DISKS];
    for (int i = 0; i < superblock.num_disks; i++) {
        int found = 0;
        for (int j = 0; j < num_disks; j++) {
            if (strncmp(superblock.disk_order[i], disk_ids[j], MAX_NAME) == 0) {
                ordered_disk_maps[i] = disk_maps[j];
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "[ERROR] main: Disk with ID '%s' not found among provided disks.\n", superblock.disk_order[i]);
            exit(EXIT_FAILURE);
        }
    }

    // Assign ordered_disk_maps to disk_maps
    for (int i = 0; i < superblock.num_disks; i++) {
        disk_maps[i] = ordered_disk_maps[i];
    }

    // Free allocated disk_ids
    for (int i = 0; i < num_disks; i++) {
        free(disk_ids[i]);
    }

    // Prepare FUSE arguments

    // Create a new argv array for FUSE that includes the program name and FUSE options
    int fuse_argc = argc - disk_argc;
    char **fuse_argv = malloc(sizeof(char*) * fuse_argc);
    if (!fuse_argv) {
        fprintf(stderr, "[ERROR] main: Memory allocation failed for fuse_argv.\n");
        exit(EXIT_FAILURE);
    }

    fuse_argv[0] = argv[0]; // Program name remains the same

    for (int i = 1; i < fuse_argc; i++) {
        fuse_argv[i] = argv[disk_argc + i];
    }

    // Additional Debugging for Argument Parsing
    // fprintf(stderr, "[DEBUG] main: Passing FUSE arguments: argc=%d\n", fuse_argc);
    // for (int i = 0; i < fuse_argc; i++) {
    //     fprintf(stderr, "[DEBUG] main: FUSE argv[%d] = '%s'\n", i, fuse_argv[i]);
    // }

    // Ensure that there is at least one FUSE argument (the mount point)
    if (fuse_argc < 1) {
        fprintf(stderr, "[ERROR] main: No mount point specified.\n");
        free(fuse_argv);
        exit(EXIT_FAILURE);
    }

    // Initialize FUSE operations structure
    struct fuse_operations *oper = malloc(sizeof(struct fuse_operations));
    if (!oper) {
        fprintf(stderr, "[ERROR] main: Memory allocation failed for fuse_operations.\n");
        free(fuse_argv);
        exit(EXIT_FAILURE);
    }
    memset(oper, 0, sizeof(struct fuse_operations));
    *oper = wfs_oper;
    oper->destroy = wfs_destroy;
    // fprintf(stderr, "[DEBUG] main: Initialized fuse_operations structure\n");

    // Initialize FUSE
    int ret = fuse_main(fuse_argc, fuse_argv, oper, NULL);
    // fprintf(stderr, "[DEBUG] main: fuse_main returned %d\n", ret);

    free(oper);
    free(fuse_argv);
    return ret;
}
