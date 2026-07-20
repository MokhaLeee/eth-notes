CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lrdmacm -libverbs

.PHONY: all clean

all: rdma_demo

rdma_demo: rdma_demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_demo
