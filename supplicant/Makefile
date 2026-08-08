CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
LDLIBS += -pthread

COMMON = src/path.c src/services.c src/fs.c src/gpfs.c

PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= $(PREFIX)/lib/systemd/system
SYSCONFDIR ?= /etc
DESTDIR ?=

.PHONY: all check clean install
all: qsee-supplicant

qsee-supplicant: src/main.c src/transport_qseecom.c $(COMMON)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

test-protocol: tests/test_protocol.c $(COMMON)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

check: test-protocol
	./test-protocol

install: qsee-supplicant
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(UNITDIR) $(DESTDIR)$(SYSCONFDIR)/init.d
	install -m 0755 qsee-supplicant $(DESTDIR)$(SBINDIR)/qsee-supplicant
	install -m 0644 packaging/qsee-supplicant.service $(DESTDIR)$(UNITDIR)/
	install -m 0755 packaging/qsee-supplicant.openrc $(DESTDIR)$(SYSCONFDIR)/init.d/qsee-supplicant

clean:
	rm -f qsee-supplicant test-protocol
