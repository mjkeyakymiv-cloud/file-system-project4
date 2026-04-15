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
    uint8_t buf[BLOCK_SIZE];

    //we write superblock to block 0
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &fs.sb, sizeof(superblock_t)); // superblock is 32 bytes; rest stays zero
    if (block_write(SUPERBLOCK_BLOCK, buf) < 0) {
        fprintf(stderr, "fs: failed to write superblock\n");
        return -1;
    }

    //write FAT to blocks 1 and 2
    uint8_t *fat_bytes = (uint8_t *)fs.fat;
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
    uint8_t buf[BLOCK_SIZE];

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
    uint8_t *fat_bytes = (uint8_t *)fs.fat;
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

//week 2 stubs 
//each one returns -1 so accidental calls fail loudly  
int fs_open(char *fname)
{
    (void)fname;
    return -1;  
}

int fs_close(int fildes)
{
    (void)fildes;
    return -1; 
}

int fs_create(char *fname)
{
    (void)fname;
    return -1; 
}

int fs_delete(char *fname)
{
    (void)fname;
    return -1;  
}

int fs_read(int fildes, void *buf, size_t nbyte)
{
    (void)fildes; (void)buf; (void)nbyte;
    return -1;  
}

int fs_write(int fildes, void *buf, size_t nbyte)
{
    (void)fildes; (void)buf; (void)nbyte;
    return -1; 
}

int fs_get_filesize(int fildes)
{
    (void)fildes;
    return -1;  
}

int fs_lseek(int fildes, off_t offset)
{
    (void)fildes; (void)offset;
    return -1;  
}

int fs_truncate(int fildes, off_t length)
{
    (void)fildes; (void)length;
    return -1; 
}