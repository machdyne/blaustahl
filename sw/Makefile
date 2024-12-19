CFLAGS += -Wall -DBACKEND_LIBUSB

bs: bs.c
	$(CC) $(CFLAGS) $(LDFLAGS) bs.c -lusb-1.0 -o bs

install: bs
	cp bs /usr/local/bin

clean:
	rm -f bs

.PHONY: clean
