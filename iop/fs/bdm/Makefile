# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.

# IOP_CFLAGS += -DDEBUG

IOP_OBJS = main.o bdm.o part_driver.o imports.o exports.o
IOP_LIBS = -lbdm
IOP_CFLAGS = -I$(PS2SDKSRC)/iop/fs/libbdm/include/
IOP_LDFLAGS = -L$(PS2SDKSRC)/iop/fs/libbdm/lib/

include $(PS2SDKSRC)/Defs.make
include $(PS2SDKSRC)/iop/Rules.bin.make
include $(PS2SDKSRC)/iop/Rules.make
include $(PS2SDKSRC)/iop/Rules.release
