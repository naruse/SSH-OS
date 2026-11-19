# The BlueSCSI doing SSH's sums

An SSH log-in needs four big calculations. The SE's 68000 takes 12 to 20
seconds over each, which is why connecting takes about a minute. The
BlueSCSI's processor does one in a few hundredths of a second. This folder
is an addition to the BlueSCSI firmware that lets SSH OS hand them over.

**Status: not yet run on a real BlueSCSI.** What is checked: the code on
the firmware's side gives the published answers when built for an ordinary
computer (`make test-fw-host`), and SSH OS logs in to a real server through
the same requests and answers with that code standing in for the BlueSCSI
(`make test-copro`). Not checked: that it builds into the firmware, and
that it works over the cable.

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

The first byte read back is 1 if the request was done and 0 if it was
refused. An answer is handed out once and then wiped.

The arithmetic is SSH OS's own (`kernel/x25519.c`, `ed25519.c`,
`sha512.c`): the same files the SE runs.

## Building it into the firmware

```bash
git clone https://github.com/BlueSCSI/BlueSCSI-v2.git
```

```bash
python3 firmware/prepare.py BlueSCSI-v2
```

Then build as the BlueSCSI's own `BUILDING.md` says, and flash the result
over USB. `prepare.py` copies the files into `lib/SSHOS` and applies
`bluescsi.patch`, made against BlueSCSI-v2 commit `5e64e3b`.

The private key and the one-time secrets cross the SCSI cable to the
BlueSCSI in the clear. Both ends are in the same machine.
