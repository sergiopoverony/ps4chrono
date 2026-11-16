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

# make:     builds ps4chrono.elf (this is the payload)
# make bin: also builds ps4chrono.bin (experimental, needs python3)
all: $(ELF)

bin: $(BIN)

$(ELF): $(SRCS) $(wildcard *.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# .bin = experimental raw payload (see tools/mkbin.py). GoldHEN payload menu rejected it
# ("invalid payload"), so use the .elf.
$(BIN): $(ELF) tools/mkbin.py
	python3 tools/mkbin.py $(ELF) $@

clean:
	rm -f $(ELF) $(BIN)

send: $(ELF)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^

send-bin: $(BIN)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^

.PHONY: all bin clean send send-bin
