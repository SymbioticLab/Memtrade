.PHONY: clean

CFLAGS  := -Wall -Werror -g
LD      := gcc
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs -lpthread

APPS    := infiniswap-daemon central-coordinator

all: ${APPS}

infiniswap-daemon: rdma-common.o infiniswap-daemon.o
	${LD} -o $@ $^ ${LDLIBS}

central-coordinator: rdma-common.o central-coordinator.o
	${LD} -o $@ $^ ${LDLIBS}

clean:
	rm -f *.o ${APPS}

