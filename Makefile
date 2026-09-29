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
BANK    = mop.bnk
BANKOBJ = mop.o

SRCS    = mop.c audio/audio.c audio/pcm.c 

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) $(BANKOBJ) 
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(BANKOBJ) $(LDLIBS)

$(BANKOBJ): $(BANK)
	$(OBJCOPY) \
		-I binary \
		-O $(ELF_FORMAT) \
		-B $(ELF_ARCH) \
		$< $@
		
clean:
	rm -f $(TARGET) $(BANKOBJ) $(BANKOBJ1) *.o
