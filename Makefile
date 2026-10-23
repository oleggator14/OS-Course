CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -g
TARGET  = tankfire
SRCS    = src/main.c src/sim.c

$(TARGET): $(SRCS) src/sim.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: clean
