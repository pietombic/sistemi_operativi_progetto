XT_PRG_PREFIX = riscv64-unknown-elf-
CC      = $(XT_PRG_PREFIX)gcc
LD      = $(XT_PRG_PREFIX)ld
OBJCOPY = $(XT_PRG_PREFIX)objcopy

URISCV_DIR_PREFIX  = /usr/local
URISCV_DATA_DIR    = $(URISCV_DIR_PREFIX)/share/uriscv
URISCV_INCLUDE_DIR = /usr/include
URISCV_LIB_DIR     = /usr/lib/uriscv

CFLAGS = -ffreestanding -static -c -nostdlib \
         -I$(URISCV_INCLUDE_DIR) -Wall -O0 \
         -march=rv32imfd -mabi=ilp32d

LDFLAGS = -nostdlib -m elf32lriscv -T $(URISCV_DATA_DIR)/uriscvaout.ldscript

EF   = uriscv-elf2uriscv
UDEV = uriscv-mkdev

PROGRAMS = shell fibEight echo fibEleven uname calc date sl

all: $(addsuffix .uriscv, $(PROGRAMS))

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $<

print.o: print.c
	$(CC) $(CFLAGS) -o $@ $<

%.t: %.o print.o
	$(LD) $(LDFLAGS) $(URISCV_LIB_DIR)/crti.o $< print.o $(URISCV_LIB_DIR)/liburiscv.o -o $@

%.t.aout.uriscv: %.t
	$(EF) -a $<

%.uriscv: %.t.aout.uriscv
	$(UDEV) -f $@ $<

clean:
	rm -f *.o *.t *.uriscv
