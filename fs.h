
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <sys/types.h>
#include "disk.h"

//4096 bytes per block
//#define BLOCK_SIZE 4096  

//the total number of blocks on the virtual disk
#define DISK_BLOCKS 8192

//the block numbers for each metadata region
#define SUPERBLOCK_BLOCK 0   
#define FAT_BLOCK_START 1   
#define FAT_BLOCKS 2   
#define DIR_BLOCK 3   
#define DATA_BLOCK_START 4   

//the number of data blocks available for storing file contents.
#define NUM_DATA_BLOCKS 4096

//the max numb of files that can exist at once in the root directory
#define MAX_FILES 64

//the max length of a filename
#define MAX_FILENAME 15

//the max number of file descriptors open at the same time across all files, if all 32 are in use, fs_open fails. 
#define MAX_FILE_DESCRIPTORS 32

//max size of a single file
#define MAX_FILE_SIZE (NUM_DATA_BLOCKS * BLOCK_SIZE)

//free data block
#define FAT_FREE 0xFFFEu 

//last data block in the file chain 
#define FAT_EOF 0xFFFFu 

//on disk data structures 
typedef struct {
    //identifies this as our FS - must equal FS_MAGIC
    uint32_t magic;       
    //the total blocks on disk (8 192)
    uint32_t num_blocks;      
    
    //how many blocks are in the data region (4096)
    uint32_t num_data_blocks;  
    
    //the block number where the FAT begins (1)
    uint32_t fat_start;      
    
    //how many blocks the FAT occupuies (2)
    uint32_t fat_blocks;     
    
    //the block number of the root directory (3)
    uint32_t dir_block;     
    
    //the block number of the first data block (4)
    uint32_t data_start;     
    
    //the max files the directory can hold (64)
    uint32_t max_files;       
    
    //the superblock struct is 32 bytes
    
} __attribute__((packed)) superblock_t;

//the magic constant that spell 'FAT' followed by version byte 0x0
#define FS_MAGIC 0x46415401u   //this is hex for 'F','A','T',0x01 

//one FAT cell is 2 bytes
typedef uint16_t fat_entry_t;               

//the full FAT is 8192 bytes
typedef fat_entry_t fat_t[NUM_DATA_BLOCKS];  
 
//the root directory, an array of 64 dir_entry_t structs stored in block 3
typedef struct {
    
    //the file name is up to 15 printable characters plus the required null terminator
    char name[MAX_FILENAME + 1];  //16 bytes 

    //when fs_delete is called we simply set used = 0 rather than shifting the rest of the arrray
    uint8_t used; //1 byte 

    //two bytes of padding so that the uint32_t size field below starts on a 4-byte boundary.  Without this the struct would still work
    uint8_t _pad[2]; //2 bytes 

    //the size of the file in bytes
    uint32_t size; //4 bytes

    //index into the FAT + data region of the file's first data block
    uint16_t first_block; //2 bytes

    //extra padding bytes that bring the total to exactly 32 bytes
    uint8_t  _pad2[7]; //7 bytes 

} __attribute__((packed)) dir_entry_t;

//compile-time size check.  
_Static_assert(sizeof(dir_entry_t) == 32,
               "dir_entry_t must be 32 bytes so 64 entries fit in one 4096-byte block");

//in memory only data structures
//the types below never touch the disk. They exist only while the file system is mounted and a process is runnin
typedef struct {
    
    //1 -> its open; 0 -> its available for reuse 
    int used;

    //which slot in fs.dir[], points to (0–63)
    int dir_index;

    //the current file pointer.. read from here and advance it afterward 
    off_t offset;
    
} file_descriptor_t;
 
 //everything the running process needs to know about the currently used file system
typedef struct {
    
    //1 -> currently mounted; 0 -> not mounted
    int mounted;

    //in memory copy of the superblock  
    superblock_t sb;

    //in memory copy of the full FAT (read from blocks 1–2 by mount_fs
    fat_t fat;

    //in-memory copy of all 64 directory entries (read from block 3). 
    dir_entry_t dir[MAX_FILES];

    //a table of open file descriptors
    file_descriptor_t fd_table[MAX_FILE_DESCRIPTORS];
} fs_state_t;



//brand new virtual disk called disk_name with an empty FS. 0-> on success, -1-> on failure
int make_fs(char *disk_name);

//open disk_name and load its FS metadata into memory so file ops can run;0 -> on success, -1 -> if it cant be opened or is not a valid FS
int mount_fs(char *disk_name);

//flush all in-memory metadata to disk and close it.. 0 -> succes; -1 on write or close failure
int umount_fs(char *disk_name);

//week 2 file operations (we need this for the program to compile)

//open file fname for reading/writing.. returns a file descriptor (0–31) or -1
int  fs_open(char *fname);

//close the open file descriptor fildes; returns 0 or -1. 
int  fs_close(int fildes);

//create a new, empty file named fname; return 0 or -1. 
int  fs_create(char *fname);

//delete file fname and free its data blocks. returns 0 or -1. 
int  fs_delete(char *fname);

//read up to nbyte bytes from fildes into buf.. returns bytes read or -1 
int  fs_read(int fildes, void *buf, size_t nbyte);

//write nbyte bytes from buf into fildes, extending the file if needed.. returns bytes written or -1
int  fs_write(int fildes, void *buf, size_t nbyte);

//return the current file size in bytes for fildes, or -1 if invalid descriptor. 
int  fs_get_filesize(int fildes);

//move the file pointer of fildes to byte position offset; the offset must satisfy: 0 <= offset <= file_size; returns 0 or -1. 
int  fs_lseek(int fildes, off_t offset);

//shrink file fildes to length bytes;frees now-unused data blocks; returns 0 or -1
int  fs_truncate(int fildes, off_t length);

#endif 