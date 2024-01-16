# CROSS_COMPILE = aarch64-none-linux-gnu-
CC?=${CROSS_COMPILE}gcc

# ifdef CROSS_COMPILE
#   CC:=$(CROSS_COMPILE)gcc
# else
#   CC:=gcc
# endif


CFLAGS=-I. -g -Wall
LDFLAGS?= -lrt -lpthread -pthread
ODIR=obj
DEBUG_OPTS = -o0 -ggdb

OBJ = aesdsocket.o

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS) $(LDFLAGS)

aesdsocket : $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(DEBUG_OPTS)
	
clean:
	rm -f ./*.o *~

aesdsocketd: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS) $(LDFLAGS)
	
all: clean aesdsocket
default: aesdsocket 
