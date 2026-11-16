PS4_HOST ?= 192.168.0.24
PS4_PORT ?= 9090

ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

ELF     ?= ps4chrono.elf
BIN     ?= ps4chrono.bin
SRCS    := main.cpp
CXXFLAGS := -std=c++17 -O2 -Wall

# make: builds both ps4chrono.elf and ps4chrono.bin
all: $(ELF) $(BIN)

$(ELF): $(SRCS) $(wildcard *.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# .bin = same payload for loaders that take raw .bin files (see tools/mkbin.py)
$(BIN): $(ELF) tools/mkbin.py
	python3 tools/mkbin.py $(ELF) $@

clean:
	rm -f $(ELF) $(BIN)

send: $(ELF)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^

send-bin: $(BIN)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^

.PHONY: all clean send send-bin
