# The BlueSCSI doing SSH's sums

An SSH log-in needs four big calculations. The SE's 68000 takes 12 to 20
seconds over each, which is why connecting takes about a minute. The
BlueSCSI's processor does one in a few hundredths of a second. This folder
is an addition to the BlueSCSI firmware that lets SSH OS hand them over.

Once logged in, every packet is encrypted and carries a check sum, and the
68000 manages about 7 KB of that a second. The same addition opens and
seals packets too. SSH OS sends it the ones of 256 bytes and more; a key
press is quicker done at home than sent down the cable and back.

**Status: it runs on a real SE with a BlueSCSI v2 and a Pico W**, and
connecting is far quicker with it; how much has not been timed yet. Each
release of SSH OS holds the firmware ready built, and the source is a
branch of BlueSCSI's: <https://github.com/naruse/BlueSCSI-v2/tree/sshos-crypto>.
What the tests check: the code on the firmware's side gives the published
answers when built for an ordinary computer and agrees to the byte with
what SSH OS does itself (`make test-fw-host`); SSH OS logs in to a real
server and holds a session there, every sum and every packet going through
the same requests and answers with that code standing in for the BlueSCSI
(`make test-copro`).

SSH OS asks whether the BlueSCSI can do this each time it starts, and does
the sums itself if not. So the same SSH OS runs with the firmware as it
comes and with this one.

## How it talks

A SCSI command of our own, `0x1D`, on the network adapter (the one the
firmware's own WiFi commands, `0x1C`, already use). The request is the
second byte:

| Request | SSH OS writes | then reads back with `10` |
|---|---|---|
| `00` hello | nothing; reads 8 bytes: `SSHOS`, version, 0, 0 | |
| `01` X25519 | scalar (32), point (32) | 1, shared point (32) |
| `02` public key | seed (32) | 1, Ed25519 public key (32) |
| `03` sign | seed (32), public key (32), message | 1, signature (64) |
| `04` verify | signature (64), public key (32), message | 1, then 1 = genuine |
| `05` open a packet | its number (4), the key (64), the packet as it arrived | 1, then 1 = genuine, then its body in clear |
| `06` seal a packet | its number (4), the key (64), the packet in clear | 1, the packet encrypted, its check sum (16) |

The first byte read back is 1 if the request was done and 0 if it was
refused. An answer is handed out once and then wiped.

The packets are those of `chacha20-poly1305@openssh.com`. Nothing is
remembered between requests: the key travels with each packet.

The arithmetic is SSH OS's own (`kernel/x25519.c`, `ed25519.c`, `sha512.c`,
`poly1305.c`): the same files the SE runs.

## Building it into the firmware

```bash
git clone https://github.com/BlueSCSI/BlueSCSI-v2.git
```

```bash
python3 firmware/prepare.py BlueSCSI-v2
```

Then build as the BlueSCSI's own `BUILDING.md` says (target
`Pico_DaynaPORT` for a BlueSCSI v2 with a Pico W), and flash the `.uf2` over
USB. The branch above is this, done. `prepare.py` copies the files into `lib/SSHOS` and applies
`bluescsi.patch`, made against BlueSCSI-v2 commit `5e64e3b`.

The firmware has about 9 KB of memory free and keeps most of its code in
RAM, so the addition takes none of either: it works in the buffer the
request arrived in, borrows the far half of that buffer for the signature
sums' tables, and is linked into flash. What it does use is stack, a few
kilobytes at the deepest.

The private key and the one-time secrets cross the SCSI cable to the
BlueSCSI in the clear. Both ends are in the same machine.
