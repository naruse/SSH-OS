# SSH OS build.
#
#   make             build the floppy images in build/
#   make test        boot headlessly in the emulator; writes build/test.png and
#                    build/test.serial.txt, fails if the kernel does not come up
#   make test-crash  same, with a kernel that crashes on purpose, to check the
#                    crash screen (uses build-crash/)
#   make test-scsi   boot with a scratch hard disk at SCSI ID 0 and exercise
#                    the SCSI driver: scan, read a block, write test, speed
#   make test-net    boot with the emulated network adapter and ping Snow's
#                    built-in gateway
#   make test-disk   the SSH OS disk: format it, save to it, restart, read back
#   make test-fs-host    the file system on this computer against a second
#                    implementation, including power cuts while writing
#   make test-net-host   run the network stack on this computer against a
#                    simulated network (tests/net_test.c): DHCP, DNS, errors
#   make test-all    every test above
#   make run         open the Snow app with the ROM and the 800K floppy
#   make tools       rebuild the headless runner (tools/snowrun) against Snow
#   make clean

CROSS   := m68k-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

ROM      := B306E171 - Mac SE FDHD.ROM
SNOW_APP := emu/Snow.app/Contents/MacOS/Snow
SNOW_SRC ?= $(HOME)/.cache/juanos/snow-src
JUANRUN  ?= $(SNOW_SRC)/target/release/juanrun

# Where the boot block loads the kernel. Must be clear of everything the ROM
# is still using while it reads the disk (its heap, stack and globals).
KERNEL_ADDR := 0x40000

B      ?= build
KFLAGS ?=

# The settings built into the kernel: your config.txt if there is one,
# otherwise the example.
CONFIG ?= $(if $(wildcard config.txt),config.txt,config.example.txt)
# The tests that type commands need a disk that starts in the OSConsole, not in
# the home screen, whatever your own settings say.
ifneq ($(filter test test-hdboot test-scsi test-net test-tcp bench test-all,$(MAKECMDGOALS)),)
CONFIG := tests/config-prompt.txt
endif

CFLAGS := -m68000 -O2 -std=gnu11 -Wall -Wextra \
          -ffreestanding -fno-builtin -fomit-frame-pointer \
          -fno-tree-loop-distribute-patterns \
          -fno-delete-null-pointer-checks --param=min-pagesize=0 \
          -Ikernel $(KFLAGS)
ASFLAGS := -m68000 -Wa,--register-prefix-optional

KOBJS := $(addprefix $(B)/, start.o kmain.o machine.o console.o serial.o \
           kprintf.o trap.o via.o adb.o kbd.o timer.o scsi.o dayna.o net.o tcp.o \
           config.o cfgdata.o sha256.o gfx.o shell.o \
           cmd_tcp.o x25519.o sha512.o ed25519.o poly1305.o polymul.o copro.o rng.o ssh.o bench.o lib.o \
           desk.o bar.o barmenu.o snd.o wifi.o hwtest.o fs.o saved.o ask.o connect.o vt.o splash.o settings.o banner.o icons.o font6x10.o)

all: $(B)/sshos-800k.dsk $(B)/sshos-1440k.dsk $(B)/sshos-boot.hda

$(B):
	mkdir -p $(B)

# ---------------------------------------------------------------- kernel --
$(B)/font6x10.c: fonts/juan6x10.txt tools/mkfont.py tools/model/charset.py tools/model/vt.py | $(B)
	python3 tools/mkfont.py $< $@

$(B)/icons.c: fonts/icons.txt tools/mkicons.py | $(B)
	python3 tools/mkicons.py $< $@

$(B)/icons.o: $(B)/icons.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/font6x10.o: $(B)/font6x10.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: kernel/%.c kernel/kernel.h kernel/hw.h kernel/types.h kernel/net.h \
           kernel/net_internal.h kernel/shell.h kernel/fe25519.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: kernel/%.S | $(B)
	$(CC) $(ASFLAGS) -c -o $@ $<

# Copied whenever its content differs, so that switching between settings
# files (as the tests do) always rebuilds, whatever the files' dates.
$(B)/config.txt: $(CONFIG) FORCE | $(B)
	@cmp -s "$(CONFIG)" $@ || cp "$(CONFIG)" $@

FORCE:

$(B)/cfgdata.o: kernel/cfgdata.S $(B)/config.txt
	$(CC) $(ASFLAGS) -Wa,-I$(B) -c -o $@ $<

# start.o must be first so _start is the first byte of the image. libgcc
# supplies 32-bit multiply and divide, which the 68000 lacks.
$(B)/kernel.elf: $(KOBJS) kernel/kernel.ld
	$(CC) -m68000 -nostdlib -Wl,-T,kernel/kernel.ld \
		-Wl,--defsym,KERNEL_ADDR=$(KERNEL_ADDR) -Wl,-Map,$(B)/kernel.map -Wl,--no-warn-rwx-segments \
		-o $@ $(KOBJS) -lgcc

$(B)/kernel.bin: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# ------------------------------------------------------------ boot block --
# The name in big letters: for the loader, and the same pixels as a C table
# for the kernel's start-up picture.
$(B)/banner.inc $(B)/banner.c: tools/mkbanner.py fonts/icons.txt | $(B)
	python3 tools/mkbanner.py $(B)/banner.inc $(B)/banner.c

$(B)/banner.o: $(B)/banner.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The boot block is told how many bytes to read: the kernel size rounded up to
# whole 512-byte sectors.
$(B)/boot.o: boot/boot.S $(B)/banner.inc $(B)/kernel.bin
	$(CC) $(ASFLAGS) -c -I$(B) -DKERNEL_ADDR=$(KERNEL_ADDR) \
		-DKERNEL_BYTES=$$(( ($$(stat -f %z $(B)/kernel.bin) + 511) / 512 * 512 )) \
		-o $@ $<

# Link at address 0. The boot block must be position independent, so fail the
# build if the linked object needed any absolute relocation.
$(B)/boot.elf: $(B)/boot.o
	$(CROSS)ld -Ttext=0 -e 0 --emit-relocs -o $@ $<
	@if $(OBJDUMP) -r $@ | grep -q R_68K_32; then \
		echo "error: boot block has absolute relocations (not position independent)"; \
		$(OBJDUMP) -r $@; exit 1; fi

$(B)/boot.bin: $(B)/boot.elf
	$(OBJCOPY) -O binary -j .text $< $@
	@test "$$(stat -f %z $@)" = 1024 || { echo "error: boot block is not 1024 bytes"; exit 1; }

# The same loader for starting from a SCSI disk (the BlueSCSI): the ROM loads
# it as that disk's "driver". The kernel sits at block KERNEL_BLOCK.
KERNEL_BLOCK := 8

$(B)/hdboot.o: boot/boot.S $(B)/banner.inc $(B)/kernel.bin
	$(CC) $(ASFLAGS) -c -I$(B) -DSCSI_BOOT -DKERNEL_BLOCK=$(KERNEL_BLOCK) -DKERNEL_ADDR=$(KERNEL_ADDR) \
		-DKERNEL_BYTES=$$(( ($$(stat -f %z $(B)/kernel.bin) + 511) / 512 * 512 )) \
		-o $@ $<

$(B)/hdboot.elf: $(B)/hdboot.o
	$(CROSS)ld -Ttext=0 -e 0 --emit-relocs -o $@ $<
	@if $(OBJDUMP) -r $@ | grep -q R_68K_32; then \
		echo "error: the SCSI loader has absolute relocations (not position independent)"; \
		$(OBJDUMP) -r $@; exit 1; fi

$(B)/hdboot.bin: $(B)/hdboot.elf
	$(OBJCOPY) -O binary -j .text $< $@

# The hard disk image to start from: on the BlueSCSI's card it is named
# "HD0_512 SSH OS boot.hda" (SCSI ID 0).
$(B)/sshos-boot.hda: $(B)/hdboot.bin $(B)/kernel.bin tools/mkbootdisk.py
	python3 tools/mkbootdisk.py $@ $(B)/hdboot.bin $(B)/kernel.bin $(KERNEL_BLOCK)

# Start from the hard disk image alone, with no floppy.
test-hdboot: $(B)/sshos-boot.hda
	"$(JUANRUN)" --rom "$(ROM)" --hdd 0:$(B)/sshos-boot.hda \
		--until SSHOS-BOOT-OK --type 'scsi\nuptime\n\s' \
		--done 'stray interrupts' --out $(B)/test-hdboot
	@echo "--- serial output ---"; cat $(B)/test-hdboot.serial.txt
	@grep -q 'stray interrupts' $(B)/test-hdboot.serial.txt
	@echo "test-hdboot passed"

# ----------------------------------------------------------- disk images --
# Raw floppy images: boot block, then the kernel, then zeros up to the exact
# size a real disk (and the floppy emulator) expects.
$(B)/sshos-800k.dsk: $(B)/boot.bin $(B)/kernel.bin
	dd if=/dev/zero of=$@ bs=1024 count=800 2>/dev/null
	cat $(B)/boot.bin $(B)/kernel.bin | dd of=$@ conv=notrunc 2>/dev/null

$(B)/sshos-1440k.dsk: $(B)/boot.bin $(B)/kernel.bin
	dd if=/dev/zero of=$@ bs=1024 count=1440 2>/dev/null
	cat $(B)/boot.bin $(B)/kernel.bin | dd of=$@ conv=notrunc 2>/dev/null

# ------------------------------------------------------------------ run --
# Boots, waits for the kernel's ready marker, types three lines on the
# emulated keyboard (one with accented letters, typed with Option as on a
# Mac), and passes when the last one has answered.
test: $(B)/sshos-800k.dsk
	"$(JUANRUN)" --rom "$(ROM)" --floppy $(B)/sshos-800k.dsk \
		--until SSHOS-BOOT-OK --type 'Hello, World! (1+2)*3\n\o?Qu\oee tal, se\onnor? \onNand\oeu \oc\o1 \ou\ou \oeb\n\s\puptime\n' \
		--done 'stray interrupts' --out $(B)/test
	@echo "--- serial output ---"; cat $(B)/test.serial.txt
	@python3 tests/check_text.py $(B)/test.serial.txt '¿Qué tal, señor? Ñandú ç¡ ¨¨´b'

test-crash:
	$(MAKE) B=build-crash KFLAGS=-DTEST_CRASH build-crash/sshos-800k.dsk
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-crash/sshos-800k.dsk \
		--until SSHOS-PANIC --out build-crash/test
	@echo "--- serial output ---"; cat build-crash/test.serial.txt

# A 4 MB scratch hard disk for the SCSI tests (tools/mkscratch.py).
$(B)/scratch.img: tools/mkscratch.py | $(B)
	python3 tools/mkscratch.py $@

test-scsi: $(B)/sshos-800k.dsk $(B)/scratch.img
	"$(JUANRUN)" --rom "$(ROM)" --floppy $(B)/sshos-800k.dsk --hdd 0:$(B)/scratch.img \
		--until SSHOS-BOOT-OK --type 'scsi\nblock 0 5\nwtest 0\nspeed 0\nscsi slow\nblock 0 8191\nwtest 0\nspeed 0\nuptime\n' \
		--done 'stray interrupts' --out $(B)/test-scsi
	@echo "--- serial output ---"; cat $(B)/test-scsi.serial.txt
	@grep -q 'ock 00000005' $(B)/test-scsi.serial.txt
	@grep -q 'ock 00008191' $(B)/test-scsi.serial.txt
	@test "$$(grep -c 'contents match' $(B)/test-scsi.serial.txt)" = 2
	@grep -q 'per second (fast reads)' $(B)/test-scsi.serial.txt
	@grep -q 'per second (slow reads)' $(B)/test-scsi.serial.txt
	@if grep -q 'Failed' $(B)/test-scsi.serial.txt; then echo "a SCSI command failed"; exit 1; fi
	@echo "SCSI test passed."

# The adapter sits at SCSI ID 4. Snow's gateway is 10.0.0.1 and has no DHCP
# server, so the address is set by hand.
test-net: $(B)/sshos-800k.dsk
	"$(JUANRUN)" --rom "$(ROM)" --floppy $(B)/sshos-800k.dsk --net 4 \
		--pcap $(B)/test-net.pcap --until SSHOS-BOOT-OK \
		--type 'net static 10.0.0.2 255.0.0.0 10.0.0.1\nping 10.0.0.1\nping 10.0.0.77\nnet\nuptime\n' \
		--done 'stray interrupts' --out $(B)/test-net
	@echo "--- serial output ---"; cat $(B)/test-net.serial.txt
	@grep -q '4 of 4 answered' $(B)/test-net.serial.txt
	@grep -q 'nobody on the local network' $(B)/test-net.serial.txt
	@grep -q 'damaged 0, ' $(B)/test-net.serial.txt
	@echo "Network test passed."

# TCP through Snow's gateway to servers on this computer
# (tools/testservers.py, started here and stopped when the test ends).
test-tcp: $(B)/sshos-800k.dsk
	@python3 tools/testservers.py & pid=$$!; trap "kill $$pid 2>/dev/null" EXIT; sleep 1; \
	"$(JUANRUN)" --rom "$(ROM)" --floppy $(B)/sshos-800k.dsk --net 4 \
		--pcap $(B)/test-tcp.pcap --until SSHOS-BOOT-OK --cycles 800000000 \
		--type 'net static 10.0.0.2 255.0.0.0 10.0.0.1\ntcptest 127.0.0.1 7001 down\ntcptest 127.0.0.1 7002 echo\nuptime\n' \
		--done 'stray interrupts' --out $(B)/test-tcp
	@echo "--- serial output ---"; sed -n '/^> net static/,$$p' $(B)/test-tcp.serial.txt
	@grep -q 'Received 262144 bytes.*Every byte correct' $(B)/test-tcp.serial.txt
	@grep -q 'Echoed 32768 bytes.*Every byte correct' $(B)/test-tcp.serial.txt
	@echo "TCP test passed."

# The second terminal (tools/model/vt.py), which the one in the kernel is
# checked against: its own tests.
test-vt-model:
	python3 -m unittest tests.test_vt

# The terminal (kernel/vt.c) compiled for this computer, checked against
# stated cases and against that separately written terminal.
test-vt-host: $(B)/font6x10.c | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-include tests/host_types.h -o $(B)/vt_host tests/vt_host.c kernel/vt.c $(B)/font6x10.c
	B=$(B) python3 -m unittest tests.test_vtc

# The same net.c, compiled for this computer with the address and
# undefined-behaviour checkers switched on.
test-net-host: | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -o $(B)/net_test tests/net_test.c
	$(B)/net_test

test-misc-host: | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -Ikernel -o $(B)/misc_test tests/misc_test.c
	python3 tests/misc_test.py $(B)/misc_test

# X25519 and ChaCha20 against plain Python arithmetic and the published values.
test-crypto-host: | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -Ikernel -o $(B)/crypto_test tests/crypto_test.c
	python3 tests/crypto_test.py $(B)/crypto_test

# The file system, built for this Mac, against tools/sshosdisk.py.
test-fs-host: | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -Ikernel -Itests -include tests/host_types.h \
		-o $(B)/fs_host tests/fs_host.c kernel/fs.c
	B=$(B) python3 -m unittest tests.test_fs

# How long the arithmetic of an SSH connection takes on the emulated SE.
bench: $(B)/sshos-800k.dsk
	"$(JUANRUN)" --rom "$(ROM)" --floppy $(B)/sshos-800k.dsk \
		--until SSHOS-BOOT-OK --cycles 3000000000 --type 'bench\n' \
		--done 'BENCH-DONE' --out $(B)/bench
	@sed -n '/^> bench/,$$p' $(B)/bench.serial.txt

# The SSH client, built for this Mac, against this Mac's own OpenSSH server
# (started without special rights on 127.0.0.1 with throw-away keys).
test-ssh-host: $(B)/font6x10.c | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -Ikernel -Itests -include tests/host_types.h \
		-o $(B)/ssh_host tests/ssh_host.c $(B)/font6x10.c \
		$(addprefix kernel/, sha256.c config.c x25519.c sha512.c poly1305.c ed25519.c ssh.c saved.c vt.c)
	python3 -m unittest tests.test_ssh

test-all: test test-hdboot test-crash test-scsi test-net test-tcp test-ssh test-disk test-desk test-wifi test-ownkey test-copro \
          test-net-host test-misc-host test-crypto-host test-ssh-host test-vt-host test-fs-host test-vt-model test-fw-host

# The BlueSCSI's side of the crypto hand-off (firmware/sshos_crypto.c), built
# for this computer and asked for the published answers.
test-fw-host: | $(B)
	cc -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
		-fno-sanitize-recover=undefined -include tests/host_types.h -Ikernel \
		-o $(B)/fw_host tests/fw_host.c firmware/sshos_crypto.c \
		kernel/x25519.c kernel/ed25519.c kernel/sha512.c
	$(B)/fw_host

run: $(B)/sshos-800k.dsk
	"$(SNOW_APP)" "$(ROM)" --floppy $(B)/sshos-800k.dsk

# SSH from the emulated SE into this Mac, in the Snow window. An OpenSSH
# server is started for it without special rights, on 127.0.0.1 only, with
# throw-away keys from tests/ssh_fixture.py: it gives your own shell to the
# one key built into the demo disk, and stops when Snow is closed. An SSH OS
# disk (build-demo/disk.img, SCSI ID 1) is made the first time and kept from
# then on, so what the SE saves is still there at the next demo. The disk
# goes straight into "ssh"; connecting takes the emulated SE about a minute.
demo:
	python3 tests/ssh_fixture.py build-demo 22025 shell
	$(MAKE) B=build-demo CONFIG=build-demo/config-ssh.txt build-demo/sshos-800k.dsk
	cp tools/demo.snoww build-demo/demo.snoww
	@test -f build-demo/disk.img || python3 tools/sshosdisk.py make build-demo/disk.img 16
	@: > build-demo/sshd.log; /usr/sbin/sshd -D -E "$(CURDIR)/build-demo/sshd.log" \
		-f "$(CURDIR)/build-demo/sshd/sshd_config" & server=$$!; \
	trap "kill $$server 2>/dev/null" EXIT; \
	"$(SNOW_APP)" build-demo/demo.snoww > build-demo/snow.log 2>&1

# The SSH client in the emulated SE, logging in to an OpenSSH server on this
# Mac: tests/ssh_fixture.py makes throw-away keys, the server's settings and
# the boot disk's settings; the server runs on 127.0.0.1 only for as long as
# the test does. The time the log-in took is in the serial output.
test-ssh:
	python3 tests/ssh_fixture.py build-ssh 22024
	$(MAKE) B=build-ssh CONFIG=build-ssh/config-ssh.txt build-ssh/sshos-800k.dsk
	@: > build-ssh/sshd.log; /usr/sbin/sshd -D -E "$(CURDIR)/build-ssh/sshd.log" \
		-f "$(CURDIR)/build-ssh/sshd/sshd_config" & pid=$$!; \
	trap "kill $$pid 2>/dev/null" EXIT; sleep 1; \
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-ssh/sshos-800k.dsk --net 4 \
		--until SSHOS-BOOT-OK --cycles 3500000000 --prompt '> ' --prompt 'ready>' --prompt 'sample.txt"' \
		--type 'ssh\necho result-$$((6*7))\necho term-$$TERM\nprintf "[%s]\\n" "\o?d\oeonde est\oea el ni\onno?"\ncat "$(CURDIR)/tests/charset-sample.txt"\n\p\s\pvi "$(CURDIR)/tests/charset-sample.txt"\n\p\pGofull screen\p\s'"$$(printf '\033')"':q!\necho after-vi\nexit 5\n\puptime\n' \
		--done 'stray interrupts' --out build-ssh/test-ssh
	@echo "--- serial output ---"; sed -n '/^> ssh/,$$p' build-ssh/test-ssh.serial.txt
	@grep -q '^result-42' build-ssh/test-ssh.serial.txt
	@grep -q '^term-xterm-256color' build-ssh/test-ssh.serial.txt
	@grep -q '^after-vi' build-ssh/test-ssh.serial.txt
	@python3 tests/check_text.py build-ssh/test-ssh.serial.txt '[¿dónde está el niño?]' \
		'╭───┬───╮  ┌───┬───┐  ┌───┬───┐' '│año│▓▒░│  │ a │ b │  │ c │ d │' \
		'╰───┴───╯  ↓ █▀▄▌▐ 50 °C ± 2 · «hola» — fin…'
	@grep -q 'Accepted publickey' build-ssh/sshd.log
	@grep -q 'The shell ended with status 5' build-ssh/test-ssh.serial.txt
	@grep 'Logged in after' build-ssh/test-ssh.serial.txt
	@echo "SSH test passed."

# The home screen: a disk with no "start" setting must come up in it. The
# keys and mouse moves from tests/desk_keys.py open the SSH OS menu, the
# About window, the network panel and the hardware test, and leave through
# the menu's OSConsole item; screenshots are saved on the way
# (build-desk/test-1.png and so on).
test-desk:
	$(MAKE) B=build-desk CONFIG=tests/config-desk.txt build-desk/sshos-800k.dsk
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-desk/sshos-800k.dsk --net 4 \
		--until 'DESK: home' --cycles 1300000000 --prompt '> ' --prompt 'DESK: settings' \
		--type "$$(python3 tests/desk_keys.py)" \
		--done 'stray interrupts' --out build-desk/test
	@grep -a 'DESK:' build-desk/test.serial.txt
	@python3 tests/desk_check.py build-desk/test.serial.txt

# The network panel's list and joining, which the emulator's adapter cannot
# do: this builds SSH OS with a made-up WiFi adapter (-DWIFI_FAKE, see
# kernel/wifi.c) and joins a network by clicking it.
test-wifi:
	$(MAKE) B=build-wifi KFLAGS=-DWIFI_FAKE CONFIG=tests/config-desk.txt build-wifi/sshos-800k.dsk
	python3 tools/sshosdisk.py make build-wifi/disk.img 4
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-wifi/sshos-800k.dsk --net 4 --hdd 2:build-wifi/disk.img \
		--until 'DESK: home' --cycles 900000000 --type "$$(python3 tests/wifi_keys.py)" \
		--done 'stray interrupts' --out build-wifi/test
	@grep -a 'DESK:' build-wifi/test.serial.txt
	@grep -a -q 'DESK: joining' build-wifi/test.serial.txt
	@grep -a -q 'DESK: joined' build-wifi/test.serial.txt
	@python3 tools/sshosdisk.py get build-wifi/disk.img SETTINGS.TXT | grep -q '^wifi = Guest'
	@# A restart: the made-up adapter is on its first network again, and
	@# SSH OS must take it back to the one joined above.
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-wifi/sshos-800k.dsk --net 4 --hdd 2:build-wifi/disk.img \
		--until 'DESK: home' --cycles 900000000 --type '\p\p\p\p\s\pj\p\v\v\v\nuptime\n' \
		--done 'stray interrupts' --out build-wifi/again
	@grep -a 'DESK:\|DISK:' build-wifi/again.serial.txt
	@grep -a -q 'DESK: back on the saved network' build-wifi/again.serial.txt
	@python3 tools/sshosdisk.py check build-wifi/disk.img
	@echo "WiFi panel test passed."

# The SSH OS disk (kernel/fs.c), in two starts of the emulated SE with the
# same disk image at SCSI ID 1, and an OpenSSH server as in test-ssh whose
# key the settings do not know.
#   First start: the image is blank; "format" makes it an SSH OS disk, a
#   setting is changed, and an SSH connection is made, which asks whether to
#   trust the server (Return in its window says yes). Then one as a user
#   with no key, to see the password window; Esc leaves it.
#   Second start: the setting is still changed, the home screen's SSH icon
#   lists the earlier connection, and connecting asks nothing.
# In between and afterwards tools/sshosdisk.py, which shares no code with the
# kernel, checks the volume and reads the files.
# Handing SSH's slow sums to the BlueSCSI (kernel/copro.c). No emulator has
# a BlueSCSI with our firmware addition, so SSH OS is built with
# -DCOPRO_FAKE: the firmware's side (firmware/sshos_crypto.c) is linked in
# and answers in its place. A log-in then goes through the same requests
# and answers as with the real thing; the server accepting it shows the
# sums came back right. All four must have gone that way.
test-copro:
	python3 tests/ssh_fixture.py build-copro 22028
	$(MAKE) B=build-copro KFLAGS=-DCOPRO_FAKE CONFIG=build-copro/config-ssh.txt build-copro/sshos-800k.dsk
	@: > build-copro/sshd.log; /usr/sbin/sshd -D -E "$(CURDIR)/build-copro/sshd.log" \
		-f "$(CURDIR)/build-copro/sshd/sshd_config" & pid=$$!; \
	trap "kill $$pid 2>/dev/null" EXIT; sleep 1; \
	"$(JUANRUN)" --rom "$(ROM)" --floppy build-copro/sshos-800k.dsk --net 4 \
		--until SSHOS-BOOT-OK --cycles 3500000000 --prompt '> ' --prompt 'ready>' \
		--type 'ssh\necho result-$$((6*7))\nexit\n\puptime\n' \
		--done 'stray interrupts' --out build-copro/test
	@sed -n '/^> ssh/,$$p' build-copro/test.serial.txt
	@grep -q 'COPRO: the BlueSCSI does the sums' build-copro/test.serial.txt
	@test "$$(grep -c 'COPRO: a sum done by the BlueSCSI' build-copro/test.serial.txt)" = 4
	@grep -q '^result-42' build-copro/test.serial.txt
	@grep -q 'Accepted publickey' build-copro/sshd.log
	@echo "Hand-off test passed."

# The key SSH OS makes for itself. First start: log in (with the test's key,
# and the setting ssh_offer_key so that the question comes as it would after
# a password), say yes: the SE makes its key, keeps it on its disk and adds
# it to the account. Second start, from settings with no key in them: it
# logs in with its own. The server keeps logins in a home folder of their
# own (build-own/home), so nothing of yours is touched.
OWN_RUN = "$(JUANRUN)" --rom "$(ROM)" --net 4 --hdd 1:build-own/disk.img --until SSHOS-BOOT-OK \
	--cycles 3000000000 --prompt '> ' --prompt 'ready>' --prompt 'yes: ' --prompt 'SSH: ' \
	--done 'stray interrupts'
test-ownkey:
	python3 tests/ssh_fixture.py build-own 22027 ownkey
	$(MAKE) B=build-own CONFIG=build-own/config-ssh.txt build-own/sshos-800k.dsk
	$(MAKE) B=build-own2 CONFIG=build-own/config-nokey.txt build-own2/sshos-800k.dsk
	python3 tools/sshosdisk.py make build-own/disk.img 4 --blank
	@: > build-own/sshd.log; /usr/sbin/sshd -D -E "$(CURDIR)/build-own/sshd.log" \
		-f "$(CURDIR)/build-own/sshd/sshd_config" & pid=$$!; \
	trap "kill $$pid 2>/dev/null" EXIT; sleep 1; \
	$(OWN_RUN) --floppy build-own/sshos-800k.dsk --out build-own/first \
		--type 'format 1\nyes\nssh\n\p\s\n\pecho one-$$((6*7))\nexit\n\puptime\n' && \
	$(OWN_RUN) --floppy build-own2/sshos-800k.dsk --out build-own/second \
		--type 'ssh\necho two-$$((7*7))\nexit\n\puptime\n'
	@echo "--- first start ---"; sed -n '/^> ssh/,$$p' build-own/first.serial.txt
	@echo "--- second start ---"; sed -n '/^> ssh/,$$p' build-own/second.serial.txt
	@grep -q 'SSH: no password next time' build-own/first.serial.txt
	@grep -q 'Done: no password next time' build-own/first.serial.txt
	@grep -q '^one-42' build-own/first.serial.txt
	@grep -q '^two-49' build-own/second.serial.txt
	@if grep -q 'SSH: ' build-own/second.serial.txt; then echo "the second start asked something"; exit 1; fi
	@grep -c 'SSH-OS$$' build-own/home/.ssh/authorized_keys | grep -qx 1
	@test "$$(grep -c 'Accepted publickey' build-own/sshd.log)" = 2
	@python3 tools/sshosdisk.py ls build-own/disk.img | grep -q 'KEY.TXT'
	@echo "Own key test passed."

DISK_RUN = "$(JUANRUN)" --rom "$(ROM)" --floppy build-disk/sshos-800k.dsk --net 4 \
	--hdd 1:build-disk/disk.img --until SSHOS-BOOT-OK --cycles 3000000000 \
	--prompt '> ' --prompt 'ready>' --prompt 'yes: ' --prompt 'SSH: ' --prompt 'DESK: ' --prompt 'home screen.' \
	--done 'stray interrupts'
test-disk:
	python3 tests/ssh_fixture.py build-disk 22026 ask
	$(MAKE) B=build-disk CONFIG=build-disk/config-ssh.txt build-disk/sshos-800k.dsk
	python3 tools/sshosdisk.py make build-disk/disk.img 4 --blank
	@: > build-disk/sshd.log; /usr/sbin/sshd -D -E "$(CURDIR)/build-disk/sshd.log" \
		-f "$(CURDIR)/build-disk/sshd/sshd_config" & pid=$$!; \
	trap "kill $$pid 2>/dev/null" EXIT; sleep 1; \
	$(DISK_RUN) --out build-disk/first \
		--type 'disk\nformat 1\nyes\nset volume 3\nssh\n\p\s\necho result-$$((6*7))\nexit\n\pssh guest@127.0.0.1:22026\n\pabc\p\s'"$$(printf '\033')"'\pls\nuptime\n' && \
	python3 tools/sshosdisk.py check build-disk/disk.img && \
	$(DISK_RUN) --out build-disk/second \
		--type 'set\ndesk\n\p\p\n\p\s\n\p\s\n\pecho again-$$((7*7))\nexit\n\p \p\s\p\n\p\v\n\pa@10.9.9.9\n\n\p\s \p\s'"$$(printf '\033')"'\pj\p\v\v\v\nls\nuptime\n'
	@grep -q 'DESK: connecting' build-disk/second.serial.txt
	@grep -q 'SSH: message' build-disk/second.serial.txt
	@echo "--- first start ---"; sed -n '/^> disk/,$$p' build-disk/first.serial.txt
	@echo "--- second start ---"; sed -n '/^> set/,$$p' build-disk/second.serial.txt | grep -a -v '^  ssh_key'
	@grep -q 'There is no SSH OS disk' build-disk/first.serial.txt
	@grep -q 'SSH OS disk at SCSI ID 1' build-disk/first.serial.txt
	@grep -q 'Remembered on the SSH OS disk' build-disk/first.serial.txt
	@grep -q '^result-42' build-disk/first.serial.txt
	@grep -q 'DISK: SSH OS disk found' build-disk/second.serial.txt
	@grep -q '^  volume = 3' build-disk/second.serial.txt
	@grep -q 'DESK: connections' build-disk/second.serial.txt
	@grep -q '^again-49' build-disk/second.serial.txt
	@grep -q 'SSH: new server' build-disk/first.serial.txt
	@grep -q 'SSH: password' build-disk/first.serial.txt
	@grep -q 'stopped at the password' build-disk/first.serial.txt
	@if grep -q 'SSH: new server' build-disk/second.serial.txt; then echo "asked about a known server"; exit 1; fi
	@test "$$(grep -c 'Accepted publickey' build-disk/sshd.log)" = 2
	python3 tools/sshosdisk.py check build-disk/disk.img
	python3 tools/sshosdisk.py ls build-disk/disk.img
	@python3 tools/sshosdisk.py get build-disk/disk.img SETTINGS.TXT | grep -q '^volume = 3'
	@python3 tools/sshosdisk.py get build-disk/disk.img HOSTS.TXT | grep -q '^127.0.0.1 SHA256:'
	@python3 tools/sshosdisk.py get build-disk/disk.img RECENT.TXT | grep -q '@127.0.0.1:22026$$'
	@echo "SSH OS disk test passed."

# What to hand to someone who wants to run SSH OS: dist/SSH-OS-VERSION.zip
# with the three files for the BlueSCSI's card and the guide
# (docs/INSTALL.txt). Built from config.release.txt, never from your own
# settings, so nothing of yours is in it.
release:
	$(MAKE) B=build-release CONFIG=config.release.txt build-release/sshos-boot.hda
	python3 tools/mkrelease.py build-release/sshos-boot.hda dist

tools:
	mkdir -p "$(SNOW_SRC)/testrunner/src/bin/juanrun"
	cp tools/snowrun/main.rs "$(SNOW_SRC)/testrunner/src/bin/juanrun/main.rs"
	cd "$(SNOW_SRC)" && PATH="/opt/homebrew/opt/rustup/bin:$$PATH" \
		CARGO_NET_GIT_FETCH_WITH_CLI=true cargo build -r -p testrunner --bin juanrun \
		--features snow_core/ethernet_nat,snow_core/mmap

disasm: $(B)/kernel.elf
	$(OBJDUMP) -d $<

clean:
	rm -rf build build-crash build-auto build-demo

.PHONY: all test test-crash test-scsi test-net test-tcp test-vt-model demo test-ssh test-desk \
        test-net-host test-misc-host test-crypto-host test-ssh-host bench test-all run tools \
        test-disk test-fs-host test-wifi test-ownkey test-hdboot test-vt-host release \
        test-copro test-fw-host \
        disasm clean
