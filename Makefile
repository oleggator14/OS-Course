CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -g -pthread
TARGET  = tankfire
SRCS    = src/main.c src/sim.c
LDFLAGS = -pthread

$(TARGET): $(SRCS) src/sim.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
