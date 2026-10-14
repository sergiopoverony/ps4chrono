PS4_HOST ?= 192.168.0.24
PS4_PORT ?= 9090

ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

ELF     ?= ps4chrono.elf
SRCS    := main.cpp
CXXFLAGS := -std=c++17 -O2 -Wall

all: $(ELF)

$(ELF): $(SRCS) $(wildcard *.h)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(ELF)

send: $(ELF)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $^
