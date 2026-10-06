# Bare-metal Felix BASIC: boots straight to the BASIC prompt.
#
#   make floppy       1.44 MB boot floppy (felixbasic-floppy.img): GRUB, the
#                     kernel and the sample programs from ../test, on FAT12;
#                     LOAD/SAVE/FILES work on the same disk. The x86_64 one
#                     boots on UEFI too (EFI/BOOT/BOOTX64.EFI)
#   make run-floppy   boot the floppy in QEMU
#   make              the kernel alone (felixbasic.elf)
#   make run          boot the kernel directly in QEMU (files on a RAM disk)
#   make iso          bootable CD / USB image (felixbasic.iso), BIOS and UEFI
#                     (when grub-efi-amd64-bin is installed), with the
#                     sample programs on the RAM disk
#   make efi          UEFI application (felixbasic.efi) plus felixbasic.img,
#                     the sample programs on a FAT12 image it loads into the
#                     RAM disk; copy both to EFI/BOOT/ on a FAT USB stick,
#                     the .efi as BOOTX64.EFI
#   make run-efi      boot felixbasic.efi under UEFI firmware (OVMF) in QEMU
#   make sign KEY=db.key CERT=db.crt
#                     sign felixbasic.efi for Secure Boot (sbsign)
#
# ARCH=x86_64 (default) builds the 64-bit kernel, and the UEFI loader
# (make efi). ARCH=i386 builds a 32-bit
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
# 
# Version 20261004

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
# The screen's own resolution first (GRUB's "auto": the monitor's preferred
# mode), so the picture fills it; BASIC scales every mode to fit. GRUB falls
# back along the list.
GFXMODE  := auto,1024x768x32,800x600x32,640x480x32,640x480x16,640x480x8
# The boot loader picks the mode from GFXMODE (no preference in the kernel's
# header); 640x480 is for QEMU -kernel, which sets none.
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=32 -DMB_W=0 -DMB_H=0 -DMB_BPP=0
# PIE=1 builds the position-independent kernel that felixbasic.efi embeds.
ifeq ($(PIE),1)
ARCHFLAGS := -fpie -mno-red-zone -msse2
BOOTDEFS := -DEFI
else
ARCHFLAGS := -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -msse2
BOOTDEFS :=
endif
# Arrays take memory when they're DIMmed; these size the fixed tables.
LIMITS   := -DMAX_VARS=1024 -DMAX_ARRAY_SIZE=4194304 -DCTRL_STACK_MAX=1024 -DMAX_VARNAME=128 -DMAX_DIR_ENTRIES=512
else ifeq ($(ARCH),i386)
MFLAG    := -m32
SUFFIX   := -i386
BOOT_S   := boot32.S
# No xHCI before PCI Express, and its driver's 2 MB DMA pool is too much
# for a 386's memory.
USB_OBJ  := usb_none
LDEMU    := -m elf_i386
# 640x480 at 8 bits needs only a 512 KB VESA card, and is quick to redraw
# on a 386; BASIC scales it to fill the screen.
GFXMODE  := 640x480x8,640x480x16,640x480x15,640x480x32,800x600x8,1024x768x8,auto
FBDEFS   := -DFB_W=640 -DFB_H=480 -DFB_BPP=8
ARCHFLAGS := -fno-pic -fno-pie -march=i386 -mtune=i486 -mfpmath=387 -mno-sse
# 512 variables, 512-deep FOR/GOSUB nesting, 63-character names and
# 256-entry FILES listings: BASIC runs in a 4 MB machine.
LIMITS   := -DMAX_VARS=512 -DMAX_ARRAY_SIZE=32768 -DCTRL_STACK_MAX=512 -DMAX_VARNAME=64 -DMAX_DIR_ENTRIES=256
else
$(error ARCH must be x86_64 or i386)
endif

LIBGCC   := $(shell $(CXX) $(MFLAG) -print-libgcc-file-name)
BASE     := $(MFLAG) $(ARCHFLAGS) $(FBDEFS) -O2 -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-exceptions -fno-math-errno -fno-builtin-printf -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNDEBUG
CXXFLAGS := $(BASE) -std=gnu++17 -fno-rtti -fno-threadsafe-statics -Ioverrides -I$(SRC) -I. -Wno-write-strings
# The interpreter, as its own Makefile builds the SDL window version
# (graphics on) without GMP. -fpermissive and -w as there.
CORE_FLAGS := $(CXXFLAGS) -DUSE_SDL_WINDOW -DBASIC_BAREMETAL -DDONTUSEGMP $(LIMITS) -fpermissive -w
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
	$(CXX) $(MFLAG) $(FBDEFS) $(BOOTDEFS) $(if $(filter i386,$(ARCH)),-march=i386 -DCPU_386) -c $< -o $@

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
	$(CXX) $(CXXFLAGS) -DUSE_SDL_WINDOW -DBASIC_BAREMETAL -DDONTUSEGMP $(LIMITS) -w -c $< -o $@

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

# ---------------------------------------------------------------- UEFI
# felixbasic.efi = a small UEFI loader (gnu-efi, efi/loader.c) with the
# kernel embedded. gnu-efi's files live in different places on different
# distros, so find them. x86_64 only.
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_BUILD := build/efi
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_BUILD) -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall
DISK_IMG  := felixbasic.img

efi: felixbasic.efi $(DISK_IMG)

ifeq ($(ARCH),x86_64)
# The kernel again, position-independent, built in its own directory.
$(EFI_BUILD)/kernel.elf: FORCE
	$(MAKE) --no-print-directory ARCH=x86_64 PIE=1 BUILD=$(EFI_BUILD)/k $(EFI_BUILD)/k/kernel-pie.elf
	cp $(EFI_BUILD)/k/kernel-pie.elf $@

$(BUILD)/kernel-pie.elf: $(OBJS) efi/kernel.ld
	ld -nostdlib -pie --no-dynamic-linker -z notext -z max-page-size=0x1000 --no-warn-rwx-segments \
	   -T efi/kernel.ld $(OBJS) $(LIBGCC) -o $@

$(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h: $(EFI_BUILD)/kernel.elf
	objcopy -O binary $< $(EFI_BUILD)/kernel.bin
	@nm $< | awk '/ __kernel_end$$/ {print "#define KERNEL_MEM_SIZE 0x" $$1} \
	              / efi_entry64$$/  {print "#define KERNEL_ENTRY 0x" $$1} \
	              / __rela_start$$/ {print "#define KERNEL_RELA_START 0x" $$1} \
	              / __rela_end$$/   {print "#define KERNEL_RELA_END 0x" $$1}' > $(EFI_BUILD)/kernel_layout.h
	@test $$(grep -c define $(EFI_BUILD)/kernel_layout.h) = 4 || { echo "kernel layout symbols missing"; exit 1; }

felixbasic.efi: efi/loader.c efi/image.S $(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h
	@test -n "$(EFI_CRT0)" -a -n "$(EFI_LDS)" || { echo "gnu-efi not found (Debian: gnu-efi, Fedora: gnu-efi-devel)"; exit 1; }
	gcc $(EFI_CFLAGS) -c efi/loader.c -o $(EFI_BUILD)/loader.o
	gcc -c -DKERNEL_BIN='"$(EFI_BUILD)/kernel.bin"' efi/image.S -o $(EFI_BUILD)/image.o
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T $(EFI_LDS) $(EFI_CRT0) \
	   $(EFI_BUILD)/loader.o $(EFI_BUILD)/image.o $(EFI_LIBS) $(EFI_LIBS) -o $(EFI_BUILD)/felixbasic.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela \
	        -j '.rel.*' -j '.rela.*' -j .reloc -O efi-app-x86_64 --subsystem=10 $(EFI_BUILD)/felixbasic.so $@

# Secure Boot: sign with your own key (enrolled in db, or as a MOK for shim).
sign: felixbasic.efi
	@test -n "$(KEY)" -a -n "$(CERT)" || { echo "usage: make sign KEY=db.key CERT=db.crt"; exit 1; }
	sbsign --key $(KEY) --cert $(CERT) --output felixbasic-signed.efi felixbasic.efi
else
felixbasic.efi run-efi sign:
	@echo "UEFI is x86_64 only; build it without ARCH=i386"; exit 1
.PHONY: felixbasic.efi
endif

# ---------------------------------------------------------------- images
# The sample programs on a FAT12 image, which felixbasic.efi and the CD
# load into the RAM disk (changes last until the machine restarts).
# Outside the x86_64 block: the i386 CD needs it too.
$(DISK_IMG): $(wildcard $(TESTS)/*.bas $(TESTS)/*.BAS)
	rm -f $@
	$(MKFS_FAT) -C -F 12 -f 2 -r 224 -n FELIXBASIC $@ 1440 >/dev/null
	for f in $(TESTS)/*.bas $(TESTS)/*.BAS; do [ -e "$$f" ] && mcopy -i $@ "$$f" ::/ ; done; true

$(BUILD)/grub.cfg: grub.cfg
	@mkdir -p $(BUILD)
	sed 's/@GFXMODE@/$(GFXMODE)/' $< > $@

iso: $(ISO)
$(ISO): $(KERNEL) $(BUILD)/grub.cfg $(DISK_IMG)
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/felixbasic.elf
	cp $(DISK_IMG) $(BUILD)/iso/boot/felixbasic.img
	cp $(BUILD)/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# A FAT12 floppy: GRUB's boot sector (keeping the FAT header) in sector 0,
# GRUB's core image in the reserved sectors after it, then the kernel
# (gzipped; GRUB unpacks it), grub.cfg and the sample programs as files.
# Needs mtools and dosfstools.
floppy: $(FLOPPY)
# EFI_APP=felixbasic-signed.efi puts the signed one on (after make sign).
EFI_APP ?= felixbasic.efi
$(FLOPPY): $(KERNEL) $(BUILD)/grub.cfg $(wildcard $(TESTS)/*.bas $(TESTS)/*.BAS) $(if $(filter x86_64,$(ARCH)),$(EFI_APP))
	$(GRUB_MKIMAGE) -O i386-pc -o $(BUILD)/core.img -p /boot/grub \
	    biosdisk fat gzio normal configfile multiboot vbe video video_fb boot
	gzip -9 -n -c $(KERNEL) > $(BUILD)/kernel.gz
	rm -f $@
	$(MKFS_FAT) -C -F 12 -f 2 -r 224 -R $$(( $$(stat -c %s $(BUILD)/core.img) / 512 + 2 )) -n FELIXBASIC $@ 1440 >/dev/null
	mmd -i $@ ::/boot ::/boot/grub
	mcopy -i $@ $(BUILD)/kernel.gz ::/boot/felixbasic.elf
	mcopy -i $@ $(BUILD)/grub.cfg ::/boot/grub/grub.cfg
	$(if $(filter x86_64,$(ARCH)),mmd -i $@ ::/EFI ::/EFI/BOOT && mcopy -i $@ $(EFI_APP) ::/EFI/BOOT/BOOTX64.EFI)
	for f in $(TESTS)/*.bas $(TESTS)/*.BAS; do [ -f "$$f" ] && mcopy -i $@ "$$f" "::/$$(basename "$$f")"; done; true
	cp $(GRUB_I386)/boot.img $(BUILD)/boot.img
	dd if=$@ of=$(BUILD)/boot.img bs=1 skip=3 seek=3 count=59 conv=notrunc status=none
	dd if=$(BUILD)/boot.img of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BUILD)/core.img of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "$@: GRUB $$(( $$(stat -c %s $(BUILD)/core.img) / 1024 )) KB + kernel $$(( $$(stat -c %s $(BUILD)/kernel.gz) / 1024 )) KB (gzipped) + programs; $$(( $$(mdir -i $@ :: | sed -n 's/ *\([0-9 ]*\) bytes free/\1/p' | tr -d ' ') / 1024 )) KB free"

# ---------------------------------------------------------------- QEMU
# SOUND=hda (default for x86_64), ac97, sb (default for i386: a Sound
# Blaster, as a 386 would have had; QEMU's is an SB16, which plays the
# SB Pro's way too), speaker or none. The kernel uses whichever it finds.
AUDIODEV ?= $(if $(filter Darwin,$(shell uname)),coreaudio,pa)
comma := ,
QAUDIO_OPTS ?= timer-period=2000,out.buffer-length=10000$(if $(filter pa,$(AUDIODEV)),$(comma)out.latency=5000)
QSOUND_speaker := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -machine pcspk-audiodev=snd0
QSOUND_ac97    := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device AC97,audiodev=snd0
QSOUND_hda     := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device intel-hda -device hda-output,audiodev=snd0
QSOUND_sb      := -audiodev $(AUDIODEV),id=snd0,$(QAUDIO_OPTS) -device sb16,audiodev=snd0
QSOUND_none    :=
SOUND    ?= $(if $(filter i386,$(ARCH)),sb,hda)
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

ifeq ($(ARCH),x86_64)
# Boot felixbasic.efi under UEFI firmware (OVMF) from a folder QEMU presents
# as a FAT disk. OVMF needs its code image plus a writable copy of the
# matching variable store.
OVMF ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
          /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/ovmf-x86_64.bin))
OVMF_VARS ?= $(firstword $(wildcard $(subst CODE,VARS,$(OVMF))))
run-efi: felixbasic.efi $(DISK_IMG)
	@test -n "$(OVMF)" -a -n "$(OVMF_VARS)" || { echo "OVMF firmware not found (Debian: ovmf, Fedora: edk2-ovmf); set OVMF= and OVMF_VARS="; exit 1; }
	@rm -rf $(BUILD)/esp && mkdir -p $(BUILD)/esp/EFI/BOOT
	cp felixbasic.efi $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	cp $(DISK_IMG) $(BUILD)/esp/EFI/BOOT/
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	        -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd -drive format=raw,file=fat:rw:$(BUILD)/esp
endif

run-floppy: $(FLOPPY)
	$(QEMU) -drive file=$(FLOPPY),if=floppy,format=raw -boot a

# Both architectures' floppies.
floppies:
	$(MAKE) --no-print-directory ARCH=x86_64 floppy
	$(MAKE) --no-print-directory ARCH=i386 floppy

clean:
	rm -rf build felixbasic.elf felixbasic.iso felixbasic-floppy.img \
	       felixbasic-i386.elf felixbasic-i386.iso felixbasic-i386-floppy.img \
	       felixbasic.efi felixbasic-signed.efi felixbasic.img

.PHONY: all iso floppy floppies efi sign run run-iso run-floppy run-efi clean FORCE
