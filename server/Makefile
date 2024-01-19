CC?=${CROSS_COMPILE}gcc

CFLAGS ?=-I. -g -Wall
LDFLAGS ?= -lrt -lpthread -pthread
ODIR ?=obj
DEBUG_OPTS ?= -o $(TARGET) -ggdb
TARGET ?=aesdsocket

OBJ ?= aesdsocket.o

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS) $(LDFLAGS)

aesdsocket : $(OBJ)
	$(CC) -o $(TARGET) $^ $(CFLAGS) $(LIBS) $(LDFLAGS)
	
clean:
	rm -f ./*.o *~ ./$(TARGET)

debug: $(OBJ)
	$(CC) -o $(TARGET) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(DEBUG_OPTS)
	
all: clean aesdsocket
default: aesdsocket 

run: kill clean debug
	./$(TARGET)

kill:
	-killall aesdsocket
