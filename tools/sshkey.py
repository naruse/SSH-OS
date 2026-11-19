#!/usr/bin/env python3
"""Turn an OpenSSH Ed25519 private key into the "ssh_key" line for config.txt.

    ssh-keygen -t ed25519 -N '' -C 'Macintosh SE' -f se_key
    python3 tools/sshkey.py se_key

The key file must have no passphrase: the SE keeps the key as it is, in the
settings built into its boot disk. Make a key just for the SE, and treat the
boot disk and config.txt like the key itself. The matching se_key.pub line
goes into ~/.ssh/authorized_keys on each server.
"""
import base64
import struct
import sys


def fields(data):
    """Split a run of length-prefixed strings."""
    out = []
    while len(data) >= 4:
        (n,) = struct.unpack(">L", data[:4])
        if n > len(data) - 4:
            break
        out.append(data[4:4 + n])
        data = data[4 + n:]
    return out


def seed_and_public(path):
    lines = open(path).read().split("\n")
    if "-----BEGIN OPENSSH PRIVATE KEY-----" not in lines:
        sys.exit("%s is not an OpenSSH private key file." % path)
    body = "".join(l for l in lines if l and not l.startswith("-----"))
    raw = base64.b64decode(body)
    magic = b"openssh-key-v1\0"
    if not raw.startswith(magic):
        sys.exit("%s is not an OpenSSH private key file." % path)
    cipher, kdf, _options = fields(raw[len(magic):])[:3]
    if cipher != b"none" or kdf != b"none":
        sys.exit("This key has a passphrase. Make one without (ssh-keygen -N ''): "
                 "the SE has no way to ask for it.")
    rest = raw[len(magic) + sum(4 + len(f) for f in (cipher, kdf, _options)) + 4:]
    _public, private = fields(rest)[:2]
    kind, public, secret = fields(private[8:])[:3]
    if kind != b"ssh-ed25519" or len(secret) != 64 or secret[32:] != public:
        sys.exit("Only Ed25519 keys work (ssh-keygen -t ed25519); this one is %s."
                 % kind.decode("ascii", "replace"))
    return secret


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    print("ssh_key = " + base64.b64encode(seed_and_public(sys.argv[1])).decode())


if __name__ == "__main__":
    main()
