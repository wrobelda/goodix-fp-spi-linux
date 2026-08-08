PREFIX ?= /usr
UDEVRULESDIR ?= $(PREFIX)/lib/udev/rules.d
DESTDIR ?=

RULE = 60-goodix-qsee.rules

all:

check:
	udevadm verify $(RULE)
	test "$$(systemd-escape --template=qsee-app-loader@.service gfenu)" = \
		"qsee-app-loader@gfenu.service"

install:
	install -D -m 0644 $(RULE) $(DESTDIR)$(UDEVRULESDIR)/$(RULE)

.PHONY: all check install
