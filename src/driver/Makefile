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

.PHONY: all run clean mac

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: $(BIN)
	./$(BIN)

# macOS HAL plug-in: builds mac/build/WiMic.driver locally. Installing it into
# /Library/Audio/Plug-Ins/HAL/ and restarting coreaudiod is left as a manual step.
MAC_DIR    := mac
MAC_BUNDLE := $(MAC_DIR)/build/WiMic.driver
MAC_BIN    := $(MAC_BUNDLE)/Contents/MacOS/WiMic
MAC_PLIST  := $(MAC_BUNDLE)/Contents/Info.plist
MAC_CFLAGS := -Wall -Wextra -Wno-unused-parameter -std=c11 -O2 -bundle -arch arm64 -arch x86_64
MAC_LIBS   := -framework CoreAudio -framework CoreFoundation

mac: $(MAC_BIN) $(MAC_PLIST)
	codesign --force --sign - $(MAC_BUNDLE)

$(MAC_BIN): $(MAC_DIR)/macdriver.c
	@mkdir -p $(@D)
	xcrun clang $(MAC_CFLAGS) $< -o $@ $(MAC_LIBS)

$(MAC_PLIST): $(MAC_DIR)/Info.plist
	@mkdir -p $(@D)
	cp $< $@

clean:
	rm -f $(BIN)
	rm -rf $(MAC_DIR)/build
