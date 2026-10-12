"""对比不同 SPI 时钟下同一份固件的 [perf] lcd 均值（多次采样），判断 SPI 是否真是瓶颈。
用法: python tools/spi_bench.py build/dir   # 目录里放各档位的串口日志
"""
import re
import sys
import glob

PAT = re.compile(r"flush n=(\d+) mean=(\d+)/(\d+)/(\d+) us \(total/conv/lcd\)")


def summarize(path):
    tot = conv = lcd = n = 0
    for line in open(path, "rb").read().decode("utf-8", "replace").split("\n"):
        m = PAT.search(line)
        if not m:
            continue
        cnt, t, c, l = (int(m.group(i)) for i in range(1, 5))
        # mean 是均值，乘回总数还原成总微秒
        tot += t * cnt
        conv += c * cnt
        lcd += l * cnt
        n += cnt
    if n == 0:
        return None
    return tot / n, conv / n, lcd / n, n


def main():
    paths = sys.argv[1:] or sorted(glob.glob("build/spi_*.log"))
    print(f"{'log':28} {'frames':>7} {'total us':>10} {'conv us':>10} {'lcd us':>9}")
    for p in paths:
        s = summarize(p)
        if not s:
            print(f"{p:28} (无数据)")
            continue
        t, c, l, n = s
        print(f"{p:28} {n:7d} {t:10.1f} {c:10.1f} {l:9.1f}")


if __name__ == "__main__":
    raise SystemExit(main())
