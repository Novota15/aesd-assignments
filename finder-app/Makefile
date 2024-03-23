CROSS_COMPILE = aarch64-none-linux-gnu-
CC = gcc
TARGET = writer
SRC = writer.c

# Default to native compilation
all: native

native:
	$(CC) $(SRC) -o $(TARGET)

arm:
	$(CROSS_COMPILE)gcc $(SRC) -o $(TARGET)

clean:
	rm -rf $(TARGET)
