"""穷举"4 行顺序 × 每行左右像素序"，找出与参照公式逐字节一致的那个组合。
参照（display_bsp.cpp 的 LUT 定义，已被单点法验证）：
    index = (x>>1)*75 + ((299-y)>>2)
    bit   = 7 - ((((299-y)&3) << 1) | (x&1))
    black(lv<0x7fff) -> 清位 ; white -> 置位
被测（整帧批量）：
    对每个 block_y，4 个连续源行各取 2 个像素组成 2bit，拼成 1 字节：
       byte = n[p0] | n[p1]<<2 | n[p2]<<4 | n[p3]<<6
    其中 n = (左?2:0) + (右?1:0)  或  (左?1:0) + (右?2:0)
    行序：p_i 指向 ybot+i（i=0..3）
"""
import itertools
import random

W, H, H4 = 400, 300, 75


def reference(lv):
    """lv: list[int]，长度 W*H，按行优先。返回 bytearray(15000)"""
    out = bytearray(W * H // 8)
    for y in range(H):
        inv_y = H - 1 - y
        block_y, local_y = inv_y >> 2, inv_y & 3
        base = y * W
        for x in range(W):
            index = (x >> 1) * H4 + block_y
            bit = 7 - ((local_y << 1) | (x & 1))
            if lv[base + x] < 0x7FFF:
                out[index] &= ~(1 << bit) & 0xFF
            else:
                out[index] |= 1 << bit
    return out


def candidate(lv, row_perm, left_high):
    out = bytearray(W * H // 8)
    for block_y in range(H4):
        ybot = H - 1 - 4 * block_y - 3
        for byte_x in range(W >> 1):
            x = byte_x << 1
            byte = 0
            for slot, ri in enumerate(row_perm):
                y = ybot + ri
                base = y * W + x
                l = 1 if lv[base] >= 0x7FFF else 0
                r = 1 if lv[base + 1] >= 0x7FFF else 0
                n = ((l << 1) | r) if left_high else ((r << 1) | l)
                byte |= n << (2 * slot)
            out[byte_x * H4 + block_y] = byte
    return out


def main():
    rng = random.Random(4242)
    cases = []
    # 白/黑/边界
    for v in (0xFFFF, 0x0000, 0x7FFF, 0x8000):
        cases.append([v] * (W * H))
    # 随机
    for _ in range(12):
        cases.append([rng.getrandbits(16) for _ in range(W * H)])
    # 稀疏
    for _ in range(6):
        cases.append([0xFFFF if rng.getrandbits(4) else 0x0000 for _ in range(W * H)])

    refs = [reference(c) for c in cases]

    matches = []
    for row_perm in itertools.permutations(range(4)):
        for left_high in (True, False):
            ok = all(candidate(c, row_perm, left_high) == r
                     for c, r in zip(cases, refs))
            if ok:
                matches.append((row_perm, left_high))

    if not matches:
        print("没有匹配组合（说明行内像素分组方式也不是这个形状）")
        return 1
    for row_perm, left_high in matches:
        print(f"MATCH  row_perm={row_perm} (p_i 指向 ybot+{row_perm[0]},...)"
              f"  left_high={left_high}  "
              f"-> byte = " + " | ".join(
                  f"n{p}<<{2*i}" for i, p in enumerate(row_perm)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
