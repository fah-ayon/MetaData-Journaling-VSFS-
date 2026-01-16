# VSFS: Very Simple File System

VSFS is a block-based filesystem designed for teaching systems programming and operating systems concepts. It uses a fixed disk layout, write-ahead journaling, and a validator to maintain consistency.

## System Architecture

The filesystem operates on a virtual disk image with a fixed architecture.

### Disk Layout

The disk consists of 85 blocks, each 4096 bytes in size.

- **Superblock (Block 0)**: Stores the filesystem magic number (`0x56534653`), block size (4096), and offsets for all other regions.
- **Journal (Blocks 1-16)**: A dedicated region for storing transaction logs to ensure atomic metadata updates.
- **Inode Bitmap (Block 17)**: Tracks the allocation status of the 64 available inodes.
- **Data Bitmap (Block 18)**: Tracks the allocation status of the 64 available data blocks.
- **Inode Table (Blocks 19-20)**: Stores inode structures, each 128 bytes in size.
- **Data Region (Blocks 21-84)**: Stores the actual file content and directory entries.

### Data Structures

- **Inodes**: Contain type information (file vs. directory), link counts, size, and 8 direct pointers to data blocks.
- **Directory Entries**: Each entry is 32 bytes, consisting of a 4-byte inode number and a 28-byte null-terminated name.

## Features

### Journaling (Write-Ahead Logging)

To prevent filesystem corruption, VSFS implements a journaling system.

- **Transaction Records**: Metadata changes are written as data records (`REC_DATA`) followed by a commit record (`REC_COMMIT`).
- **Atomicity**: Updates to the inode bitmap, inode table, and directory blocks are first staged in the journal.
- **Recovery**: The `install` command replays committed transactions from the journal to the permanent data region.

### Filesystem Validator

The project includes a robust validation tool to ensure disk consistency.

- **Superblock Check**: Verifies magic numbers and internal offsets.
- **Bitmap Verification**: Cross-references the inode and data bitmaps against actual usage in the inode table and directory structures.
- **Directory Integrity**: Ensures that all directories contain valid `.` and `..` entries and that link counts are accurate.
- **Pointer Safety**: Detects out-of-range block pointers and data block double-allocation.

## Build and Usage

### Prerequisites
- GCC or Clang on a POSIX-like environment
- Make (optional) for scripted builds

### Compilation

Compile the utilities using any standard C compiler:
```bash
gcc -o mkfs mkfs.c
gcc -o journal journal.c
gcc -o validator validator.c
```

### Formatting the Disk

Initialize the `vsfs.img` file with the required structures:
```bash
./mkfs
```

This creates the superblock, reserves the root inode (Inode 0), and sets up the root directory.

### File Operations

**Create a File**

Stage a file creation in the journal:
```bash
./journal create <filename>
```

This logs the necessary metadata updates to the journal region.

**Commit Changes**

Permanently apply the journaled updates to the disk:
```bash
./journal install
```

This applies all completed transactions and clears the journal.

### Checking Integrity

Run the validator to identify any inconsistencies:
```bash
./validator
```

If the filesystem is healthy, it reports: "Filesystem 'vsfs.img' is consistent."

## Technical Specifications

| Parameter | Value |
|---|---|
| Block Size | 4096 Bytes |
| Inode Size | 128 Bytes |
| Max Direct Pointers | 8 |
| Max Files | 64 |
| Max Data Blocks | 64 |
| Superblock Magic | `0x56534653` |
| Journal Magic | `0x4A524E4C` |

## Contributors

- [Ahnaf Iqbal](https://github.com/ahnaf-iqbal) - Collaborator
- [Md. Mahmudur Rahman](https://github.com/MdLabib007) - Collaborator


## License

This project is provided for educational use in systems programming and operating systems courses.

---

## Contact

**Abdullah Al Fahad**

[![LinkedIn](https://img.shields.io/badge/LinkedIn-Connect-blue?style=flat&logo=linkedin)](https://www.linkedin.com/in/abdullahalfahadayon/)
[![Email](https://img.shields.io/badge/Email-Contact-red?style=flat&logo=gmail)](mailto:abdullah.al.fahad2@g.bracu.ac.bd)
[![GitHub](https://img.shields.io/badge/GitHub-Follow-black?style=flat&logo=github)](https://github.com/fah-ayon)

---
