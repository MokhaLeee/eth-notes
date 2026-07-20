CC        := gcc
RDMA_CORE ?= $(HOME)/rdma-core/build

CFLAGS  := -Wall -Wextra -O2 -g \
           -I$(RDMA_CORE)/include

LDFLAGS := -L$(RDMA_CORE)/lib \
           -Wl,-rpath,$(RDMA_CORE)/lib \
           -lrdmacm -libverbs

.PHONY: all clean

all: rdma_demo

rdma_demo: rdma_demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rdma_demo
