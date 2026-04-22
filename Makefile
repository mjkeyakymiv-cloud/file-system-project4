CC= gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g

OBJS = disk.o fs.o

all: demo

demo: $(OBJS) demo.o
$(CC) $(CFLAGS) -o $@ $^ -lpthread

disk.o: disk.c disk.h
$(CC) $(CFLAGS) -c -o $@ $<

fs.o: fs.c fs.h disk.h
$(CC) $(CFLAGS) -c -o $@ $<

demo.o: demo.c fs.h disk.h
$(CC) $(CFLAGS) -c -o $@ $<

run: demo
./demo

clean:
rm -f *.o demo *.vdisk

.PHONY: all run clean

