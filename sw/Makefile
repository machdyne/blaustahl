prefix = /usr/local
bindir = $(prefix)/bin

CFLAGS += -Wall -DBACKEND_LIBUSB

bs: bs.c
	$(CC) $(CFLAGS) $(LDFLAGS) bs.c -lusb-1.0 -o bs

install: bs
	install -D bs "$(DESTDIR)$(bindir)"

clean:
	rm -f bs

.PHONY: install clean
