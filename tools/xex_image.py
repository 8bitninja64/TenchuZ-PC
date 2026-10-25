"""Read the supported retail XEX directly. AES-128-CBC comes from the system: Windows'
built-in provider (BCrypt), elsewhere OpenSSL's libcrypto, else a pure-Python fallback."""
import ctypes
import ctypes.util
from pathlib import Path
import struct
import sys

RETAIL_KEY = bytes.fromhex('20b185a59d28fdc340583fbb0896bf91')
MAX_IMAGE = 64 * 1024 * 1024


def decrypt(data: bytes, key: bytes) -> bytes:
    if not data or len(data) % 16:
        raise ValueError('Invalid encrypted XEX length')
    if sys.platform != 'win32':
        return _decrypt_openssl(data, key) or _decrypt_python(data, key)
    cng = ctypes.WinDLL('bcrypt')
    handle = ctypes.c_void_p
    ulong = ctypes.c_ulong
    cng.BCryptOpenAlgorithmProvider.argtypes = [ctypes.POINTER(handle), ctypes.c_wchar_p, ctypes.c_wchar_p, ulong]
    cng.BCryptSetProperty.argtypes = [handle, ctypes.c_wchar_p, ctypes.c_void_p, ulong, ulong]
    cng.BCryptGenerateSymmetricKey.argtypes = [handle, ctypes.POINTER(handle), ctypes.c_void_p, ulong, ctypes.c_void_p, ulong, ulong]
    cng.BCryptDecrypt.argtypes = [handle, ctypes.c_void_p, ulong, ctypes.c_void_p, ctypes.c_void_p, ulong, ctypes.c_void_p, ulong, ctypes.POINTER(ulong), ulong]
    cng.BCryptDestroyKey.argtypes = [handle]
    cng.BCryptCloseAlgorithmProvider.argtypes = [handle, ulong]
    algorithm, symmetric = handle(), handle()

    def check(status):
        if status < 0:
            raise RuntimeError('Windows AES decoding failed')

    try:
        check(cng.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), 'AES', None, 0))
        mode = ctypes.create_unicode_buffer('ChainingModeCBC')
        check(cng.BCryptSetProperty(algorithm, 'ChainingMode', mode, ctypes.sizeof(mode), 0))
        secret = ctypes.create_string_buffer(key)
        check(cng.BCryptGenerateSymmetricKey(algorithm, ctypes.byref(symmetric), None, 0, secret, len(key), 0))
        buffer = ctypes.create_string_buffer(data, len(data))
        iv = ctypes.create_string_buffer(16)
        written = ulong()
        check(cng.BCryptDecrypt(symmetric, buffer, len(data), None, iv, 16, buffer, len(data), ctypes.byref(written), 0))
        if written.value != len(data):
            raise ValueError('Incomplete AES output')
        return buffer.raw
    finally:
        if symmetric.value:
            cng.BCryptDestroyKey(symmetric)
        if algorithm.value:
            cng.BCryptCloseAlgorithmProvider(algorithm, 0)



def _decrypt_openssl(data: bytes, key: bytes):
    """AES-128-CBC (zero IV, no padding) through libcrypto; None when it is not there."""
    library = None
    for name in (ctypes.util.find_library('crypto'), 'libcrypto.so.3', 'libcrypto.so.1.1', 'libcrypto.so'):
        if not name:
            continue
        try:
            library = ctypes.CDLL(name)
            break
        except OSError:
            continue
    if library is None:
        return None
    try:
        new, free = library.EVP_CIPHER_CTX_new, library.EVP_CIPHER_CTX_free
        cipher = library.EVP_aes_128_cbc
    except AttributeError:
        return None
    new.restype = ctypes.c_void_p
    free.argtypes = [ctypes.c_void_p]
    cipher.restype = ctypes.c_void_p
    library.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                                           ctypes.c_char_p]
    library.EVP_CIPHER_CTX_set_padding.argtypes = [ctypes.c_void_p, ctypes.c_int]
    library.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                          ctypes.c_char_p, ctypes.c_int]
    context = new()
    if not context:
        return None
    try:
        output = ctypes.create_string_buffer(len(data) + 16)
        written = ctypes.c_int()
        if (library.EVP_DecryptInit_ex(context, cipher(), None, key, bytes(16)) != 1 or
                library.EVP_CIPHER_CTX_set_padding(context, 0) != 1 or
                library.EVP_DecryptUpdate(context, output, ctypes.byref(written), data, len(data)) != 1 or
                written.value != len(data)):
            return None
        return output.raw[:len(data)]
    finally:
        free(context)


def _aes_tables():
    sbox = [0] * 256
    p = q = 1
    while True:  # multiplicative inverse in GF(2^8) through generator 3, then the affine map
        p = (p ^ (p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        rot = lambda v, n: ((v << n) | (v >> (8 - n))) & 0xFF
        sbox[p] = q ^ rot(q, 1) ^ rot(q, 2) ^ rot(q, 3) ^ rot(q, 4) ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    inverse = [0] * 256
    for i, value in enumerate(sbox):
        inverse[value] = i

    def mul(a, b):
        result = 0
        while b:
            if b & 1:
                result ^= a
            a = ((a << 1) ^ (0x1B if a & 0x80 else 0)) & 0xFF
            b >>= 1
        return result
    # Inverse round tables: InvSubBytes then InvMixColumns for each row position.
    tables = [[0] * 256 for _ in range(4)]
    for x in range(256):
        s = inverse[x]
        column = (mul(s, 14) << 24) | (mul(s, 9) << 16) | (mul(s, 13) << 8) | mul(s, 11)
        for t in range(4):
            tables[t][x] = ((column >> (8 * t)) | (column << (32 - 8 * t))) & 0xFFFFFFFF
    return sbox, inverse, tables, mul


def _decrypt_python(data: bytes, key: bytes) -> bytes:
    """Slow but dependency-free AES-128-CBC decryption (zero IV)."""
    sbox, inverse, (t0, t1, t2, t3), mul = _aes_tables()
    words = list(struct.unpack('>4I', key))
    rcon = 1
    for i in range(4, 44):
        w = words[i - 1]
        if i % 4 == 0:
            w = ((w << 8) | (w >> 24)) & 0xFFFFFFFF
            w = (sbox[w >> 24] << 24) | (sbox[(w >> 16) & 0xFF] << 16) | (sbox[(w >> 8) & 0xFF] << 8) | sbox[w & 0xFF]
            w ^= rcon << 24
            rcon = ((rcon << 1) ^ (0x1B if rcon & 0x80 else 0)) & 0xFF
        words.append(words[i - 4] ^ w)
    # Decryption round keys (equivalent inverse cipher): InvMixColumns on rounds 1-9.
    def inv_mix(w):
        b = [(w >> 24) & 0xFF, (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF]
        return ((mul(b[0], 14) ^ mul(b[1], 11) ^ mul(b[2], 13) ^ mul(b[3], 9)) << 24 |
                (mul(b[0], 9) ^ mul(b[1], 14) ^ mul(b[2], 11) ^ mul(b[3], 13)) << 16 |
                (mul(b[0], 13) ^ mul(b[1], 9) ^ mul(b[2], 14) ^ mul(b[3], 11)) << 8 |
                (mul(b[0], 11) ^ mul(b[1], 13) ^ mul(b[2], 9) ^ mul(b[3], 14)))
    rounds = [words[40:44]] + [[inv_mix(w) for w in words[4 * r:4 * r + 4]] for r in range(9, 0, -1)] + [words[0:4]]
    out = bytearray(len(data))
    previous = (0, 0, 0, 0)
    unpack, pack = struct.Struct('>4I').unpack_from, struct.Struct('>4I').pack_into
    for offset in range(0, len(data), 16):
        c = unpack(data, offset)
        k = rounds[0]
        s0, s1, s2, s3 = c[0] ^ k[0], c[1] ^ k[1], c[2] ^ k[2], c[3] ^ k[3]
        for k in rounds[1:10]:
            s0, s1, s2, s3 = (
                t0[s0 >> 24] ^ t1[(s3 >> 16) & 0xFF] ^ t2[(s2 >> 8) & 0xFF] ^ t3[s1 & 0xFF] ^ k[0],
                t0[s1 >> 24] ^ t1[(s0 >> 16) & 0xFF] ^ t2[(s3 >> 8) & 0xFF] ^ t3[s2 & 0xFF] ^ k[1],
                t0[s2 >> 24] ^ t1[(s1 >> 16) & 0xFF] ^ t2[(s0 >> 8) & 0xFF] ^ t3[s3 & 0xFF] ^ k[2],
                t0[s3 >> 24] ^ t1[(s2 >> 16) & 0xFF] ^ t2[(s1 >> 8) & 0xFF] ^ t3[s0 & 0xFF] ^ k[3])
        k = rounds[10]
        state = (s0, s1, s2, s3)
        result = []
        for i in range(4):
            a, b, cc, d = state[i], state[(i + 3) % 4], state[(i + 2) % 4], state[(i + 1) % 4]
            result.append(((inverse[a >> 24] << 24) | (inverse[(b >> 16) & 0xFF] << 16) |
                           (inverse[(cc >> 8) & 0xFF] << 8) | inverse[d & 0xFF]) ^ k[i])
        pack(out, offset, result[0] ^ previous[0], result[1] ^ previous[1], result[2] ^ previous[2],
             result[3] ^ previous[3])
        previous = c
    return bytes(out)

def decode_xex(data: bytes) -> tuple[bytes, bytes]:
    def word(offset):
        if offset < 0 or offset + 4 > len(data):
            raise ValueError('Truncated XEX field')
        return struct.unpack_from('>I', data, offset)[0]

    if not 24 <= len(data) <= MAX_IMAGE or data[:4] != b'XEX2':
        raise ValueError('Invalid XEX2 file')
    header_size, security, count = word(8), word(16), word(20)
    if not 24 <= header_size <= min(len(data), 0x100000):
        raise ValueError('Invalid XEX header size')
    if not 24 <= security <= header_size - 0x180 or count > (header_size - 24) // 8:
        raise ValueError('Invalid XEX header layout')
    image_size = word(security + 4)
    if not 0 < image_size <= MAX_IMAGE or word(security + 0x110) != 0x82000000:
        raise ValueError('Unsupported XEX image layout')
    formats = [word(28 + i * 8) for i in range(count) if word(24 + i * 8) == 0x3ff]
    if len(formats) != 1 or not 24 <= formats[0] <= header_size - 8:
        raise ValueError('Invalid XEX format header')
    start = formats[0]
    size, encryption, compression = struct.unpack_from('>IHH', data, start)
    if not 8 <= size <= header_size - start or encryption > 1 or compression > 1:
        raise ValueError('Unsupported XEX format for this port')
    payload = data[header_size:]
    if encryption:
        session = decrypt(data[security + 0x150:security + 0x160], RETAIL_KEY)
        payload = decrypt(payload, session)
    if compression == 0:
        if len(payload) < image_size:
            raise ValueError('Truncated XEX image')
        image = payload[:image_size]
    else:
        if size < 16 or (size - 8) % 8:
            raise ValueError('Invalid XEX block table')
        image = bytearray(image_size)
        source = destination = 0
        for offset in range(start + 8, start + size, 8):
            block_size, zeros = word(offset), word(offset + 4)
            if block_size > len(payload) - source or block_size + zeros > image_size - destination:
                raise ValueError('Invalid XEX block range')
            image[destination:destination + block_size] = payload[source:source + block_size]
            source += block_size
            destination += block_size + zeros
        # Security image size also includes the final zero-filled region.
        if len(payload) - source >= 16:
            raise ValueError('Incomplete XEX image or unexpected trailing payload')
        image = bytes(image)
    if image[:2] != b'MZ':
        raise ValueError('Decoded XEX image is invalid')
    return data[:header_size], image


def read_image(path: Path) -> bytes:
    return decode_xex(path.read_bytes())[1]
