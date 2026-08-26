# stock PS2SDK homebrew build
EE_BIN = hwcap.elf
EE_OBJS = main.o
EE_CFLAGS += -O1

all: $(EE_BIN)
clean:
	rm -f $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
