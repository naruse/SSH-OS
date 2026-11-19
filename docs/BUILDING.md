# Building and testing SSH OS

For a ready-made copy see "Install" in the [README](../README.md); this is
for building it yourself, running it in the emulator and running the tests.

## What you need

To run it:

- A Macintosh SE. It is developed on an SE FDHD with 4 MB.
- A [BlueSCSI v2](https://bluescsi.com) with the Pico W (the WiFi model) and
  an SD card. The BlueSCSI is both the disk SSH OS starts from and its
  network adapter.
- An SSH server to log in to: any OpenSSH from 2014 or later.

To build it (macOS or Linux):

- `m68k-elf-gcc` and `m68k-elf-binutils`. With Homebrew:

  ```bash
  brew install m68k-elf-binutils m68k-elf-gcc
  ```

- Python 3 and `make`.

Nothing else: no Apple software, no libraries. The ROM is not part of this
repository and is not needed to build; only the emulator needs one (see
"Emulator and tests").

## Build

```bash
make
```

That writes, in `build/`:

| File | What |
|---|---|
| `sshos-boot.hda` | The hard disk image the SE starts from on a BlueSCSI |
| `sshos-800k.dsk`, `sshos-1440k.dsk` | The same system as raw floppy images, for a floppy emulator or a real disk |

Your own settings are built in. Before `make`, copy `config.example.txt` to
`config.txt` and edit it; without one the example is used (DHCP, start at the
home screen), which is fine for a first try. `config.txt` can hold secrets
(an SSH key, a WiFi password): it is in `.gitignore`, and so are the images
built from it. Keep them private.

How it is put together: `boot/boot.S` is the loader, in assembly; everything
in `kernel/` is linked into one kernel image that the loader reads to address
`0x40000`; `tools/mkbootdisk.py` lays the loader and the kernel out on the
disk image behind the small map the ROM looks for. The font, the icons and
the big name are text art (`fonts/`, `tools/mkbanner.py`) turned into tables
at build time.

## Put it on the BlueSCSI's card

Format the SD card as exFAT (or FAT32) and copy three files into its top
folder:

| File on the card | What |
|---|---|
| `HD0_512 SSH OS boot.hda` | A copy of `build/sshos-boot.hda`: SSH OS itself, at SCSI ID 0 |
| `HD1_512 SSH OS.hda` | The SSH OS disk, where settings and connections are kept (SCSI ID 1). Make it once, see below |
| `NE4.hda` | An empty file: it switches on the BlueSCSI's network adapter at SCSI ID 4 |

```bash
cp build/sshos-boot.hda "/Volumes/CARD/HD0_512 SSH OS boot.hda"
```

```bash
python3 tools/sshosdisk.py make "/Volumes/CARD/HD1_512 SSH OS.hda" 16
```

```bash
touch /Volumes/CARD/NE4.hda
```

Only the part of each name before the first space matters to the BlueSCSI.
Put the card in, switch the SE on. After a new build, replace the `HD0` file
only: your settings live in the `HD1` file and stay as they are.

## Emulator and tests

Everything is developed in the [Snow](https://snowemu.com) emulator, which
runs the SE's real ROM. For that you need two things that are not in this
repository:

- A ROM image of your own Macintosh SE FDHD, in the project folder as
  `B306E171 - Mac SE FDHD.ROM` (or `make ROM=path ...`). It is Apple's code
  and may not be shared.
- Snow: the app in `emu/Snow.app` for `make run` and `make demo`, and its
  source in `~/.cache/juanos/snow-src` for the tests' headless runner, which
  `make tools` builds from `tools/snowrun/main.rs` (needs Rust).

```bash
make demo
```

opens Snow with the emulated SE at its home screen. The SSH icon then logs
in to the computer you are sitting at: for that the demo starts an OpenSSH
server (your computer's own `sshd`, without special rights) that listens on
that computer only and accepts one throw-away key built into the demo disk.
It stops when Snow closes.

```bash
make test-all
```

| Target | What it checks |
|---|---|
| `test`, `test-crash` | Boot, typing in the OSConsole (accents included), the crash screen |
| `test-disk` | The SSH OS disk in the emulated SE: format, save, restart, read back; SSH asks about a server once only |
| `test-ownkey` | The SE's own key: made, kept on its disk and added to an account on a real OpenSSH server, then used to log in with no key in the settings |
| `test-fs-host` | The file system on this computer against a second implementation: every size, a full disk, power cuts while writing, disks that are not ours |
| `test-scsi` | Bus scan, block read, write and read back, read speed |
| `test-net`, `test-tcp` | Ping; a 256 KB download and a 32 KB echo with every byte checked |
| `test-net-host` | The network code on this computer against a simulated network: DHCP, DNS, TCP with lost, damaged and reordered packets |
| `test-misc-host`, `test-vt-model` | SHA-256, settings parser, every Option combination of the keyboard, the WiFi commands against a stand-in, the second terminal the kernel's is checked against |
| `test-desk` | The home screen in the emulator: SSH OS menu, About, network panel, hardware test and back, by keys and mouse |
| `test-wifi` | The network panel's list and joining, with a made-up WiFi adapter built in |
| `test-crypto-host` | X25519, ChaCha20, SHA-512, Poly1305 and Ed25519 against plain Python arithmetic and the published values |
| `test-ssh-host` | The SSH client, built for this computer, against its own OpenSSH server: key and password log-in, unknown and changed server keys, 600 KB of output across several changes of keys, forged and damaged packets |
| `test-ssh` | The same client in the emulated SE: log-in with a key, a shell, accents typed and shown |

| `test-hdboot` | Starting from the hard disk image alone, as on the BlueSCSI |
| `test-vt-host` | The terminal, built for this computer: stated cases, and 300 random streams of orders compared with a second, separately written terminal |

### Tests on GitHub

`.github/workflows/tests.yml` runs the tests on every push. The ones built
for an ordinary computer (network, ciphers, SSH, terminal, file system) need
nothing else and also run for pull requests. The ones in the emulated SE
need Apple's ROM, which is not in this repository and may not be: they take
it from a private repository of yours.

1. Make a private repository on GitHub and put your ROM image in it (any
   name ending in `.ROM`).
2. Give it a read-only deploy key and hand the private half to this
   repository as the secret `ROM_REPO_KEY`:

   ```bash
   ssh-keygen -t ed25519 -N "" -f romkey
   gh repo deploy-key add romkey.pub --repo you/its-name --title "SSH OS tests"
   gh secret set ROM_REPO_KEY --repo you/SSH-OS < romkey
   rm romkey romkey.pub
   ```

   A deploy key opens that one repository and nothing else of yours.
3. Name the private repository in the variable `ROM_REPO`:
   `gh variable set ROM_REPO --repo you/SSH-OS --body you/its-name`.

Without the variable the emulator job is skipped. Pull requests from other
people never get the ROM: GitHub does not hand them secrets.

