# Bare-metal Felix BASIC: boots straight to the BASIC prompt.
#
#   make floppy       1.44 MB boot floppy (felixbasic-floppy.img): GRUB, the
#                     kernel and the sample programs from ../test, on FAT12;
#                     LOAD/SAVE/FILES work on the same disk
#   make run-floppy   boot the floppy in QEMU
#   make              the kernel alone (felixbasic.elf)
#   make run          boot the kernel directly in QEMU (files on a RAM disk)
#   make iso          bootable CD / USB image (felixbasic.iso)
#
# ARCH=x86_64 (default) builds the 64-bit kernel. ARCH=i386 builds a 32-bit
# one that runs on a 386 or later with a 387 (BASIC numbers are doubles);
# its outputs are named felixbasic-i386.*. The 32-bit build needs
# g++-multilib.
#
# The interpreter (../main.cpp, vars, expr, program, commands, basic_print)
# is compiled unchanged, with the SDL build's graphics enabled
# (USE_SDL_WINDOW) and GMP off. basic_gfx_bm.cpp, generated from
# ../basic_gfx_sdl.cpp by tools/make_gfx_bm.py, draws on the framebuffer;
# sound_bm.cpp, libc.cpp, storage.cpp and kernel.cpp stand in for SDL audio,
# the C library and the operating system. Nothing from libc or libstdc++ is
# linked.

ARCH     ?= x86_64
CXX      ?= g++
SRC      := ..
BUILD    ?= build/$(ARCH)
TESTS    ?= $(SRC)/test

GRUB_MKIMAGE  ?= $(shell command -v grub2-mkimage || command -v grub-mkimage)
GRUB_MKRESCUE ?= $(shell command -v grub2-mkrescue || command -v grub-mkrescue)
GRUB_I386     ?= $(firstword $(wildcard /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc))
MKFS_FAT      ?= $(shell command -v mkfs.fat || command -v mkfs.vfat || echo /usr/sbin/mkfs.fat)

ifeq ($(ARCH),x86_64)
MFLAG    := -m64
SUFFIX   :=
BOOT_S   := boot.S
USB_OBJ  := usb
LDEMU    :=
# VGA's 640x480 first: the 80x25 screen and every SCREEN mode fit it at
# whole-number scales. GRUB falls back along the list.
GFXMODE  := 640x480x32,640x480x16,640x480x8,800x600x32,1024x768x32,auto
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=32
ARCHFLAGS := -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -msse2
# Every variable carries its own array slots (../basic.h): 256 x 4096 is
# 16 MB, so BASIC runs in 32 MB. (The desktop build's 16384 x 4096 would
# be a gigabyte.)
LIMITS   := -DMAX_VARS=256 -DMAX_ARRAY_SIZE=4096 -DCTRL_STACK_MAX=4096 -DMAX_VARNAME=256
else ifeq ($(ARCH),i386)
MFLAG    := -m32
SUFFIX   := -i386
BOOT_S   := boot32.S
# No xHCI before PCI Express, and its driver's 2 MB DMA pool is too much
# for a 386's memory.
USB_OBJ  := usb_none
LDEMU    := -m elf_i386
# 640x480 at 8 bits needs only a 512 KB VESA card.
GFXMODE  := 640x480x8,640x480x16,640x480x15,640x480x32,800x600x8,1024x768x8,auto
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=8
ARCHFLAGS := -fno-pic -fno-pie -march=i386 -mtune=i486 -mfpmath=387 -mno-sse
# 200 variables x 1024-element arrays (about 2.5 MB), 512-deep FOR/GOSUB
# nesting, 63-character names and 256-entry FILES listings: BASIC fits in
# an 8 MB machine.
LIMITS   := -DMAX_VARS=200 -DMAX_ARRAY_SIZE=1024 -DCTRL_STACK_MAX=512 -DMAX_VARNAME=64 -DMAX_DIR_ENTRIES=256
else
$(error ARCH must be x86_64 or i386)
endif

LIBGCC   := $(shell $(CXX) $(MFLAG) -print-libgcc-file-name)
BASE     := $(MFLAG) $(ARCHFLAGS) $(FBDEFS) -O2 -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-exceptions -fno-math-errno -fno-builtin-printf -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNDEBUG
CXXFLAGS := $(BASE) -std=gnu++17 -fno-rtti -fno-threadsafe-statics -Ioverrides -I$(SRC) -I. -Wno-write-strings
# The interpreter, as its own Makefile builds the SDL window version
# (graphics on) without GMP. -fpermissive and -w as there.
CORE_FLAGS := $(CXXFLAGS) -DUSE_SDL_WINDOW -DDONTUSEGMP $(LIMITS) -fpermissive -w
# Interrupt-time code (and what it calls) never touches SSE or x87
# registers: the entry stubs save only the general-purpose ones.
IRQFLAGS := $(filter-out -msse2 -mfpmath=387,$(CXXFLAGS)) -mgeneral-regs-only
LDFLAGS  := $(LDEMU) -nostdlib -static -no-pie -T linker.ld -z max-page-size=0x1000 --no-warn-rwx-segments

KERNEL := felixbasic$(SUFFIX).elf
ISO    := felixbasic$(SUFFIX).iso
FLOPPY := felixbasic$(SUFFIX)-floppy.img

CORE     := main vars expr program commands basic_print
PLATFORM := kernel sound_bm libc runtime stdhooks rbtree pci $(USB_OBJ) floppy fat12 storage
IRQ_OBJS := irq sound_player audio

OBJS := $(BUILD)/boot.o $(BUILD)/setjmp.o \
        $(addprefix $(BUILD)/,$(addsuffix .o,$(PLATFORM) $(IRQ_OBJS))) \
        $(BUILD)/basic_gfx_bm.o \
        $(addprefix $(BUILD)/core/,$(addsuffix .o,$(CORE)))

HDRS      := $(wildcard *.hpp) font.h overrides/SDL2/SDL.h
CORE_HDRS := $(filter-out %/msvc_posix_compat.h,$(wildcard $(SRC)/*.h))

all: $(KERNEL)

$(BUILD)/boot.o: $(BOOT_S)
	@mkdir -p $(BUILD)
	$(CXX) $(MFLAG) $(FBDEFS) $(if $(filter i386,$(ARCH)),-march=i386 -DCPU_386) -c $< -o $@

$(BUILD)/setjmp.o: setjmp.S
	@mkdir -p $(BUILD)
	$(CXX) $(MFLAG) -c $< -o $@

$(addprefix $(BUILD)/,$(addsuffix .o,$(IRQ_OBJS))): $(BUILD)/%.o: %.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(IRQFLAGS) -c $< -o $@

$(BUILD)/sound_bm.o: sound_bm.cpp $(HDRS) $(CORE_HDRS) $(BUILD)/limits
	@mkdir -p $(BUILD)
	$(CXX) $(CORE_FLAGS) -c $< -o $@

$(BUILD)/kernel.o: kernel.cpp $(HDRS) $(CORE_HDRS) $(BUILD)/limits
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DUSE_SDL_WINDOW -DDONTUSEGMP $(LIMITS) -w -c $< -o $@

# Rebuild everything that depends on the limits when they change.
$(BUILD)/limits: FORCE
	@mkdir -p $(BUILD)
	@echo '$(LIMITS)' | cmp -s - $@ || echo '$(LIMITS)' > $@
FORCE:

$(BUILD)/%.o: %.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The display backend, derived from the SDL one.
$(BUILD)/basic_gfx_bm.cpp: $(SRC)/basic_gfx_sdl.cpp tools/make_gfx_bm.py
	@mkdir -p $(BUILD)
	python3 tools/make_gfx_bm.py $< $@

$(BUILD)/basic_gfx_bm.o: $(BUILD)/basic_gfx_bm.cpp $(HDRS) $(CORE_HDRS) $(BUILD)/limits
	$(CXX) $(CORE_FLAGS) -c $< -o $@

$(BUILD)/core/%.o: $(SRC)/%.cpp $(CORE_HDRS) overrides/SDL2/SDL.h $(BUILD)/limits
	@mkdir -p $(BUILD)/core
	$(CXX) $(CORE_FLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)
# Link as ELF64, then wrap as ELF32 so QEMU's -kernel and GRUB's multiboot
# loader both accept it. The 32-bit entry stub switches to long mode.
$(KERNEL): $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $(BUILD)/felixbasic64.elf
	objcopy -O elf32-i386 $(BUILD)/felixbasic64.elf $@
else
$(KERNEL): $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $@
endif

# ---------------------------------------------------------------- images
$(BUILD)/grub.cfg: grub.cfg
	@mkdir -p $(BUILD)
	sed 's/@GFXMODE@/$(GFXMODE)/' $< > $@

iso: $(ISO)
$(ISO): $(KERNEL) $(BUILD)/grub.cfg
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/felixbasic.elf
	cp $(BUILD)/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# A FAT12 floppy: GRUB's boot sector (keeping the FAT header) in sector 0,
# GRUB's core image in the reserved sectors after it, then the kernel
# (gzipped; GRUB unpacks it), grub.cfg and the sample programs as files.
# Needs mtools and dosfstools.
floppy: $(FLOPPY)
$(FLOPPY): $(KERNEL) $(BUILD)/grub.cfg $(wildcard $(TESTS)/*.bas $(TESTS)/*.BAS)
	$(GRUB_MKIMAGE) -O i386-pc -o $(BUILD)/core.img -p /boot/grub \
	    biosdisk fat gzio normal configfile multiboot vbe video video_fb boot
	gzip -9 -n -c $(KERNEL) > $(BUILD)/kernel.gz
	rm -f $@
	$(MKFS_FAT) -C -F 12 -f 2 -r 224 -R $$(( $$(stat -c %s $(BUILD)/core.img) / 512 + 2 )) -n FELIXBASIC $@ 1440 >/dev/null
	mmd -i $@ ::/boot ::/boot/grub
	mcopy -i $@ $(BUILD)/kernel.gz ::/boot/felixbasic.elf
	mcopy -i $@ $(BUILD)/grub.cfg ::/boot/grub/grub.cfg
	for f in $(TESTS)/*.bas $(TESTS)/*.BAS; do [ -f "$$f" ] && mcopy -i $@ "$$f" "::/$$(basename "$$f")"; done; true
	cp $(GRUB_I386)/boot.img $(BUILD)/boot.img
	dd if=$@ of=$(BUILD)/boot.img bs=1 skip=3 seek=3 count=59 conv=notrunc status=none
	dd if=$(BUILD)/boot.img of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BUILD)/core.img of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "$@: GRUB $$(( $$(stat -c %s $(BUILD)/core.img) / 1024 )) KB + kernel $$(( $$(stat -c %s $(BUILD)/kernel.gz) / 1024 )) KB (gzipped) + programs; $$(( $$(mdir -i $@ :: | sed -n 's/ *\([0-9 ]*\) bytes free/\1/p' | tr -d ' ') / 1024 )) KB free"

# ---------------------------------------------------------------- QEMU
# SOUND=hda (default for x86_64), ac97, speaker (default for i386: a 386
# had no PCI sound card) or none. The kernel uses whichever it finds.
AUDIODEV ?= $(if $(filter Darwin,$(shell uname)),coreaudio,pa)
comma := ,
QAUDIO_OPTS ?= timer-period=2000,out.buffer-length=10000$(if $(filter pa,$(AUDIODEV)),$(comma)out.latency=5000)
QSOUND_speaker := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -machine pcspk-audiodev=snd0
QSOUND_ac97    := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device AC97,audiodev=snd0
QSOUND_hda     := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device intel-hda -device hda-output,audiodev=snd0
QSOUND_none    :=
SOUND    ?= $(if $(filter i386,$(ARCH)),speaker,hda)
KVM      ?= $(if $(wildcard /dev/kvm),-enable-kvm,)
ifeq ($(ARCH),x86_64)
QEMU     := qemu-system-x86_64 $(KVM) -m 256 -serial stdio $(QSOUND_$(SOUND))
else
# The oldest CPU QEMU models is a 486; 8 MB.
QEMU     := qemu-system-i386 -cpu 486 -m 8 -serial stdio $(QSOUND_$(SOUND))
endif

run: $(KERNEL)
	$(QEMU) -kernel $(KERNEL) -append "$(if $(filter speaker,$(SOUND)),audio=speaker,audio=$(SOUND)) $(ARGS)"

run-iso: $(ISO)
	$(QEMU) -cdrom $(ISO)

run-floppy: $(FLOPPY)
	$(QEMU) -drive file=$(FLOPPY),if=floppy,format=raw -boot a

# Both architectures' floppies.
floppies:
	$(MAKE) --no-print-directory ARCH=x86_64 floppy
	$(MAKE) --no-print-directory ARCH=i386 floppy

clean:
	rm -rf build felixbasic.elf felixbasic.iso felixbasic-floppy.img \
	       felixbasic-i386.elf felixbasic-i386.iso felixbasic-i386-floppy.img

.PHONY: all iso floppy floppies run run-iso run-floppy clean FORCE
