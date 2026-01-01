#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>

// --- 1. Constants & Structures (Must match mkfs.c and Slides) ---
#define BLOCK_SIZE 4096
#define INODE_SIZE 128
#define FS_MAGIC 0x56534653
#define JOURNAL_MAGIC 0x4A524E4C // "JRNL" in ASCII
#define DEFAULT_IMAGE "vsfs.img"

// Record Types
#define REC_DATA 1
#define REC_COMMIT 2

// Superblock
struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

// Inode
struct inode {
    uint16_t type;      // 0=free, 1=file, 2=dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2+2+4+8*4+4+4)];
};

// Directory Entry
#define NAME_LEN 28
struct dirent {
    uint32_t inode;     // 0 = unused
    char name[NAME_LEN];
};

// Journal Header
struct journal_header {
    uint32_t magic;       
    uint32_t nbytes_used; 
};

// Record Header
struct rec_header {
    uint32_t type; 
    uint32_t size; 
};

// --- Globals ---
uint8_t *disk_image;
struct superblock *sb;

// --- Helper Functions ---

// Get a pointer to a specific block index
void *get_block(uint32_t block_idx) {
    return disk_image + (block_idx * BLOCK_SIZE);
}

// Check if a specific bit is set (1) or clear (0)
int check_bit(void *bitmap_block, int idx) {
    uint8_t *bmap = (uint8_t *)bitmap_block;
    return (bmap[idx / 8] >> (idx % 8)) & 1;
}

// Set a specific bit to 1
void set_bit(void *bitmap_block, int idx) {
    uint8_t *bmap = (uint8_t *)bitmap_block;
    bmap[idx / 8] |= (1 << (idx % 8));
}

// --- Command: Create ---
void cmd_create(const char *filename) {
    // 1. Load Metadata pointers
    void *inode_bmap = get_block(sb->inode_bitmap);
    struct inode *inode_table = (struct inode *)get_block(sb->inode_start);
    
    // Get Root Directory (Inode 0)
    struct inode *root_inode = &inode_table[0]; 
    uint32_t root_data_blk_idx = root_inode->direct[0];
    void *root_dir_block = get_block(root_data_blk_idx);

    // 2. Find Free Inode (Scan Inode Bitmap)
    int free_inode_idx = -1;
    for (int i = 1; i < sb->inode_count; i++) { // Start at 1 (0 is root)
        if (check_bit(inode_bmap, i) == 0) {
            free_inode_idx = i;
            break;
        }
    }
    if (free_inode_idx == -1) { printf("Error: No free inodes\n"); exit(1); }

// 3. Find Free Directory Entry (Scan Root Directory Block)
    struct dirent *entries = (struct dirent *)root_dir_block;
    int free_dirent_idx = -1;
    int max_entries = BLOCK_SIZE / sizeof(struct dirent);
    
    for (int i = 0; i < max_entries; i++) {
        // FIX: Check if name is empty. Inode 0 is valid for Root ('.'), so we can't rely solely on inode==0.
        // Unused slots are memset to 0 by mkfs, so their name[0] will be '\0'.
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') { 
            free_dirent_idx = i;
            break;
        }
    }
    if (free_dirent_idx == -1) { printf("Error: Root directory full\n"); exit(1); }

    // 4. PREPARE TRANSACTIONS (In Memory Only)
    // We need 3 modified blocks: Inode Bitmap, Inode Table, Directory Data
    
    // A. Modify Inode Bitmap
    uint8_t new_bmap[BLOCK_SIZE];
    memcpy(new_bmap, inode_bmap, BLOCK_SIZE);
    set_bit(new_bmap, free_inode_idx);

// B. Modify Inode Table
    // Calculate which block of the inode table contains our free inode
    uint32_t inodes_per_block = BLOCK_SIZE / sizeof(struct inode);
    uint32_t itable_blk_offset = free_inode_idx / inodes_per_block;
    uint32_t inode_blk_idx = sb->inode_start + itable_blk_offset;
    
    uint8_t new_inode_blk[BLOCK_SIZE];
    memcpy(new_inode_blk, get_block(inode_blk_idx), BLOCK_SIZE);
    
    // 1. Update the Target Inode (The new file)
    struct inode *target_inode = (struct inode *)(new_inode_blk + 
                                 ((free_inode_idx % inodes_per_block) * sizeof(struct inode)));
    target_inode->type = 1; // File
    target_inode->links = 1;
    target_inode->size = 0;

    // 2. FIX: Update Root Inode Size (Inode 0)
    // Note: Inode 0 is always at the start of the first inode block.
    // Since we are allocating Inode 1, we are in that same block.
    if (inode_blk_idx == sb->inode_start) {
        struct inode *root_inode_ptr = (struct inode *)new_inode_blk; // Inode 0 is at offset 0
        
        // We are using the slot at 'free_dirent_idx'. 
        // We need to ensure size covers this slot.
        uint32_t needed_size = (free_dirent_idx + 1) * sizeof(struct dirent);
        
        if (root_inode_ptr->size < needed_size) {
            root_inode_ptr->size = needed_size;
            // printf("Debug: Updating Root Inode size to %d\n", needed_size);
        }
    }

    // C. Modify Directory Block
    uint8_t new_dir_blk[BLOCK_SIZE];
    memcpy(new_dir_blk, root_dir_block, BLOCK_SIZE);
    struct dirent *new_entries = (struct dirent *)new_dir_blk;
    new_entries[free_dirent_idx].inode = free_inode_idx;
    strncpy(new_entries[free_dirent_idx].name, filename, NAME_LEN);

    // 5. WRITE TO JOURNAL
    struct journal_header *jh = (struct journal_header *)get_block(sb->journal_block);
    void *journal_base = get_block(sb->journal_block);
    
    // Initialize journal if empty
    if (jh->nbytes_used == 0) {
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = sizeof(struct journal_header);
    }

    uint32_t offset = jh->nbytes_used;
    
    // Check for space (Header + BlockNum + BlockData) * 3 records + Commit
    size_t needed = 3 * (sizeof(struct rec_header) + 4 + BLOCK_SIZE) + sizeof(struct rec_header);
    if (offset + needed > 16 * BLOCK_SIZE) {
        printf("Error: Journal full. Run ./journal install first.\n");
        exit(1);
    }

    // Helper macro to write a data record
    // Format: [Header] [BlockNum] [4096 Bytes Data]
    #define WRITE_RECORD(target_blk_idx, data_source) { \
        struct rec_header rh = { .type = REC_DATA, .size = 4 + BLOCK_SIZE }; \
        memcpy(journal_base + offset, &rh, sizeof(rh)); offset += sizeof(rh); \
        memcpy(journal_base + offset, &target_blk_idx, 4); offset += 4; \
        memcpy(journal_base + offset, data_source, BLOCK_SIZE); offset += BLOCK_SIZE; \
    }

    WRITE_RECORD(sb->inode_bitmap, new_bmap); // 1. Bitmap
    WRITE_RECORD(inode_blk_idx, new_inode_blk); // 2. Inode Table
    WRITE_RECORD(root_data_blk_idx, new_dir_blk); // 3. Directory

    // Write Commit Record [cite: 64]
    struct rec_header rh_commit = { .type = REC_COMMIT, .size = 0 };
    memcpy(journal_base + offset, &rh_commit, sizeof(rh_commit)); 
    offset += sizeof(rh_commit);

    // Update Journal Header
    jh->nbytes_used = offset;
    printf("Successfully created transaction for '%s' (Inode %d)\n", filename, free_inode_idx);
}

// --- Command: Install ---
void cmd_install() {
    struct journal_header *jh = (struct journal_header *)get_block(sb->journal_block);
    void *journal_base = get_block(sb->journal_block);

    if (jh->magic != JOURNAL_MAGIC) {
        printf("Error: Invalid journal magic number.\n");
        return;
    }

    printf("Replaying journal...\n");

    uint32_t offset = sizeof(struct journal_header);
    
    // Buffer for current transaction data
    struct pending_write {
        uint32_t target_blk;
        void *data_ptr; // Pointer into the mmapped journal
    } buffer[16]; 
    int buf_idx = 0;

    while (offset < jh->nbytes_used) {
        struct rec_header *rh = (struct rec_header *)(journal_base + offset);
        
        if (rh->type == REC_DATA) {
            // Queue this write, but don't perform it until we see a COMMIT [cite: 67]
            uint32_t *blk_num = (uint32_t *)(journal_base + offset + sizeof(struct rec_header));
            void *data = (void *)(journal_base + offset + sizeof(struct rec_header) + 4);
            
            buffer[buf_idx].target_blk = *blk_num;
            buffer[buf_idx].data_ptr = data;
            buf_idx++;
            
            offset += sizeof(struct rec_header) + rh->size;
        
        } else if (rh->type == REC_COMMIT) {
            // Valid transaction found. Apply all buffered writes to real disk [cite: 81]
            for (int i = 0; i < buf_idx; i++) {
                memcpy(get_block(buffer[i].target_blk), buffer[i].data_ptr, BLOCK_SIZE);
            }
            printf("Applied transaction with %d block updates.\n", buf_idx);
            buf_idx = 0; // Reset buffer for next transaction
            offset += sizeof(struct rec_header);
        } else {
            // Corruption or end
            break;
        }
    }

    // Reset Journal [cite: 83]
    jh->nbytes_used = sizeof(struct journal_header);
    printf("Journal installed and cleared.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <create <name> | install>\n", argv[0]);
        return 1;
    }

    int fd = open(DEFAULT_IMAGE, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    fstat(fd, &st);
    
    // Map the whole disk image into memory for easy access
    disk_image = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk_image == MAP_FAILED) { perror("mmap"); return 1; }
    
    sb = (struct superblock *)disk_image;

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) { printf("Usage: create <filename>\n"); return 1; }
        cmd_create(argv[2]);
    } else if (strcmp(argv[1], "install") == 0) {
        cmd_install();
    } else {
        printf("Unknown command: %s\n", argv[1]);
    }

    munmap(disk_image, st.st_size);
    close(fd);
    return 0;
}
