TARGET = pi
CC ?= cc
CFLAGS ?= -Wall -O0 -g -pthread
LFLAGS ?= -lm
OBJS := pi.o tpool.o

all: pi

pi: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o $(TARGET) *~
