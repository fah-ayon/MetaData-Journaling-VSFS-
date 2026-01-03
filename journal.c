#define _XOPEN_SOURCE 500
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U
#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
#define DEFAULT_IMAGE "vsfs.img"

#define JOURNAL_MAGIC 0x4A524E4CU
#define REC_DATA      1
#define REC_COMMIT    2

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

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + 8 * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == 8, "journal_header must be 8 bytes");
_Static_assert(sizeof(struct rec_header) == 4, "rec_header must be 4 bytes");

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void pread_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread");
    }
}

static void pwrite_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite");
    }
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static int find_free_bit(const uint8_t *bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; ++i) {
        if (!bitmap_test(bitmap, i)) {
            return (int)i;
        }
    }
    return -1;
}

static void init_journal(int fd) {
    uint8_t journal_data[JOURNAL_BLOCKS * BLOCK_SIZE];
    
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        pread_block(fd, JOURNAL_BLOCK_IDX + i, journal_data + (i * BLOCK_SIZE));
    }
    
    struct journal_header *jhdr = (struct journal_header *)journal_data;
    
    if (jhdr->magic == JOURNAL_MAGIC) {
        return;
    }
    
    jhdr->magic = JOURNAL_MAGIC;
    jhdr->nbytes_used = sizeof(struct journal_header);
    
    pwrite_block(fd, JOURNAL_BLOCK_IDX, journal_data);
}

static uint8_t* read_journal(int fd) {
    uint8_t *journal_data = malloc(JOURNAL_BLOCKS * BLOCK_SIZE);
    if (!journal_data) {
        die("malloc journal");
    }
    
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        pread_block(fd, JOURNAL_BLOCK_IDX + i, journal_data + (i * BLOCK_SIZE));
    }
    
    return journal_data;
}

static void write_journal(int fd, const uint8_t *journal_data) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        pwrite_block(fd, JOURNAL_BLOCK_IDX + i, journal_data + (i * BLOCK_SIZE));
    }
}

static int append_data_record(uint8_t *journal_data, uint32_t block_no, const uint8_t *block_data) {
    struct journal_header *jhdr = (struct journal_header *)journal_data;
    uint32_t nbytes = jhdr->nbytes_used;
    
    uint32_t record_size = sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE;
    
    if (nbytes + record_size > JOURNAL_BLOCKS * BLOCK_SIZE) {
        fprintf(stderr, "Journal full! Please run './journal install' first.\n");
        return -1;
    }
    
    struct rec_header rec_hdr;
    rec_hdr.type = REC_DATA;
    rec_hdr.size = record_size;
    
    memcpy(journal_data + nbytes, &rec_hdr, sizeof(rec_hdr));
    nbytes += sizeof(rec_hdr);
    
    memcpy(journal_data + nbytes, &block_no, sizeof(block_no));
    nbytes += sizeof(block_no);
    
    memcpy(journal_data + nbytes, block_data, BLOCK_SIZE);
    nbytes += BLOCK_SIZE;
    
    jhdr->nbytes_used = nbytes;
    
    return 0;
}

static int append_commit_record(uint8_t *journal_data) {
    struct journal_header *jhdr = (struct journal_header *)journal_data;
    uint32_t nbytes = jhdr->nbytes_used;
    
    uint32_t record_size = sizeof(struct rec_header);
    
    if (nbytes + record_size > JOURNAL_BLOCKS * BLOCK_SIZE) {
        fprintf(stderr, "Journal full! Please run './journal install' first.\n");
        return -1;
    }
    
    struct rec_header rec_hdr;
    rec_hdr.type = REC_COMMIT;
    rec_hdr.size = record_size;
    
    memcpy(journal_data + nbytes, &rec_hdr, sizeof(rec_hdr));
    nbytes += sizeof(rec_hdr);
    
    jhdr->nbytes_used = nbytes;
    
    return 0;
}

static void cmd_create(int fd, const char *filename) {
    struct superblock sb;
    pread_block(fd, 0, &sb);
    
    init_journal(fd);
    
    uint8_t *journal_data = read_journal(fd);
    
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    pread_block(fd, INODE_BMAP_IDX, inode_bitmap);
    pread_block(fd, DATA_BMAP_IDX, data_bitmap);
    
    int free_inode = find_free_bit(inode_bitmap, sb.inode_count);
    if (free_inode < 0) {
        fprintf(stderr, "No free inodes available.\n");
        free(journal_data);
        return;
    }
    
    uint8_t root_data_block[BLOCK_SIZE];
    pread_block(fd, DATA_START_IDX, root_data_block);
    struct dirent *dirents = (struct dirent *)root_data_block;
    
    int free_entry = -1;
    uint32_t max_entries = BLOCK_SIZE / sizeof(struct dirent);
    for (uint32_t i = 0; i < max_entries; ++i) {
        if (dirents[i].inode == 0 && dirents[i].name[0] == '\0') {
            free_entry = (int)i;
            break;
        }
    }
    
    if (free_entry < 0) {
        fprintf(stderr, "Root directory is full.\n");
        free(journal_data);
        return;
    }
    
    uint8_t inode_block[BLOCK_SIZE];
    uint32_t inode_block_idx = free_inode / (BLOCK_SIZE / INODE_SIZE);
    uint32_t inode_offset = (free_inode % (BLOCK_SIZE / INODE_SIZE)) * INODE_SIZE;
    pread_block(fd, INODE_START_IDX + inode_block_idx, inode_block);
    
    struct inode new_inode_data;
    memset(&new_inode_data, 0, sizeof(new_inode_data));
    new_inode_data.type = 1;
    new_inode_data.links = 1;
    new_inode_data.size = 0;
    memset(new_inode_data.direct, 0, sizeof(new_inode_data.direct));
    time_t now = time(NULL);
    new_inode_data.ctime = (uint32_t)now;
    new_inode_data.mtime = (uint32_t)now;
    
    memcpy(inode_block + inode_offset, &new_inode_data, sizeof(struct inode));
    
    if (inode_block_idx == 0) {
        struct inode *root_inode = (struct inode *)inode_block;
        root_inode->size = (free_entry + 1) * sizeof(struct dirent);
        root_inode->mtime = (uint32_t)now;
    }
    
    bitmap_set(inode_bitmap, free_inode);
    
    dirents[free_entry].inode = (uint32_t)free_inode;
    strncpy(dirents[free_entry].name, filename, sizeof(dirents[free_entry].name) - 1);
    dirents[free_entry].name[sizeof(dirents[free_entry].name) - 1] = '\0';
    
    if (append_data_record(journal_data, INODE_BMAP_IDX, inode_bitmap) < 0) {
        free(journal_data);
        return;
    }
    
    if (append_data_record(journal_data, INODE_START_IDX + inode_block_idx, inode_block) < 0) {
        free(journal_data);
        return;
    }
    
    if (inode_block_idx != 0) {
        uint8_t root_inode_block[BLOCK_SIZE];
        pread_block(fd, INODE_START_IDX, root_inode_block);
        struct inode *root_inode = (struct inode *)root_inode_block;
        root_inode->size = (free_entry + 1) * sizeof(struct dirent);
        root_inode->mtime = (uint32_t)now;
        
        if (append_data_record(journal_data, INODE_START_IDX, root_inode_block) < 0) {
            free(journal_data);
            return;
        }
    }
    
    if (append_data_record(journal_data, DATA_START_IDX, root_data_block) < 0) {
        free(journal_data);
        return;
    }
    
    if (append_commit_record(journal_data) < 0) {
        free(journal_data);
        return;
    }
    
    write_journal(fd, journal_data);
    free(journal_data);
}

static void cmd_install(int fd) {
    uint8_t *journal_data = read_journal(fd);
    struct journal_header *jhdr = (struct journal_header *)journal_data;
    
    if (jhdr->magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Journal is not initialized.\n");
        free(journal_data);
        return;
    }
    
    if (jhdr->nbytes_used == sizeof(struct journal_header)) {
        free(journal_data);
        return;
    }
    
    uint32_t offset = sizeof(struct journal_header);
    int transaction_count = 0;
    
    while (offset < jhdr->nbytes_used) {
        if (offset + sizeof(struct rec_header) > jhdr->nbytes_used) {
            fprintf(stderr, "Incomplete record header at offset %u\n", offset);
            break;
        }
        
        struct rec_header *rec_hdr = (struct rec_header *)(journal_data + offset);
        
        if (rec_hdr->type == REC_DATA) {
            if (offset + sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE > jhdr->nbytes_used) {
                fprintf(stderr, "Incomplete data record at offset %u\n", offset);
                break;
            }
            
            uint32_t block_no;
            memcpy(&block_no, journal_data + offset + sizeof(struct rec_header), sizeof(block_no));
            
            uint8_t *block_data = journal_data + offset + sizeof(struct rec_header) + sizeof(uint32_t);
            
            pwrite_block(fd, block_no, block_data);
            
            offset += rec_hdr->size;
        }
        else if (rec_hdr->type == REC_COMMIT) {
            transaction_count++;
            offset += rec_hdr->size;
        }
        else {
            fprintf(stderr, "Unknown record type %u at offset %u\n", rec_hdr->type, offset);
            break;
        }
    }
    
    jhdr->nbytes_used = sizeof(struct journal_header);
    write_journal(fd, journal_data);
    free(journal_data);
    
    if (transaction_count > 0) {
        printf("Applied %d transaction(s) from journal.\n", transaction_count);
        printf("Journal cleared.\n");
    }
    
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [filename]\n", argv[0]);
        fprintf(stderr, "  create <filename>  - Create a file entry (log metadata)\n");
        fprintf(stderr, "  install            - Apply journaled updates to disk\n");
        return EXIT_FAILURE;
    }
    
    const char *command = argv[1];
    const char *image_path = DEFAULT_IMAGE;
    
    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }
    
    if (strcmp(command, "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s create <filename>\n", argv[0]);
            close(fd);
            return EXIT_FAILURE;
        }
        const char *filename = argv[2];
        cmd_create(fd, filename);
    }
    else if (strcmp(command, "install") == 0) {
        cmd_install(fd);
    }
    else {
        fprintf(stderr, "Unknown command '%s'\n", command);
        fprintf(stderr, "Valid commands: create, install\n");
        close(fd);
        return EXIT_FAILURE;
    }
    
    close(fd);
    return EXIT_SUCCESS;
}
