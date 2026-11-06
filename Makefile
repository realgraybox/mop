CC      ?= gcc
OBJCOPY ?= objcopy

TARGET_MACHINE := $(shell $(CC) -dumpmachine)

ifeq ($(findstring x86_64,$(TARGET_MACHINE)),x86_64)

ELF_FORMAT = elf64-x86-64
ELF_ARCH   = i386:x86-64

else ifneq ($(findstring i386,$(TARGET_MACHINE)),)
	
ELF_FORMAT = elf32-i386
ELF_ARCH   = i386
STATIC = -static

else ifneq ($(findstring i686,$(TARGET_MACHINE)),)

ELF_FORMAT = elf32-i386
ELF_ARCH   = i386

else

$(error Unsupported target: $(TARGET_MACHINE))

endif

CFLAGS  ?= -Os -std=gnu99 -Wall -Wextra -Wshadow -Wno-deprecated-declarations -ffunction-sections -fdata-sections \
			-I./audio -DAUDIO_HAVE_OSS -DAUDIO_HAVE_TINYALSA
			
LDFLAGS ?= $(STATIC) -Wl,--gc-sections,--sort-common,-s
LDLIBS  = -lm

TARGET  = mop

# Built-in bank: an AdLib .bnk or a DMX .op2 (GENMIDI), e.g.
#   make BANK=banks/GENMIDI.op2
# Add -DNO_BNK to CFLAGS for an OP2-only build (the built-in bank must then be .op2).
# Add -DMOP_STATS to print voice steal counts after playback.
BANK    ?= mop.bnk
BANKBIN = bank.bin
BANKOBJ = bank.o
# OP2: keep the header and 175 instruments; the names after them are not used
OP2_DATA_SIZE = 6308

SRCS    = mop.c audio/audio.c audio/pcm.c 

.PHONY: all clean FORCE

all: $(TARGET)

$(TARGET): $(SRCS) $(BANKOBJ) 
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(BANKOBJ) $(LDLIBS)

# Copy to a fixed name so the symbols are always _binary_bank_bin_*.
# FORCE: rebuild when BANK changes on the command line.
$(BANKBIN): $(BANK) FORCE
	case "$(BANK)" in \
		*.op2|*.OP2) head -c $(OP2_DATA_SIZE) "$(BANK)" > $@.tmp ;; \
		*) cp "$(BANK)" $@.tmp ;; \
	esac
	cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

$(BANKOBJ): $(BANKBIN)
	$(OBJCOPY) \
		-I binary \
		-O $(ELF_FORMAT) \
		-B $(ELF_ARCH) \
		$< $@
		
clean:
	rm -f $(TARGET) $(BANKOBJ) $(BANKBIN) $(BANKBIN).tmp *.o
