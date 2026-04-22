#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "fs.h"

//the disk name used throughout the demo
#define DISK_NAME "demo.vdisk"

//the file names
#define FILE_A "fileA.txt"
#define FILE_B "fileB.txt"

//how much data we will write, this spans two disk blocks to test block chaining
#define DATA_SIZE (BLOCK_SIZE * 2)

//write_buf is global so the worker thread can access it to verify reads
static char *write_buf = NULL;

//simple pass/fail helper, it prints result and returns 1 on failure
static int check(const char *label, int result, int expected)
{
    if (result == expected)
    {
        printf("  PASS  %s\n", label);
        return 0;
    } else
    {
        printf("  FAIL  %s  (got %d, expected %d)\n", label, result, expected);
        return 1;
    }
}

//hexdump helper, it prints a region of the disk file using xxd
static void hexdump(const char *label, int skip_bytes, int count)
{
    printf("\n    Hexdump: %s    \n", label);
    char cmd[256];
    //xxd -s skips to the byte offset, -l limits how many bytes to show
    snprintf(cmd, sizeof(cmd), "xxd -s %d -l %d %s", skip_bytes, count, DISK_NAME);
    system(cmd);
    printf("---\n\n");
}

//worker thread, mounts disk, reads/verifies data, copies file, deletes orignal,
//tests remaing functions (lseek, truncate, get_filesize), shows hexdumps
static void *worker(void *arg)
{
    (void)arg;
    int failures = 0;
    int fd, fd2, ret;

    //mount the disk so we can access the file system
    failures += check("thread: mount_fs", mount_fs(DISK_NAME), 0);

    printf("\n=== Step 3: Re-open and read from various locations ===\n");

    //we open the file again ->the file pointer resets to 0
    fd = fs_open(FILE_A);
    failures += check("fs_open fileA (second open)", fd >= 0 ? 0 : -1, 0);

    //we read from the beginning
    char *read_buf = malloc(DATA_SIZE);
    if (!read_buf)
    {
        fprintf(stderr, "malloc failed\n"); return NULL;
    }

    ret = fs_read(fd, read_buf, 512);
    failures += check("fs_read 512 bytes from start", ret, 512);
    failures += check("data matches at start",
                      memcmp(read_buf, write_buf, 512), 0);

    //seek to the middle of block 1 and read, offset 1000 is still within the first data block (block size = 4096)
    failures += check("fs_lseek to offset 1000", fs_lseek(fd, 1000), 0);
    ret = fs_read(fd, read_buf, 512);
    failures += check("fs_read 512 bytes from offset 1000", ret, 512);
    failures += check("data matches at offset 1000",
                      memcmp(read_buf, write_buf + 1000, 512), 0);

    //seek to the second block and read, offset BLOCK_SIZE crosses into the second data block
    failures += check("fs_lseek to start of block 2",
                      fs_lseek(fd, BLOCK_SIZE), 0);
    ret = fs_read(fd, read_buf, 512);
    failures += check("fs_read 512 bytes from block 2", ret, 512);
    failures += check("data matches at block 2",
                      memcmp(read_buf, write_buf + BLOCK_SIZE, 512), 0);

    //we try to seek past end of file (should fail)
    failures += check("fs_lseek past EOF returns -1",
                      fs_lseek(fd, DATA_SIZE + 1), -1);

    //we truncate the file to half its size
    failures += check("fs_truncate to BLOCK_SIZE",
                      fs_truncate(fd, BLOCK_SIZE), 0);
    failures += check("fs_get_filesize after truncate",
                      fs_get_filesize(fd), BLOCK_SIZE);

    failures += check("fs_close after reads", fs_close(fd), 0);

    printf("\n=== Step 4: Copy fileA to fileB ===\n");

    //we create the destination file
    failures += check("fs_create fileB", fs_create(FILE_B), 0);

    //we open source (fileA, now truncated to BLOCK_SIZE) and destination (fileB)
    fd  = fs_open(FILE_A);
    fd2 = fs_open(FILE_B);
    failures += check("fs_open fileA for copy src", fd  >= 0 ? 0 : -1, 0);
    failures += check("fs_open fileB for copy dst", fd2 >= 0 ? 0 : -1, 0);

    //we read all of fileA then write into fileB
    char *copy_buf = malloc(BLOCK_SIZE);
    if (!copy_buf) { fprintf(stderr, "malloc failed\n"); return NULL; }

    ret = fs_read(fd, copy_buf, BLOCK_SIZE);
    failures += check("fs_read fileA for copy", ret, BLOCK_SIZE);

    ret = fs_write(fd2, copy_buf, BLOCK_SIZE);
    failures += check("fs_write fileB copy", ret, BLOCK_SIZE);

    //verify the copy matches the original
    char *verify_buf = malloc(BLOCK_SIZE);
    if (!verify_buf) { fprintf(stderr, "malloc failed\n"); return NULL; }

    fs_lseek(fd2, 0); //rewind fileB to verify
    fs_read(fd2, verify_buf, BLOCK_SIZE);
    failures += check("copy data matches original",
                      memcmp(copy_buf, verify_buf, BLOCK_SIZE), 0);

    failures += check("fs_close fileA", fs_close(fd),  0);
    failures += check("fs_close fileB", fs_close(fd2), 0);

    printf("\n=== Step 5: Delete original file and verify directory ===\n");

    //fs_delete should free fileA's data blcks and remove its directory entry
    failures += check("fs_delete fileA", fs_delete(FILE_A), 0);

    //trying to open a deleted file should fail
    failures += check("fs_open deleted fileA returns -1",
                      fs_open(FILE_A), -1);

    //fileB should still be accessible
    fd = fs_open(FILE_B);
    failures += check("fs_open fileB still works", fd >= 0 ? 0 : -1, 0);
    failures += check("fileB size is still BLOCK_SIZE",
                      fs_get_filesize(fd), BLOCK_SIZE);
    failures += check("fs_close fileB", fs_close(fd), 0);

    //deleting a file that is open should fail
    fd = fs_open(FILE_B);
    failures += check("fs_delete open file returns -1",
                      fs_delete(FILE_B), -1);
    fs_close(fd);

    //now that it is closed, delete should succeed
    failures += check("fs_delete fileB after close", fs_delete(FILE_B), 0);

    //hexdump root directory and FAT to show files are deleted and blocks are freed
    hexdump("root directory after deletion (block 3)", 3 * BLOCK_SIZE, 128);
    hexdump("FAT after deletion (block 1)", 1 * BLOCK_SIZE, 64);

    printf("\n=== Step 6: Unmount and remount to verify persistence ===\n");

    //umount_fs flushes all changes to disk
    failures += check("umount_fs", umount_fs(DISK_NAME), 0);

    //we mount again, this reads everything back from the disk file
    failures += check("mount_fs again", mount_fs(DISK_NAME), 0);

    //both files were deleted before unmount, so neither should exist
    failures += check("fileA gone after remount", fs_open(FILE_A), -1);
    failures += check("fileB gone after remount", fs_open(FILE_B), -1);

    //final unmount
    failures += check("final umount_fs", umount_fs(DISK_NAME), 0);

    free(read_buf);
    free(copy_buf);
    free(verify_buf);

    int *result = malloc(sizeof(int));
    *result = failures;
    return result;
}


int main(void)
{
    int failures = 0;
    int fd, ret;

    printf("\n=== Step 1: Create disk volume ===\n");

    //make_fs creates the virtual disk file and formats it with an empty FS
    failures += check("make_fs", make_fs(DISK_NAME), 0);

    //hexdump the superblock to show the magic number and layout info
    hexdump("superblock after make_fs (block 0)", 0, 32);

    //mount_fs loads the metadata into RAM so file operations can run
    failures += check("mount_fs", mount_fs(DISK_NAME), 0);

    printf("\n=== Step 2: Create a file and write data ===\n");

    //fs_create adds a new empty entry to the root directory
    failures += check("fs_create fileA", fs_create(FILE_A), 0);

    //fs_open returns a file descriptor; file pointer starts at offset 0
    fd = fs_open(FILE_A);
    failures += check("fs_open fileA", fd >= 0 ? 0 : -1, 0);

    //build the write buffer, we fill with a repeating pattern so we can verify reads
    //write_buf is global so the worker thread can access it
    write_buf = malloc(DATA_SIZE);
    if (!write_buf)
    {
        fprintf(stderr, "malloc failed\n"); return 1;
    }
    for (int i = 0; i < DATA_SIZE; i++)
    {
        write_buf[i] = (char)(i % 256);
    }

    //fs_write should write all DATA_SIZE bytes and advance the file pointer
    ret = fs_write(fd, write_buf, DATA_SIZE);
    failures += check("fs_write DATA_SIZE bytes", ret, DATA_SIZE);

    //fs_get_filesize should now return DATA_SIZE
    failures += check("fs_get_filesize after write", fs_get_filesize(fd), DATA_SIZE);

    //close the file before unmounting
    failures += check("fs_close after write", fs_close(fd), 0);

    //unmount so the thread gets a clean mount
    failures += check("umount_fs before thread", umount_fs(DISK_NAME), 0);

    //hexdump root directory and FAT to show fileA is present and blocks are allocated
    hexdump("root directory after write (block 3)", 3 * BLOCK_SIZE, 64);
    hexdump("FAT after write (block 1)", 1 * BLOCK_SIZE, 32);

    printf("\n=== Step 3-6: Spawning worker thread ===\n");

    //create the thread —>it will mount the disk and do all remaining work
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) != 0)
    {
        fprintf(stderr, "pthread_create failed\n"); return 1;
    }

    //join the thread and wait for it to finish before we continue
    //joining ensures everything runs in order as required
    void *thread_ret;
    pthread_join(thread, &thread_ret);

    //collect failure count from the thread
    if (thread_ret)
    {
        failures += *(int *)thread_ret;
        free(thread_ret);
    }

    printf("\n=== Results ===\n");
    if (failures == 0)
    {
        printf("All tests passed.\n\n");
    } else
    {
        printf("%d test(s) FAILED.\n\n", failures);
    }

    free(write_buf);

    return failures > 0 ? 1 : 0;
}