# Minimal Yaz0 decoder/encoder (greedy LZ with a hash chain), enough for ResourceSizeTable.srsizetable.
import struct

def decompress(src):
    assert src[:4] == b'Yaz0'
    size = struct.unpack_from('>I', src, 4)[0]
    dst = bytearray(); s = 16
    while len(dst) < size:
        code = src[s]; s += 1
        for bit in range(8):
            if len(dst) >= size: break
            if code & (0x80 >> bit):
                dst.append(src[s]); s += 1
            else:
                b1, b2 = src[s], src[s + 1]; s += 2
                dist = ((b1 & 0xf) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = src[s] + 0x12; s += 1
                else:
                    n += 2
                for _ in range(n):
                    dst.append(dst[-dist])
    return bytes(dst)

def compress(data, max_chain=64):
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + b'\0' * 8)
    heads = {}
    i, n = 0, len(data)
    while i < n:
        code_pos = len(out); out.append(0); code = 0
        for bit in range(8):
            if i >= n: break
            best_len, best_dist = 0, 0
            if i + 3 <= n:
                key = data[i:i + 3]
                chain = heads.get(key, [])
                for j in reversed(chain[-max_chain:]):
                    dist = i - j
                    if dist > 0x1000: continue
                    l = 3; m = min(0x111, n - i)
                    while l < m and data[j + l] == data[i + l]: l += 1
                    if l > best_len:
                        best_len, best_dist = l, dist
                        if l == m: break
            if best_len >= 3:
                d = best_dist - 1
                if best_len >= 0x12:
                    out += bytes([d >> 8, d & 0xff, best_len - 0x12])
                else:
                    out += bytes([((best_len - 2) << 4) | (d >> 8), d & 0xff])
                step = best_len
            else:
                code |= 0x80 >> bit
                out.append(data[i]); step = 1
            for k in range(i, min(i + step, n - 2)):
                heads.setdefault(data[k:k + 3], []).append(k)
            i += step
        out[code_pos] = code
    return bytes(out)
