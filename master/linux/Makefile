# The master, built on the Linux host it runs on (a Raspberry Pi, say):
#   make && sudo make install
CFLAGS   ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -I../../include
PREFIX ?= /usr/local

tmptun: tmptun.c ../../include/tmp_v2.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tmptun.c

install: tmptun
	install -m 0755 tmptun $(PREFIX)/bin/tmptun
	install -m 0755 tunnel-up.sh $(PREFIX)/sbin/tunnel-up.sh
	install -m 0644 tmptun.service /etc/systemd/system/tmptun.service

clean:
	rm -f tmptun

.PHONY: install clean
