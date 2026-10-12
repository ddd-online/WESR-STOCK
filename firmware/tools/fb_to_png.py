"""把串口日志里的 FB-BEGIN-FULL ... FB-END 块渲染成 PNG，用于核对画面。
用法: python tools/fb_to_png.py build/verify.log build/fb.png [block_index]
"""
import sys
import re

from PIL import Image

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main() -> int:
    log = sys.argv[1] if len(sys.argv) > 1 else "build/verify.log"
    out = sys.argv[2] if len(sys.argv) > 2 else "build/fb.png"
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    text = open(log, "rb").read().decode("utf-8", "replace")
    text = ANSI.sub("", text)
    lines = text.replace("\r", "").split("\n")

    blocks = []
    cur = None
    for ln in lines:
        if "FB-BEGIN-FULL" in ln:
            m = re.search(r"FB-BEGIN-FULL\s+(\d+)x(\d+)", ln)
            w, h = (int(m.group(1)), int(m.group(2))) if m else (400, 300)
            cur = {"w": w, "h": h, "rows": []}
            continue
        if cur is not None:
            if "FB-END" in ln:
                blocks.append(cur)
                cur = None
                continue
            # 只认纯 '#'/'.' 行（日志前缀会让长度不等于 w）
            body = ln.strip()
            if body and set(body) <= set("#.") and len(body) == cur["w"]:
                cur["rows"].append(body)

    if not blocks:
        print("no complete FB block found")
        return 1
    b = blocks[min(want, len(blocks) - 1)]
    print(f"found {len(blocks)} block(s); rendering block {want} "
          f"{b['w']}x{b['h']} with {len(b['rows'])} rows")

    img = Image.new("1", (b["w"], b["h"]), 0)   # 0 = 黑底
    px = img.load()
    for y, row in enumerate(b["rows"]):
        if y >= b["h"]:
            break
        for x, ch in enumerate(row):
            if ch == ".":
                px[x, y] = 1                      # 白：和真实屏一致（1 = 白）
    img.save(out)
    print(f"saved {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
