.PHONY: clean

CFLAGS  := -Wall -Werror -g
LD      := gcc
LDFLAGS := ${LDFLAGS} -lrdmacm -L/lib64 -libverbs

APPS    := rdma_server rdma_client

all: ${APPS}

ibv_basic_send: rdma_server.o rdma_client.o
	${LD} -o $@ $^ ${LDFLAGS}

clean:
	rm -f *.o ${APPS}
