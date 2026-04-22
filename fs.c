//implementing the FAT-based file system

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "disk.h"    
#include "fs.h"     

//global FS state—>holds the in-memory copy of everything: superblock, FAT, directory, open file descriptors
static fs_state_t fs;

//flush_metadata — write superblock, FAT, and root directory from RAM to disk
static int flush_metadata(void)
{
    //scratch buffer — block_write requires a full 4096-byte block every time
    //we memset to 0 first so unused bytes on disk are always clean zeros
    char buf[BLOCK_SIZE];

    //we write superblock to block 0
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &fs.sb, sizeof(superblock_t)); // superblock is 32 bytes; rest stays zero
    if (block_write(SUPERBLOCK_BLOCK, buf) < 0) {
        fprintf(stderr, "fs: failed to write superblock\n");
        return -1;
    }

    //write FAT to blocks 1 and 2
    char *fat_bytes = (char *)fs.fat;
    for (int i = 0; i < FAT_BLOCKS; i++) {
        memset(buf, 0, BLOCK_SIZE);
        memcpy(buf, fat_bytes + i * BLOCK_SIZE, BLOCK_SIZE);
        if (block_write(FAT_BLOCK_START + i, buf) < 0) { // writes to block 1 then block 2
            fprintf(stderr, "fs: failed to write FAT block %d\n", i);
            return -1;
        }
    }

    //write root directory to block 3
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, fs.dir, sizeof(fs.dir));
    if (block_write(DIR_BLOCK, buf) < 0) {
        fprintf(stderr, "fs: failed to write directory block\n");
        return -1;
    }

    return 0; 
}

//load_metadata —>read superblock, FAT, and root directory from disk into RAM
static int load_metadata(void)
{
    //we scratch the buffer for reading one block at a time
    char buf[BLOCK_SIZE];

    //we read superblock from block 0
    if (block_read(SUPERBLOCK_BLOCK, buf) < 0) {
        fprintf(stderr, "fs: failed to read superblock\n");
        return -1;
    }
    memcpy(&fs.sb, buf, sizeof(superblock_t));

    //we check magic number.. if it doesn't match then the disk was never formatted by make_fs
    if (fs.sb.magic != FS_MAGIC) {
        fprintf(stderr, "fs: invalid magic number -- disk is not a valid FS\n");
        return -1;
    }

    //we read FAT from blocks 1 and 2
    char *fat_bytes = (char *)fs.fat;
    for (int i = 0; i < FAT_BLOCKS; i++) {
        if (block_read(FAT_BLOCK_START + i, buf) < 0) {
            fprintf(stderr, "fs: failed to read FAT block %d\n", i);
            return -1;
        }
        memcpy(fat_bytes + i * BLOCK_SIZE, buf, BLOCK_SIZE);
    }

    //we read root directory from block 3
    if (block_read(DIR_BLOCK, buf) < 0) {
        fprintf(stderr, "fs: failed to read directory block\n");
        return -1;
    }
    memcpy(fs.dir, buf, sizeof(fs.dir));

    return 0; 
}

//make_fs —>create and format a brand new empty file system on disk_name 
int make_fs(char *disk_name)
{
    //we create the virtual disk file 
    if (make_disk(disk_name) < 0) {
        fprintf(stderr, "make_fs: make_disk failed for '%s'\n", disk_name);
        return -1;
    }

    //we open the disk so block_write calls can work
    if (open_disk(disk_name) < 0) {
        fprintf(stderr, "make_fs: open_disk failed for '%s'\n", disk_name);
        return -1;
    }

    //we zero out the entire global fs struct so no garbage leaks into metadata
    memset(&fs, 0, sizeof(fs_state_t));

    //we fill in superblock and describe the layout of this disk
    fs.sb.magic = FS_MAGIC;          
    fs.sb.num_blocks = DISK_BLOCKS;      
    fs.sb.num_data_blocks = NUM_DATA_BLOCKS;   
    fs.sb.fat_start = FAT_BLOCK_START;    
    fs.sb.fat_blocks = FAT_BLOCKS;       
    fs.sb.dir_block = DIR_BLOCK;        
    fs.sb.data_start = DATA_BLOCK_START;  
    fs.sb.max_files = MAX_FILES;         

    //we mark every FAT entry as free  
    for (int i = 0; i < NUM_DATA_BLOCKS; i++) {
        fs.fat[i] = FAT_FREE;
    }

    //we mark every directory slot as unused
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&fs.dir[i], 0, sizeof(dir_entry_t));
        fs.dir[i].used = 0;
    }

    //we write superblock, FAT, and directory to the disk
    //if fails -> close the disk before returning so we don't leak the open file descriptor
    if (flush_metadata() < 0) {
        fprintf(stderr, "make_fs: failed to write initial metadata\n");
        close_disk();
        return -1;
    }

    //we close the disk.. caller must use mount_fs separately before any file operations
    if (close_disk() < 0) {
        fprintf(stderr, "make_fs: close_disk failed\n");
        return -1;
    }

    return 0;
}

//mount_fs — open disk_name and load its FS metadata into RAM
//after this call the FS is "ready for use" and fs_* functions will work
int mount_fs(char *disk_name)
{
    //only one FS can be mounted at a time
    if (fs.mounted) {
        fprintf(stderr, "mount_fs: a file system is already mounted\n");
        return -1;
    }

    //we open the virtual disk file so block_read calls can work
    if (open_disk(disk_name) < 0) {
        fprintf(stderr, "mount_fs: open_disk failed for '%s'\n", disk_name);
        return -1;
    }

    memset(&fs, 0, sizeof(fs_state_t));

    //we read superblock, FAT, and directory from disk 
    if (load_metadata() < 0) {
        close_disk();
        return -1;
    }

    //we mark all file descriptor slots as closed  
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        fs.fd_table[i].used = 0;
    }

    //FS is ready.. fs_* functions check this flag before doing anything
    fs.mounted = 1;
    return 0;
}


//umount_fs — flushes all in-memory metadata back to disk, then closes
//after this call. No file operations are valid until mount_fs is called again
int umount_fs(char *disk_name)
{
    //we can't unmount what isn't mounted
    if (!fs.mounted) {
        fprintf(stderr, "umount_fs: no file system is currently mounted\n");
        return -1;
    }

    //force-close all open file descriptors
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) {
        if (fs.fd_table[i].used) {
            fs.fd_table[i].used = 0;
        }
    }

    //write superblock, FAT, and directory back to disk
    //if we dont have this step, any changes since mount_fs would be lost when the process exits
    if (flush_metadata() < 0) {
        fprintf(stderr, "umount_fs: failed to flush metadata\n");
        return -1;
    }

    //close the virtual disk file
    if (close_disk() < 0) {
        fprintf(stderr, "umount_fs: close_disk failed for '%s'\n", disk_name);
        return -1;
    }

    //we cler the flag so mount_fs can be called again
    fs.mounted = 0;
    return 0;
}

//fs_open —>open file fname for reading and writing
int fs_open(char *fname)
{
    //reject if no FS is mounted
    if (!fs.mounted) return -1;
 
    //find the file in the directory
    int dir_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) 
    {
        if (fs.dir[i].used && strcmp(fs.dir[i].name, fname) == 0) 
        {
            dir_idx = i;
            break;
        }
    }
 
    //fail if the file is not found
    if (dir_idx == -1) return -1;
 
    //find a free file descriptor slot
    int fd = -1;
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) 
    {
        if (!fs.fd_table[i].used) 
        {
            fd = i;
            break;
        }
    }
 
    //fail if all of the 32 descriptors are already open
    if (fd == -1) return -1;
 
    //initialize the file descriptor
    fs.fd_table[fd].used      = 1;
    fs.fd_table[fd].dir_index = dir_idx;

    //the file pointer starts at beginning
    fs.fd_table[fd].offset    = 0; 
 
    return fd;
}
 

//fs_close, close teh file descriptor fildes
int fs_close(int fildes)
{
    //reject if no FS is mounted
    if (!fs.mounted) return -1;
 
    //reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    //mark the slot as free
    fs.fd_table[fildes].used = 0;
 
    return 0;
}

//fs_create ->create a new empty file named fname in the root directory
int fs_create(char *fname)
{
    //reject if there is no FS mounted
    if (!fs.mounted) return -1;
 
    //reject if the name is too long (the max 15 characters)
    if (strlen(fname) > MAX_FILENAME) return -1;
 
    int free_slot = -1;
 
    for (int i = 0; i < MAX_FILES; i++) 
    {
        if (fs.dir[i].used) 
        {
            //fail if a file with this name already exists
            if (strcmp(fs.dir[i].name, fname) == 0) return -1;
        } else 
        {
            //remember the first free slot that we find
            if (free_slot == -1) free_slot = i;
        }
    }
 
    //fail if the directory is full (no free slot found)
    if (free_slot == -1) return -1;
 
    //initialize the new directory entry
    memset(&fs.dir[free_slot], 0, sizeof(dir_entry_t));
    strncpy(fs.dir[free_slot].name, fname, MAX_FILENAME);

    //we guarantee the null terminator
    fs.dir[free_slot].name[MAX_FILENAME] = '\0'; 
    fs.dir[free_slot].used = 1;

    //the file starts empty
    fs.dir[free_slot].size = 0;        

    //no data blocks yet
    fs.dir[free_slot].first_block = FAT_FREE;  
 
    return 0;
}

//fs_delete-> delete file fname and free all its data blocks
int fs_delete(char *fname)
{
    //reject if there is no FS mounted
    if (!fs.mounted) return -1;
 
    //find the file in the directory
    int dir_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) 
    {
        if (fs.dir[i].used && strcmp(fs.dir[i].name, fname) == 0) 
        {
            dir_idx = i;
            break;
        }
    }
 
    //fail if the file is not found
    if (dir_idx == -1) return -1;
 
    //fail if the file is currently open (any fd points to it)
    for (int i = 0; i < MAX_FILE_DESCRIPTORS; i++) 
    {
        if (fs.fd_table[i].used && fs.fd_table[i].dir_index == dir_idx) return -1;
    }
 
    //walk the FAT chain and free every data block the file owns
    uint16_t block = fs.dir[dir_idx].first_block;
    while (block != FAT_FREE && block != FAT_EOF) 
    {
        //save next before we overwrite
        uint16_t next = fs.fat[block]; 

        //we mark this block as free
        fs.fat[block] = FAT_FREE;      

        //we move to next block in chain
        block = next;                  
    }
 
    //we clear the directory entry so the slot is available again
    memset(&fs.dir[dir_idx], 0, sizeof(dir_entry_t));
 
    return 0;
}

int fs_read(int fildes, void *buf, size_t nbyte)
{
    //we reject if there is  no FS mounted
    if (!fs.mounted) return -1;
 
    //we reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    int dir_idx     = fs.fd_table[fildes].dir_index;
    off_t offset    = fs.fd_table[fildes].offset;
    uint32_t size   = fs.dir[dir_idx].size;
 
    //if the file pointer is already at or past end of file, there is nothing to read
    if (offset >= (off_t)size) return 0;
 
    //we clamp nbyte so we don't read past end of the file
    if (offset + (off_t)nbyte > (off_t)size)
        nbyte = (size_t)(size - offset);
 
    //we walk the FAT chain to the block that contans our starting offset.. offset/BLOCK_SIZE tells us how many blocks into the chain we need to skip
    uint16_t block     = fs.dir[dir_idx].first_block;
    int blocks_to_skip = (int)(offset / BLOCK_SIZE);
    for (int i = 0; i < blocks_to_skip; i++) 
    {
        block = fs.fat[block];  
    }
 
    char disk_buf[BLOCK_SIZE];
    size_t bytes_read = 0;
    size_t bytes_left = nbyte;
    
    //this is where within the current block our offset lands
    int block_offset = (int)(offset % BLOCK_SIZE);
 
    while (bytes_left > 0 && block != FAT_EOF && block != FAT_FREE) {
        //we read the full block from disk into our scratch buffer
        if (block_read(DATA_BLOCK_START + block, disk_buf) < 0) return -1;
 
        //how many bytes we can take from this block
        size_t available = BLOCK_SIZE - block_offset;
        size_t to_copy   = (bytes_left < available) ? bytes_left : available;
 
        //we copy from the block into the caller's buffer
        memcpy((char *)buf + bytes_read, disk_buf + block_offset, to_copy);
 
        bytes_read += to_copy;
        bytes_left -= to_copy;
    
        //after the first block, we always start at byte zero
        block_offset = 0;         

        //we move to the next block in the chain
        block = fs.fat[block];  
    }
 
    //we advance the file pointer
    fs.fd_table[fildes].offset += (off_t)bytes_read;
 
    return (int)bytes_read;
}
 

//write nbyte bytes from buf into fildes starting at current offset.. extends the file automatically if writing past end.
//allocates new data blocks from the FAT as needed + stops early if the disk runs out of free blocks + advances the file pointer by the number of bytes actually written
int fs_write(int fildes, void *buf, size_t nbyte)
{
    //we reject if no FS is mounted
    if (!fs.mounted) return -1;
 
    //we reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    int dir_idx = fs.fd_table[fildes].dir_index;
    off_t offset = fs.fd_table[fildes].offset;
 
    //we walk the FAT chain to the block containing our starting offset, allocating new blocks along the way if needed
    uint16_t block      = fs.dir[dir_idx].first_block;
    uint16_t prev_block = FAT_FREE;
    int blocks_to_skip  = (int)(offset / BLOCK_SIZE);
 
    for (int i = 0; i < blocks_to_skip; i++) 
    {
        if (block == FAT_FREE || block == FAT_EOF) 
        {
            //we need to allocate a new block to extend the chain
            uint16_t new_block = FAT_FREE;
            for (int j = 0; j < NUM_DATA_BLOCKS; j++) 
            {
                if (fs.fat[j] == FAT_FREE) { new_block = j; break; }
            }
            //if the disk is full, we return how many bytes we managed to write so far
            if (new_block == FAT_FREE) 
            {
                return (int)(offset - fs.fd_table[fildes].offset);
            }
            fs.fat[new_block] = FAT_EOF; //the new block is the end of the chain
 
            if (prev_block == FAT_FREE) 
            {
                //the file had no blocks at all, this is the first one
                fs.dir[dir_idx].first_block = new_block;
            } else 
            {
                //we link the previous block to the new one
                fs.fat[prev_block] = new_block;  
            }
            block = new_block;
        }
        prev_block = block;
        block = fs.fat[block];
    }
 
    char disk_buf[BLOCK_SIZE];
    size_t bytes_written = 0;
    size_t bytes_left = nbyte;
    int block_offset = (int)(offset % BLOCK_SIZE);
 
    while (bytes_left > 0) 
    {
        //if we have no current block, allocate one
        if (block == FAT_FREE || block == FAT_EOF) 
        {
            uint16_t new_block = FAT_FREE;
            for (int j = 0; j < NUM_DATA_BLOCKS; j++) 
            {
                if (fs.fat[j] == FAT_FREE) { new_block = j; break; }
            }
            //the disk is full, stop here
            if (new_block == FAT_FREE) break;
 
            fs.fat[new_block] = FAT_EOF;
 
            if (prev_block == FAT_FREE) 
            {
                fs.dir[dir_idx].first_block = new_block;
            } else 
            {
                fs.fat[prev_block] = new_block;
            }
            block = new_block;
        }
 
        //we read the block first so we don't overwrite bytes we aren't touching
        if (block_read(DATA_BLOCK_START + block, disk_buf) < 0) break;
 
        //this is how many bytes we can fit into this block from block_offset
        size_t available = BLOCK_SIZE - block_offset;
        size_t to_copy = (bytes_left < available) ? bytes_left : available;
 
        //we copy from caller's buffer into the block
        memcpy(disk_buf + block_offset, (char *)buf + bytes_written, to_copy);
 
        //we write the modified block back to disk
        if (block_write(DATA_BLOCK_START + block, disk_buf) < 0) break;
 
        bytes_written += to_copy;
        bytes_left -= to_copy;

        //after the first block, we always start at byte 0
        block_offset = 0;       
        prev_block = block;

        //we move to the next block
        block = fs.fat[block];  
    }
 
    //we update file size if we wrote past the old end
    off_t new_end = offset + (off_t)bytes_written;
    if (new_end > (off_t)fs.dir[dir_idx].size)
        fs.dir[dir_idx].size = (uint32_t)new_end;
 
    //we advance the file pointer
    fs.fd_table[fildes].offset += (off_t)bytes_written;
 
    return (int)bytes_written;
}
 

//return the size in bytes of the file pointed to by fildes 
int fs_get_filesize(int fildes)
{
    //we reject if there is no FS mounted
    if (!fs.mounted) return -1;
 
    //we reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    //we look up the directory entry via the fd and return its size
    int dir_idx = fs.fd_table[fildes].dir_index;
    return (int)fs.dir[dir_idx].size;
}

//move the file pointer of fildes to byte position offset.. the offset must satisfy: 0 <= offset <= file size
//it returns 0 -> success, -1 -> invalid fd or offset out of range
int fs_lseek(int fildes, off_t offset)
{
    //we reject if no FS is mounted
    if (!fs.mounted) return -1;
 
    //we reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    //we reject if offset is negative
    if (offset < 0) return -1;
 
    int dir_idx = fs.fd_table[fildes].dir_index;
 
    //we reject if offset is past the end of the file
    if (offset > (off_t)fs.dir[dir_idx].size) return -1;
 
    //we set the file pointer to the new position
    fs.fd_table[fildes].offset = offset;
 
    return 0;
}

//shrink file fildes to length bytes, frees any data blocks that fall beyond the new length... 
//if the file pointer is past the new length, move it to the new end. It cannot extend a file, the length must be <= current file size
int fs_truncate(int fildes, off_t length)
{
    //we reject if no FS is mounted
    if (!fs.mounted) return -1;
 
    //we reject if fildes is out of range or not open
    if (fildes < 0 || fildes >= MAX_FILE_DESCRIPTORS) return -1;
    if (!fs.fd_table[fildes].used) return -1;
 
    //we reject if length is negative
    if (length < 0) return -1;
 
    int dir_idx = fs.fd_table[fildes].dir_index;
 
    //we reject if length is larger than current file size (can't extend with truncate)
    if (length > (off_t)fs.dir[dir_idx].size) return -1;
 
    //we walk the FAT chain and free all blocks beyond the new length
    //blocks_to_keep -> how many blocks the truncated file still needs
    int blocks_to_keep = (int)((length + BLOCK_SIZE - 1) / BLOCK_SIZE); //length=0 -> keep 0 blocks, length=1 -> keep 1 block, length=4096 -> keep 1 block

    uint16_t block = fs.dir[dir_idx].first_block;
    uint16_t prev  = FAT_FREE;
 
    //we skip past the blocks we are keeping
    for (int i = 0; i < blocks_to_keep; i++) {
        prev  = block;
        block = fs.fat[block];
    }
 
    //if we kept zero blocks, the file now has no data blocks at all
    if (blocks_to_keep == 0) 
    {
        fs.dir[dir_idx].first_block = FAT_FREE;
    } else if (prev != FAT_FREE) 
    {
        //we terminate the chain at the last block we kept
        fs.fat[prev] = FAT_EOF;
    }
 
    //we free every block from here to the end of the old chain
    while (block != FAT_FREE && block != FAT_EOF) {
        uint16_t next  = fs.fat[block];
        fs.fat[block]  = FAT_FREE;
        block          = next;
    }
 
    //we update the file size to the new length
    fs.dir[dir_idx].size = (uint32_t)length;
 
    //if the file pointer is now past the new end, move it to the new end
    if (fs.fd_table[fildes].offset > length) 
    {
        fs.fd_table[fildes].offset = length;
    }
    return 0;
}