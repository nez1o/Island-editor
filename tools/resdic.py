def get_bit(name, bit):
    idx = bit >> 3
    if idx >= len(name): return 0
    return (name[len(name) - 1 - idx] >> (bit & 7)) & 1
def first_diff(a, b):
    for i in range(max(len(a), len(b)) * 8):
        if get_bit(a, i) != get_bit(b, i): return i
    return -1
def build(keys):
    """-> list of (ref, left, right, key) with node 0 = root (ref -1, key b'')"""
    nodes = [[-1, 0, 0, b'']]
    for key in keys:
        cur = nodes[0]; nxt = nodes[cur[1]]
        while cur[0] < nxt[0]:
            cur = nxt
            nxt = nodes[nxt[2]] if get_bit(key, nxt[0]) else nodes[nxt[1]]
        bit = first_diff(key, nxt[3])
        cur = nodes[0]; ni = cur[1]
        while cur[0] < nodes[ni][0] < bit:
            cur = nodes[ni]
            ni = cur[2] if get_bit(key, cur[0]) else cur[1]
        idx = len(nodes)
        new = [bit, 0, 0, key]
        if get_bit(key, bit): new[1], new[2] = ni, idx
        else: new[1], new[2] = idx, ni
        if cur[0] >= 0 and get_bit(key, cur[0]): cur[2] = idx
        else: cur[1] = idx
        nodes.append(new)
    return nodes
