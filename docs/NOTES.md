# Notes: speed, limits, layout

How fast it is, what is not done, and where things are in the source. Back
to the [README](../README.md).

## How long connecting takes

The 68000 does all of SSH's arithmetic itself, in plain C. `make bench`
times the pieces on the emulated SE:

| | |
|---|---|
| Key exchange (X25519), two of these | 12.4 seconds each |
| Checking the server's signature (Ed25519) | 20.7 seconds |
| Signing with the SE's key (Ed25519) | 16.0 seconds |
| ChaCha20 | 18.8 KB per second |
| Poly1305 (its multiplication in assembly) | 12.3 KB per second |

So logging in with a key takes 63 seconds (measured by `make test-ssh`), with
a password about 46, and the first of those steps is done before the server
is contacted. Afterwards output arrives at up to about 7 KB per second, since
every byte passes through both ChaCha20 and Poly1305; typing feels immediate.
Most servers close a connection that has not logged in within two minutes,
so there is room, but not much.

The terminal is quick enough not to hold that up: `termbench` in the
OSConsole times it at 6 KB per second for lines that scroll and 5 to 8 for
text drawn in place. It draws four characters at a time, which are exactly
three bytes of the screen, and only once all the bytes in hand have been
read, so a cell written several times is drawn once and a page that scrolls
by moves the screen once.

The plan is to hand these calculations to the BlueSCSI's own processor,
which does each in a few thousandths of a second; connecting would then take
a second or two. The SE's side is ready for that: the four slow calculations
are four function calls. The BlueSCSI's side, a change to its firmware, is
not written.

## Other machines

**Macintosh SE/30: not as it is.** The kernel's 68000 code would run on the
SE/30's 68030, and the loader and the screen (same size, found through the
ROM) would work, but the chips sit at different addresses and some behave
differently: the VIAs, the serial chip, the SCSI chip's fast transfer mode,
sound, and the frames the processor leaves on the stack when something goes
wrong. It is a port of the lowest layer (`kernel/hw.h`, `scsi.c`, `via.c`,
`trap.c`, `snd.c`), not a rewrite, and has not been started. An SE/30 would
do the SSH arithmetic several times faster.

**Macintosh Plus and Classic:** not tried. The Plus has no ADB, so its
keyboard and mouse would need their own driver.

## Not done

- The BlueSCSI doing SSH's slow sums and its packets, which would take
  connecting from a minute to a few seconds and lift the 7 KB a second of
  a session: both sides are written and tested against each
  other on this side of the cable ([firmware/](../firmware/README.md)), and
  the firmware builds with it, but it has not run on a real BlueSCSI yet.
- SSH:
  - Connecting takes about a minute (see above).
  - Random numbers, which SSH's safety rests on, are gathered from the
    timing of keys, mouse and network packets (`kernel/rng.c`). That is
    weaker than a modern computer's source, most of all right after start-up.
    The BlueSCSI's processor has a real source and should supply them.
  - The question-and-answer log-in some servers use instead of plain
    passwords is not there.
  - One choice of each method (see [SSH](USING.md#ssh)); no RSA or ECDSA server
    keys, no compression, no port forwarding, no file copy.
  - While the server changes keys in mid-session (OpenSSH: after 1 GB or
    when set to) the screen stands still for about 45 seconds.
  - The terminal has no colours, no mouse reporting and a fixed size; wide
    characters (Chinese, Japanese, Korean, emoji) show as `?`.
  - Ping is a connection to the SSH port that is dropped at once;
    a careful server may log it. Its time is measured in sixtieths of a
    second, so small figures are rough.
- SSH OS disk:
  - One folder, short names, whole files; nothing bigger than settings uses
    it yet. The WiFi password is stored as plain text.
  - A power cut while writing can leave space marked as used that no file
    has (`tools/sshosdisk.py check` reports it; a disk checker frees it).
- Home screen:
  - The WiFi name, signal, list and joining use commands that the BlueSCSI
    firmware adds to its network adapter (`kernel/wifi.c`, written from the
    firmware's source). Snow's adapter has none of them, so in the emulator
    they only run against a made-up adapter (`make test-wifi`).
  - Sound is a beep (SSH's bell, the volume control) and one tune: the
    hardware test plays "La cucaracha" in square waves.
  - The clock cannot be set.
  - Shut down cannot switch the SE off (it has a mechanical switch); it stops
    the network and says the Mac can be switched off.
  - Restart has not been tried: the test does not cover it.
- The Snow emulator ignores the processor's sleep instruction, so in it SSH OS
  never really idles; the load figure counts only real work, which reads the
  same on both.
- DHCP leases are not renewed.
- Sending is slow (about 12 KB per second); receiving is about 40.
- US keyboard layout only (accents through Option, see [typing accented letters](USING.md#typing-accented-letters)); nothing shows
  that an accent is waiting for its letter. No floppy driver.

## Where things are

```
boot/boot.S          the loader: floppy boot block, or SCSI "driver" (SCSI_BOOT)
kernel/              the kernel and its programs
  ssh.c vt.c           the SSH client and its terminal
  x25519.c ed25519.c sha512.c sha256.c poly1305.c rng.c   SSH's arithmetic
  net.c tcp.c dayna.c wifi.c   the network, down to the BlueSCSI's adapter
  scsi.c fs.c saved.c config.c   disk, file system, what is remembered
  desk.c bar.c barmenu.c settings.c splash.c ask.c connect.c   what you see
  adb.c kbd.c via.c timer.c snd.c trap.c   the machine
fonts/               the 6 x 10 font and the icons, as text art
tools/mkbootdisk.py  lays out the hard disk image the SE starts from
tools/sshosdisk.py   makes, lists, reads, writes and checks an SSH OS disk
tools/sshkey.py      turns an OpenSSH key into the ssh_key setting
tools/snowrun/       headless test runner built on Snow
tests/               tests, in the emulator and on this computer
```

