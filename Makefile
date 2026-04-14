CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c17
PREFIX  ?= $(CURDIR)/bin

SRC_DIR = src
TARGETS = $(PREFIX)/zsh-capture-wrapper $(PREFIX)/clc

.PHONY: all clean install

all: $(TARGETS)

$(PREFIX):
	mkdir -p $(PREFIX)

$(PREFIX)/zsh-capture-wrapper: $(SRC_DIR)/wrapper.c | $(PREFIX)
	$(CC) $(CFLAGS) -o $@ $<

$(PREFIX)/clc: $(SRC_DIR)/clc.c | $(PREFIX)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

# Install to /usr/local/bin (optional, or just use from plugin dir)
install: all
	install -m 755 $(PREFIX)/zsh-capture-wrapper /usr/local/bin/
	install -m 755 $(PREFIX)/clc /usr/local/bin/
