Project 4 - Simple File System Design and Implementation
Volume Layout
The virtual disk has 8,192 blocks (4,096 bytes) that are divided into four regions

Block 0: The superblock, magic number and layout parameters
Block 1-2: FAT, 4,096 entires * 2 bytes = 8,192 bytes
Block 3: The root directory. 64 entries * 32 bytes = 2,048 bytes. 
Block 4: The data region, 4,096 block for all the file content

|  Superblock |   FAT    |  Root Directory  |        Data Region          |
|  (block 0)     | (1 - 2)   |    (block 3)         |       (blocks 4-4099)      |
     1 block       2 blocks      1 block                  4,096 blocks

The superblock is always block zero. It’s the first thing that mount_fs reads and it tells the system where all the other regions live.

The Physical Directory (space allocation)
The method of space allocation that the file system uses
The file system uses the (FIle Allocation Table) strategy. THe file data is stored across 1 or more 4,096 byte data blocks. The FAT is just an array of 4,096 uint16_t values, there is one per data block, that forms a LL describing how each file’s blocks are chained together. 

The 3 values that each FAT entry can hold:
Value					Meaning
0xFFFE				Block is free (FAT_FREE)
OxFFFF				Last block in a file’s chain (FAT_EOF
Any other value j			Next block of the file is block j 

Managing Free space
The free blocks are tracked through scanning the FAT for entries equal to FAT_FREE (0xFFFE). There is no free -block bitmap that exists separately, all the FAT works as both the block chain and the free-space tracker.

make_fs: every FAT entry is set to FAT_FREE
Fs_write : the FAT is scanned linearly for the first FAT_FREE entry to allocate a new block.
fs_delete/fs_truncate: each block in the chain is reset to FAT_FREE

A visual example of FAT CHAIN

This file occupies data blocks 0,1,and 2
Index:  [0]     	     [1]          [2]           [3]          [4]
FAT:  [0x0001][0x0002][0xFFFF][0xFFFE][0xFFFE]
         next=1      next=2      EOF      free         free
The logical directory structure
The root directory: exists in block 3 as an array of 64 dir_entry_t structs (each being exactly 32 bytes)

The directory record structure

typedef struct { 
char name[16]; //null-terminated the file name
uint8_t used; //1 = slot is occupied, 0 = slot is free 
uint8_t _pad[2]; //alignment padding 
uint32_t size; //for the file size in bytes 
uint16_t first_block; //FAT index of the file's first data block 
uint8_t _pad2[7]; //reserved, zero-padded
} dir_entry_t;

Field Layout (32 bytes in total)
Offset  Size   Field
------     ----   -----
0         16     name (null-terminated, max 15 chars)
16        1     used flag
17         2     padding
19        4     size (bytes)
23        2     first_block (FAT index)
25        7     reserved padding

File Naming
The file names are stored as c strings that are null-terminated
The max length is 15 characters (+1 null terminator so = 16 bytes total)
Fs_create rejects names that are longer than 15 characters or names that exist already.
When a file is deleted, used is set to zero, and the slot becomes available for reuse

4. Physical directory connection
The logical directory (dir_entry_t) connects to the physical allocation (FAT chain) through the first_block field.

Logical Directory (block 3)          FAT (blocks 1-2)          Data Region (blocks 4+)
—--------------------------------------------------------------------------------------------------------------
| name: "fileA.txt"    |          | fat[0] = 0x0001  |        | block 4 (data)   |
| used: 1                  |          | fat[1] = 0xFFFF  |        +------------------+
| size: 8192             |          | fat[2] = 0xFFFE  |        | block 5 (data)   |
| first_block: 0         |           | fat[3] = 0xFFFE  |        +------------------+
        |                                       |   +---> data block 0 (disk block 4)
fat[0] = 1
data block 1 (disk block 5)
fat[1] = FAT_EOF

How the connection works:
1. Fs_create sets first_block = FAT_FREE (the file starts empty, no blocks)
2. Fs_write allocates the first free FAT entry, fs_write stores it in dir_entry.first_block, marks it FAT_EOF
3. Each following block is chained by updating the previous block’s FAT entry to point to the new one
4. Fs_read and fs_write navigate the chain by starting at dir_entry.first_block and following FAT entries
5. Fs_delete walks the chain from first_block, setting every FAT entry to FAT_FREE, then zeroes the directory entry

Space allocation and freeing

Allocating a block (in fs_write)
Scan FAT linearly for first entry == FAT_FREE
Set fat[new_block] = FAT_EOF
If this is the first block then: dir_entry.first_block = new_block
Else:  fat[prev_block] = new_block
We write data into disk block (DATA_BLOCK_START + new_block)

Freeing Blocks (in fs_delete)
Start at dir_entry.first_block
Save next = fat[block]
Set fat[block] = FAT_FREE
Move to the next block
Repeat this until eof (FAT_EOF)
We zero the directory entry (set used = 0)

Freeing blocks in fs_truncate
We calculate the blocks_to_keep = ceil(length / BLOCK_SIZE)
Walk the chain, skipping all blocks_to_keep blocks
We terminate the chain at the last kept block (fat[prev] = FAT_EOF)
We free all the remaining blocks in the chain using fat[block] = FAT_FREE
Then update dir_entry.size = length
If the the file pointer > new length, we move it to length


6. The File System Functions

Make_fs
Creates + formats a new virtual disk. 
Calls make_disk to create the file 
Open_disk initializes the superblock struct with layout parameters and the magic number 0x46415401
Sets every FAT entry to FAT_FREE (0xFFFE). 
Zeroes all 64 directory entries
Calls flush_metadata to write the superblock (block 0), FAT (blocks 1 and 2), and directory (block 3) to disk
Closes the disk. 

Mount_fs
Opens the disk with open_disk
Zeroes the global fs_state_t struct
Calls load_metadata which reads the superblock and validates the magic number, reads FAT from blocks 1-2 into RAM, reads the directory from block 3 into RAM
Sets fs.mounted = 1

Umount_fs
Force-closes all open file descriptors by setting fd_table[i].used = 0
Calls flush_metadata to write all in-memory changes back to the disk
Closes the disk
Fs.mounted = 0 

Fs_create
Scans the directory for a duplicate name (returns -1 if found)
Finds the first slot where used == 0 
Copies the name with strncpy, sets used = 1, size = 0, first_block = FAT_FREE
The file has no data blocks until fs_write is called

Fs_delete
Finds the file by name in the directory
Checks that no open file descriptor points to it (returns -1 if any do)
Walks the FAT chain starting at first_block, setting each entry to FAT_FREE and saving the next pointer before overwriting. 
Then it zeroes the directory entry with memset.

Fs_open
Scans the directory for the file name
Finds the first fd_table slot where used == 0
Sets used = 1, dir_index to the directory slot, offset = 0
Returns the slot index as the file descriptor

Fs_close
Validates the file descriptor range and checks used == 1
Sets fd_table[fildes].used = 0
The directory entry and FAT are not modified, only the in-memory descriptor is cleared

Fs_read
Clamps nbyte to avoid reading past EOF(size - offset)
Calculates blocks_to_skip = offset / BLOCK_SIZE and walks that many FAT chain links to reach the starting block
Computes block_offset = offset % BLOCK_SIZE for the position within that block. 
Loops: calls block_read into a local char disk_buf[BLOCK_SIZE], copies min(bytes_left, BLOCK_SIZE - block_offset) bytes into the caller’s buffer then follow fat[block] to the next block
Advances fd_table[fildes].offset by bytes read

Fs_write
Navigates to the block containing the current offset, allocating new FAT entries as needed by scanning for FAT_FREE. 
Performs block_read before block_write to preserve untouched bytes in a partial block
Chains new blocks by updating fat[prev_block] = new_block
Updates dir_entry.size if writing past the old end
Moves fd_table[fildes].offset by bytes written

Fs_get_filesize
Validates the file descriptor
returns dir[fd_table[fildes].dir_index].size.

Fs_lseek
Validates the file descriptor 
Checks 0 <= offset <= dir_entry.size (returns -1 if out of range).
Sets fd_table[fildes].offset = offset.

Fs_truncate
Validates the file descriptor
Checks length <= dir_entry.size (cannot extend)
Calculates blocks_to_keep = ceil(length / BLOCK_SIZE)
Walks the FAT chain skipping blocks_to_keep blocks, terminates the chain with fat[prev] = FAT_EOF, frees all remaining blocks with fat[block] = FAT_FREE
Updates dir_entry.size = length
If fd_table[fildes].offset > length, sets offset to length

7. Hexdumps

Block 0: Superblock after make_fs
00000000: 0154 4146 0020 0000 0010 0000 0100 0000   ....
00000010: 0200 0000 0300 0000 0400 0000 4000 0000  ....
Interpretation (little-endian):
Bytes 0-3: 0x46415401 — magic number ("FAT\x01")
Bytes 4-7: 0x00002000 = 8192 total blocks
Bytes 8-11: 0x00001000 = 4096 data blocks
Bytes 12-15: 0x00000001 = 1 FAT start block
Bytes 16-19: 0x00000002 = 2 FAT block count
Bytes 20-23: 0x00000003 = 3 directory block
Bytes 24-27: 0x00000004 = 4 data start block
Bytes 28-31: 0x00000040 = 64 max files

Block 3: The root directory after writing fileA
00003000: 6669 6c65 412e 7478 7400 0000 0000 0000  fileA.txt.......
00003010: 0100 0000 2000 0000 0000 0000 0000 0000  .... ...........
00003020: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00003030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
Interpretation:
Bytes 0-15: fileA.txt - the file name (it’s null-padded to 16 bytes)
Byte 16: 0x01 - used = 1 (this means the slot is occupied)
Bytes 19-22: 0x00002000 = 8192 (the file size in bytes)
Bytes 23-24: 0x0000 = 0 (the first_block (FAT index 0))
FAT (block 1) after writing fileA
00001000: 0100 ffff feff feff feff feff feff feff  ................
00001010: feff feff feff feff feff feff feff feff  ................

The Interpretation (little-endian, 2 bytes per entry):
fat[0]= 0x0001 -  block 0's next block is block 1
fat[1] = 0xFFFF - block 1 is the last block (FAT_EOF)
fat[2+] = 0xFFFE - all remaining blocks are free (FAT_FREE)

This shows fileA taking up exactly 2 data blocks (8192 bytes = 2 × 4096)

Block 3: The root directory after deleting both files
00003000: 6669 6c65 412e 7478 7400 0000 0000 0000  fileA.txt.......
00003010: 0100 0000 2000 0000 0000 0000 0000 0000  .... ...........
00003020: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00003030: 0000 0000 0000 0000 0000 0000 0000 0000  ...............
00003040: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00003050: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00003060: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00003070: 0000 0000 0000 0000 0000 0000 0000 0000  ................

The name bytes remain in memory but the used flag (byte 16 of each entry) is 0x00, marking the slot as free. The file system treats any entry with used == 0 as available for reuse. 

Block 1: FAT after deleting both files
00001000: 0100 ffff feff feff feff feff feff feff  ................
00001010: feff feff feff feff feff feff feff feff  ................
00001020: feff feff feff feff feff feff feff feff  ................
00001030: feff feff feff feff feff feff feff feff  ................

All of the data blocks that were allocated to fileA and fileB have been returned to FAT_FREE (0xFFFE), confirming that deletion correctly frees all associated data blocks. 











