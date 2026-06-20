# musicstream.irx — custom IOP music streamer (see musicstream.c).
# Built with the ps2sdk IOP toolchain (mipsel-none-elf-) via the standard
# Makefile.iopglobal rules. No exports.tab: the module only registers an RPC
# server, so Makefile.iopglobal auto-adds srxfixup --allow-zero-text.

IOP_BIN  = musicstream.irx
IOP_OBJS = musicstream.o imports.o

all: $(IOP_BIN)

clean:
	rm -f $(IOP_BIN) $(IOP_BIN:.irx=.notiopmod.elf) $(IOP_BIN:.irx=.notiopmod.stripped.elf)
	rm -rf obj

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
