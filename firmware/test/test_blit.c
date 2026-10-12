/* 面板位序回归测试：DisplayPort::RLCD_BlitLvglFrame（整帧批量转换）
   必须与 AlgorithmOptimization==3 的逐像素 LUT 路径逐字节一致。

   为什么必须有这条：这条一旦挂了，屏上会出现内容错位/残影这类
   "看起来像硬件坏了"的现象，而编译器不会报任何错。

   两侧都按 display_bsp.cpp 的定义写成公式形式（等价于那两张 LUT）：
       index = (x>>1)*75 + ((299-y)>>2)
       bit   = 7 - ((((299-y)&3) << 1) | (x&1))
       black(lv<0x7fff) → 清位 ; white → 置位
   被测侧：每 4 个连续源行产出一个整字节（block_y 固定、byte_x 连写）。

   宿主机运行： mingw32-make -C test run   （Makefile 里已并入 run 目标）
*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 400
#define H 300
#define H4 (H >> 2)      /* 75 */

/* ---------- 参照：逐像素、按公式落位 ---------- */
static void ref_convert(const uint16_t *lv, int stride, uint8_t *out)
{
    memset(out, 0, W * H / 8);
    for (int y = 0; y < H; y++) {
        int inv_y = H - 1 - y;
        int block_y = inv_y >> 2;
        int local_y = inv_y & 3;
        for (int x = 0; x < W; x++) {
            uint32_t index = (uint32_t)(x >> 1) * H4 + (uint32_t)block_y;
            uint8_t bit = (uint8_t)(7 - ((local_y << 1) | (x & 1)));
            if (lv[(size_t)y * stride + x] < 0x7fff) out[index] &= (uint8_t)~(1u << bit);
            else                                     out[index] |= (uint8_t)(1u << bit);
        }
    }
}

/* ---------- 被测：与 display_bsp.cpp 的 RLCD_BlitLvglFrame 同形 ---------- */
static void new_convert(const uint16_t *lv, int stride, uint8_t *out)
{
    /* (左像素?2:0) + (右像素?1:0)：白=1（置位）、黑=0（清位） */
    static const uint8_t kNib[4] = { 0x0, 0x1, 0x2, 0x3 };
    memset(out, 0, W * H / 8);
    for (int block_y = 0; block_y < H4; block_y++) {
        const int ybot = H - 4 - 4 * block_y;                     /* bit0/1 所在行（本组最上面一行） */
        const uint16_t *p0 = lv + (size_t)ybot * stride;          /* bit0/1 */
        const uint16_t *p1 = p0 + stride;                         /* bit2/3 */
        const uint16_t *p2 = p1 + stride;                         /* bit4/5 */
        const uint16_t *p3 = p2 + stride;                         /* bit6/7 */
        uint8_t *o = out + block_y;
        for (int byte_x = 0; byte_x < (W >> 1); byte_x++, o += H4) {
            const int x = byte_x << 1;
            int n0 = ((p0[x] >= 0x7fff) ? 2 : 0) + ((p0[x + 1] >= 0x7fff) ? 1 : 0);
            int n1 = ((p1[x] >= 0x7fff) ? 2 : 0) + ((p1[x + 1] >= 0x7fff) ? 1 : 0);
            int n2 = ((p2[x] >= 0x7fff) ? 2 : 0) + ((p2[x + 1] >= 0x7fff) ? 1 : 0);
            int n3 = ((p3[x] >= 0x7fff) ? 2 : 0) + ((p3[x + 1] >= 0x7fff) ? 1 : 0);
            *o = (uint8_t)(kNib[n0] | (kNib[n1] << 2) | (kNib[n2] << 4) | (kNib[n3] << 6));
        }
    }
}

static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

/* 单点法自检：验证"参照"本身没写错（一个孤立的黑像素应该只清掉一位）。
   左列是 bit1、右列是 bit0；自下而上每行占 2 位。 */
static int check_reference_itself(void)
{
    static uint16_t lv[W * H];
    static uint8_t d[W * H / 8];
    const int xs[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };
    const int ys[8] = { 296, 296, 297, 297, 298, 298, 299, 299 };
    const uint8_t want[8] = { 0x02, 0x01, 0x08, 0x04, 0x20, 0x10, 0x80, 0x40 };
    int bad = 0;
    for (int k = 0; k < 8; k++) {
        for (int i = 0; i < W * H; i++) lv[i] = 0xFFFF;
        lv[(size_t)ys[k] * W + xs[k]] = 0x0000;
        ref_convert(lv, W, d);
        uint8_t cleared = (uint8_t)(0xFF ^ d[0]);
        if (cleared != want[k]) {
            printf("FAIL 参照自检: 黑点(x=%d,y=%d) 清位=%02X 期望=%02X\n",
                   xs[k], ys[k], cleared, want[k]);
            bad++;
        }
    }
    if (!bad) printf("  blit 参照自检 ok（8 个孤立黑点各自只清一位）\n");
    return bad;
}

int main(void)
{
    static uint16_t lv[W * H];
    static uint8_t a[W * H / 8], b[W * H / 8];
    int fails = check_reference_itself();

    /* 1) 全白 / 全黑 / 边界值 */
    const uint16_t cases[] = { 0xFFFF, 0x0000, 0x7FFF, 0x8000 };
    for (int ci = 0; ci < 4; ci++) {
        for (int i = 0; i < W * H; i++) lv[i] = cases[ci];
        ref_convert(lv, W, a);
        new_convert(lv, W, b);
        if (memcmp(a, b, sizeof a) != 0) { printf("FAIL solid 0x%04X\n", cases[ci]); fails++; }
    }

    /* 2) 随机图 —— 200 轮 */
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < W * H; i++) lv[i] = (uint16_t)rnd();
        ref_convert(lv, W, a);
        new_convert(lv, W, b);
        if (memcmp(a, b, sizeof a) != 0) {
            for (int i = 0; i < (int)sizeof a; i++)
                if (a[i] != b[i]) { printf("FAIL round %d byte %d: ref=%02X new=%02X\n", round, i, a[i], b[i]); break; }
            fails++;
            if (fails > 3) break;
        }
    }

    /* 3) 稀疏点阵 —— 文字/图形场景（大部分像素是背景） */
    for (int r = 0; r < 30; r++) {
        for (int i = 0; i < W * H; i++) lv[i] = (rnd() & 15) ? 0xFFFF : 0x0000;
        ref_convert(lv, W, a);
        new_convert(lv, W, b);
        if (memcmp(a, b, sizeof a) != 0) { printf("FAIL sparse round %d\n", r); fails++; break; }
    }

    /* 4) 逐列渐变 —— 覆盖所有 local_x/local_y 组合 */
    for (int i = 0; i < W * H; i++) {
        int x = i % W, y = i / W;
        lv[i] = ((x * 3 + y * 7) % 5 == 0) ? 0x0000 : 0xFFFF;
    }
    ref_convert(lv, W, a);
    new_convert(lv, W, b);
    if (memcmp(a, b, sizeof a) != 0) { printf("FAIL gradient\n"); fails++; }

    printf(fails ? "test_blit: %d FAILED\n" : "test_blit ok\n", fails);
    return fails ? 1 : 0;
}
