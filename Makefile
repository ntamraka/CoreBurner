
# CoreBurner Makefile

CC = gcc
CFLAGS = -O2 -march=native -pthread -std=c11 -Wall -Wextra
LDFLAGS = -lm
TARGET = coreburner
SRC = coreburner.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install:
	cp $(TARGET) /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall

