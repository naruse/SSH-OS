#!/usr/bin/env python3
"""Compare the kernel's X25519, ChaCha20, SHA-512, Poly1305 and Ed25519
(build/crypto_test) with plain Python arithmetic, on the published test
values and on many other inputs."""
import hashlib
import random
import struct
import subprocess
import sys

P = 2**255 - 19


def x25519(k, u):
    k = int.from_bytes(k, "little")
    k &= ~7
    k &= (1 << 255) - 1
    k |= 1 << 254
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3, swap = 1, 0, x1, 1, 0
    for t in range(254, -1, -1):
        bit = (k >> t) & 1
        if swap ^ bit:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = bit
        a, b = (x2 + z2) % P, (x2 - z2) % P
        aa, bb = a * a % P, b * b % P
        e = (aa - bb) % P
        c, d = (x3 + z3) % P, (x3 - z3) % P
        da, cb = d * a % P, c * b % P
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + 121665 * e) % P
    if swap:
        x2, z2 = x3, z3
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


def rol(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def chacha20_stream(key, nonce, counter, length):
    out = b""
    while len(out) < length:
        s = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
        s += struct.unpack("<8L", key) + (counter & 0xFFFFFFFF,) + struct.unpack("<3L", nonce)
        x = list(s)

        def quarter(a, b, c, d):
            x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rol(x[d] ^ x[a], 16)
            x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rol(x[b] ^ x[c], 12)
            x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rol(x[d] ^ x[a], 8)
            x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rol(x[b] ^ x[c], 7)

        for _ in range(10):
            quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15)
            quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14)
        out += struct.pack("<16L", *[(a + b) & 0xFFFFFFFF for a, b in zip(x, s)])
        counter += 1
    return out[:length]


H = bytes.fromhex

# Published values first, so that the Python above is itself checked.
# RFC 7748 section 5.2 (both tests) and section 6.1.
RFC_X = [
    ("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
     "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
     "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
    ("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
     "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
     "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
    ("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
     "09" + "00" * 31,
     "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"),
    ("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
     "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
     "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
]
for k, u, want in RFC_X:
    assert x25519(H(k), H(u)).hex() == want, "the Python X25519 disagrees with RFC 7748"

# RFC 7748 section 5.2: k = u = 9; repeat k, u = X25519(k, u), k.
k = u = H("09" + "00" * 31)
k, u = x25519(k, u), k
assert k.hex() == "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"
iterated = [(k, u)]
for _ in range(999):
    k, u = x25519(k, u), k
    iterated.append((k, u))
assert k.hex() == "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"

# RFC 8439 section 2.3.2: the key stream block for counter 1.
RFC_KEY = bytes(range(32))
RFC_NONCE = H("000000090000004a00000000")
assert chacha20_stream(RFC_KEY, RFC_NONCE, 1, 64).hex().startswith(
    "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"), \
    "the Python ChaCha20 disagrees with RFC 8439"

rng = random.Random(25519)
cases = []          # (input line, wanted answer)


def add_x(k, u):
    cases.append(("x %s %s" % (k.hex(), u.hex()), x25519(k, u).hex()))


for k, u, _ in RFC_X:
    add_x(H(k), H(u))
for i in range(0, 1000, 37):                    # a sample of the iterated test
    add_x(*iterated[i])

# Points at the edges of the field: the carries and wrap-arounds.
edges = [0, 1, 2, 9, 18, 19, 20, 37, 38, 39, 2**16 - 1, 2**16, 2**128 - 1, 2**128,
         2**255 - 21, 2**255 - 20, P, P + 1, P + 18, 2**255 - 1, 2**255, 2**255 + 18,
         2**256 - 39, 2**256 - 38, 2**256 - 1, (2**256 - 1) // 3, (2**256 - 1) // 0xFFFF,
         0xFFFF * sum(1 << (32 * i) for i in range(8)),
         0xFFFF0000 * sum(1 << (32 * i) for i in range(8))]
for e in edges:
    for k in (H("ff" * 32), H("00" * 32), bytes(rng.randrange(256) for _ in range(32))):
        add_x(k, e.to_bytes(32, "little"))
for _ in range(300):
    add_x(bytes(rng.randrange(256) for _ in range(32)), bytes(rng.randrange(256) for _ in range(32)))
# Limbs that are all ones or all zeros, in random mixtures.
for _ in range(200):
    u = b"".join(rng.choice((b"\xff\xff", b"\x00\x00", b"\x01\x00", b"\xfe\xff")) for _ in range(16))
    add_x(bytes(rng.randrange(256) for _ in range(32)), u)

# The field operations one at a time, on raw 256-bit values. The wrap-around
# paths (a carry out of 2^256, then another) are only reachable like this.
values = sorted(set(edges + [2**256 - 1 - e for e in edges] + [2**256 - 38 * n for n in range(1, 41)]
                    + [38 * n for n in range(1, 41)]))
values += [rng.randrange(2**256) for _ in range(20)]


def add_f(op, a, b, want):
    cases.append(("f %s %s %s" % (op, a.to_bytes(32, "little").hex(), b.to_bytes(32, "little").hex()),
                  (want % P).to_bytes(32, "little").hex()))


for a in values:
    add_f("q", a, 0, a * a)
    add_f("k", a, 0, a * 121665)
    add_f("i", a, 0, pow(a, P - 2, P))
    for b in values:
        add_f("a", a, b, a + b)
        add_f("s", a, b, a - b)
        add_f("m", a, b, a * b)

for length in [0, 1, 3, 4, 5, 63, 64, 65, 127, 128, 129, 200, 1024]:
    key = bytes(rng.randrange(256) for _ in range(32))
    nonce = bytes(rng.randrange(256) for _ in range(12))
    counter = rng.choice([0, 1, 7, 0xFFFFFFFE, 0xFFFFFFFF, rng.randrange(2**32)])
    cases.append(("c %s %s %x %x" % (key.hex(), nonce.hex(), counter, length),
                  chacha20_stream(key, nonce, counter, length).hex()))
cases.append(("c %s %s 1 40" % (RFC_KEY.hex(), RFC_NONCE.hex()),
              chacha20_stream(RFC_KEY, RFC_NONCE, 1, 64).hex()))

# ------------------------------------------------- SHA-512 and Poly1305 --
def hexmsg(m):
    return m.hex() or "-"


def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    h = 0
    for i in range(0, len(msg), 16):
        h = (h + int.from_bytes(msg[i:i + 16] + b"\x01", "little")) * r % (2**130 - 5)
    return ((h + s) % 2**128).to_bytes(16, "little")


rng2 = random.Random(8032)
for n in list(range(0, 300)) + [1000, 4000]:
    m = bytes(rng2.getrandbits(8) for _ in range(n))
    cases.append(("h " + hexmsg(m), hashlib.sha512(m).hexdigest()))
assert poly1305(bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"),
                b"Cryptographic Forum Research Group").hex() == "a8061dc1305136c6c22b8baf0c0127a9"
for n in list(range(0, 140)) + [1000, 4000]:
    for key in (bytes(rng2.getrandbits(8) for _ in range(32)), b"\xff" * 32,
                b"\xff" * 16 + bytes(16), bytes(32),
                # r = 1, 2 and 3 with every byte of the message 0xff: sums that land
                # on and just around the modulus, where the last step matters
                bytes([1]) + bytes(31), bytes([2]) + bytes(31), bytes([3]) + bytes(15) + b"\xff" * 16):
        for m in (bytes(rng2.getrandbits(8) for _ in range(n)), b"\xff" * n):
            cases.append(("p %s %s" % (key.hex(), hexmsg(m)), poly1305(key, m).hex()))

# ---------------------------------------------------------------- Ed25519 --
# The textbook way, with x and y as plain fractions modulo P: slow, and
# nothing like the kernel's method, which is the point.
ED_D = -121665 * pow(121666, P - 2, P) % P
ED_L = 2**252 + 27742317777372353535851937790883648493
ED_I = pow(2, (P - 1) // 4, P)


def ed_add(p, q):
    (x1, y1), (x2, y2) = p, q
    t = ED_D * x1 * x2 * y1 * y2 % P
    return ((x1 * y2 + x2 * y1) * pow(1 + t, P - 2, P) % P,
            (y1 * y2 + x1 * x2) * pow(1 - t, P - 2, P) % P)


def ed_mul(k, p):
    r = (0, 1)
    while k:
        if k & 1:
            r = ed_add(r, p)
        p = ed_add(p, p)
        k >>= 1
    return r


def ed_enc(p):
    return (p[1] | ((p[0] & 1) << 255)).to_bytes(32, "little")


def ed_dec(b):
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    u, v = (y * y - 1) % P, (ED_D * y * y + 1) % P
    x = u * pow(v, 3, P) * pow(u * pow(v, 7, P), (P - 5) // 8, P) % P
    if (v * x * x - u) % P:
        if (v * x * x + u) % P:
            return None
        x = x * ED_I % P
    if x == 0 and sign:
        return None
    if x & 1 != sign:
        x = P - x
    return (x, y % P)


ED_B = ed_dec((4 * pow(5, P - 2, P) % P).to_bytes(32, "little"))


def ed_secret(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    return (a & ((1 << 254) - 8)) | (1 << 254), h[32:]


def ed_sign(seed, msg):
    a, prefix = ed_secret(seed)
    pub = ed_enc(ed_mul(a, ED_B))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % ED_L
    R = ed_enc(ed_mul(r, ED_B))
    k = int.from_bytes(hashlib.sha512(R + pub + msg).digest(), "little") % ED_L
    return pub, R + ((r + k * a) % ED_L).to_bytes(32, "little")


def ed_verify(pub, sig, msg):
    A = ed_dec(pub)
    s = int.from_bytes(sig[32:], "little")
    if A is None or s >= ED_L:
        return 0
    k = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), "little") % ED_L
    # s*B - k*A, encoded, must be the first half of the signature
    return int(ed_enc(ed_add(ed_mul(s, ED_B), ed_mul(ED_L - k, A))) == sig[:32])


# RFC 8032, section 7.1, test 1
seed1 = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
pub1, sig1 = ed_sign(seed1, b"")
assert pub1.hex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
assert sig1.hex() == ("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
assert ed_verify(pub1, sig1, b"") == 1

signed = []
for i in range(120):
    seed = seed1 if i == 0 else bytes(rng2.getrandbits(8) for _ in range(32))
    msg = b"" if i == 0 else bytes(rng2.getrandbits(8) for _ in range(rng2.choice([1, 32, 64, 150, 400])))
    pub, sig = ed_sign(seed, msg)
    signed.append((pub, sig, msg))
    cases.append(("e %s %s" % (seed.hex(), hexmsg(msg)), pub.hex() + sig.hex()))
    cases.append(("v %s %s %s" % (pub.hex(), sig.hex(), hexmsg(msg)), "1"))


def flip(b, bit):
    b = bytearray(b)
    b[bit >> 3] ^= 1 << (bit & 7)
    return bytes(b)


def add_v(pub, sig, msg):
    cases.append(("v %s %s %s" % (pub.hex(), sig.hex(), hexmsg(msg)), str(ed_verify(pub, sig, msg))))


# Which 32-byte strings are points at all, and that they come back out the same.
def add_d(b):
    p = ed_dec(b)
    cases.append(("d " + b.hex(), "0" if p is None else ed_enc(p).hex()))


TOP = 1 << 255
for y in [0, 1, 2, 3, 4, P - 1, P - 2, P - 3, (P - 1) // 2, rng2.getrandbits(255)]:
    add_d(y.to_bytes(32, "little"))
    add_d((y | TOP).to_bytes(32, "little"))     # the same y with the other x; for x = 0 there is none
for _ in range(200):
    add_d(bytes(rng2.getrandbits(8) for _ in range(32)))    # about half are not points
for pub, sig, msg in signed[:20]:
    add_d(pub)
    add_d(flip(pub, 255))

forged = 0
for pub, sig, msg in signed[:60]:
    before = len(cases)
    for bit in (255, 254, 248, 0):                             # the last byte of R, and the first
        add_v(pub, flip(sig, bit), msg)
    add_v(pub, flip(sig, rng2.randrange(512)), msg)            # a damaged signature
    add_v(flip(pub, rng2.randrange(256)), sig, msg)            # the wrong key
    add_v(pub, sig, msg + b"x")                                # a different message
    if msg:
        add_v(pub, sig, flip(msg, rng2.randrange(len(msg) * 8)))
    # the same S written a second way (S + L) must not pass
    s = int.from_bytes(sig[32:], "little") + ED_L
    add_v(pub, sig[:32] + s.to_bytes(32, "little"), msg)
    add_v(pub, sig[:32] + (ED_L).to_bytes(32, "little"), msg)
    add_v(pub, sig[:32] + (ED_L - 1).to_bytes(32, "little"), msg)
    add_v(pub, bytes(64), msg)
    add_v(bytes(32), sig, msg)
    add_v(b"\xff" * 32, sig, msg)
    forged += sum(1 for c in cases[before:] if c[1] == "1")
assert forged == 0, forged
# The neutral point (0, 1) as the key "signs" everything with R = the neutral
# point and S = 0; a useless key, but it shows what S = L (which is 0 modulo
# L, written a second way) and an impossible key do when all else would pass.
NEUTRAL = (1).to_bytes(32, "little")
add_v(NEUTRAL, NEUTRAL + bytes(32), b"abc")
assert cases[-1][1] == "1"
add_v(NEUTRAL, NEUTRAL + ED_L.to_bytes(32, "little"), b"abc")
add_v((1 | TOP).to_bytes(32, "little"), NEUTRAL + bytes(32), b"abc")      # x = 0 cannot be "odd"
add_v(NEUTRAL, (1 | TOP).to_bytes(32, "little") + bytes(32), b"abc")
assert [c[1] for c in cases[-3:]] == ["0", "0", "0"]

out = subprocess.run([sys.argv[1]], input="\n".join(c[0] for c in cases) + "\n",
                     capture_output=True, text=True)
sys.stderr.write(out.stderr)
got = out.stdout.split("\n")[:-1]
bad = out.returncode != 0 or len(got) != len(cases)
for (line, want), answer in zip(cases, got):
    if answer != want:
        print("MISMATCH", line, "\n  got ", answer, "\n  want", want)
        bad = True
print("%d of %d answers compared" % (len(got), len(cases)))
sys.exit(1 if bad else 0)
