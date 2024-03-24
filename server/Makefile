CFLAGS+=-Wall -pthread
LDFLAGS+=-pthread

# Default rule for building .c files into executables
%: %.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Target to build aesdsocket
all: aesdsocket

# Target to clean up the built program
clean:
	rm -f aesdsocket