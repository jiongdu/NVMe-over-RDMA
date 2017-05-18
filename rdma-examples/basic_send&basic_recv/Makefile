.PHONY: clean

CFLAGS  := -Wall -Werror -g
LD      := gcc
LDFLAGS := ${LDFLAGS} -libverbs

APPS    := basic_send basic_recv

all: ${APPS}

ibv_example: basic_send.o basic_recv.o
	${LD} -o $@ $^ ${LDFLAGS}

clean:
	rm -f *.o ${APPS}
