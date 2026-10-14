CC := cc
CFLAGS := -Wall -Wextra -std=c11
LDLIBS := -lopus
BIN := drv

ifeq ($(OS),Windows_NT)
# Running inside an MSYS2 UCRT64 shell: opus's headers/libs are already on
# the default search path (installed via pacman below), and sockets need
# Winsock linked in. Produce a real .exe, not an MSYS-runtime-dependent one.
LDLIBS += -lws2_32
BIN := driver.exe
else
# Homebrew namespaces its headers under include/opus/, and doesn't put
# /opt/homebrew on the default search path, so point the compiler at it
# directly rather than relying on pkg-config (not installed here).
OPUS_PREFIX := $(shell brew --prefix opus 2>/dev/null)
ifneq ($(OPUS_PREFIX),)
CFLAGS  += -I$(OPUS_PREFIX)/include
LDFLAGS += -L$(OPUS_PREFIX)/lib
endif
endif

SRC := udprec.c

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
