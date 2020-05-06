.PHONY: clean

CFLAGS  := -Wall -Werror -g
LD      := gcc
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs -lpthread

APPS    := coordinator producer

all: ${APPS}

# coordinator: coordinator.o
# 	${LD} -o $@ $^ ${LDLIBS}

# producer: producer.o
# 	${LD} -o $@ $^ ${LDLIBS}

coordinator: rcommon
	$(LD) $(CFLAGS) -c ./coordinator.c
	$(LD) $(LDFLAGS) ./rcommon.o ./coordinator.o -o coordinator  ${LDLIBS}

producer: rcommon
	$(LD) $(CFLAGS) -c ./producer.c 
	$(LD) $(LDFLAGS) ./rcommon.o ./producer.o -o producer  ${LDLIBS}

rcommon:
	$(LD) $(CFLAGS) -c ./rcommon.c

clean:
	rm -f coordinator producer *.o ${APPS}

