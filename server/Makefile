CFLAGS+=-Wall
LDFLAGS+=-lpthread

%: %.c
	$(CC) $(CFLAGS) $^ $(CFLAGS) -o $@ $(LDFLAGS)

all: aesdsocket

clean:
	rm -f aesdsocket

valgrind: clean all
	valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all --track-origins=yes --errors-for-leak-kinds=definite --verbose --log-file=valgrind-out.txt ./aesdsocket