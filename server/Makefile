# CROSS_COMPILE = aarch64-none-linux-gnu-


ifdef CROSS_COMPILE
  CC:=$(CROSS_COMPILE)gcc
else
  CC:=gcc
endif


CFLAGS=-I. -g -Wall -pthread  -lpthread -lrt

ODIR=obj

OBJ = aesdsocket.o

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

aesdsocket : $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)
	
clean:
	rm -f ./*.o *~
	
all: clean aesdsocket
default: aesdsocket 
