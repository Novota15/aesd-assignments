CFLAGS+=-Wall

%: %.c
	$(CC) $^ $(CFLAGS) -o $@ $(LDFLAGS)

all: aesdsocket

clean:
	rm -f aesdsocket