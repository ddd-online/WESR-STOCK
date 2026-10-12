"""抓 COM5 串口日志到文件。用法：
    python tools/serial_cap.py --port COM5 --secs 90 --out build/baseline.log
只读不写（本板 S3 原生 USB-Serial-JTAG 写会 Semaphore timeout，见 README §7）。
"""
import argparse
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=60.0)
    ap.add_argument("--out", default="build/serial.log")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    t_end = time.time() + args.secs
    n = 0
    with open(args.out, "wb") as f:
        while time.time() < t_end:
            chunk = ser.read(4096)
            if chunk:
                f.write(chunk)
                f.flush()
                n += len(chunk)
    ser.close()
    print(f"captured {n} bytes in {args.secs:.0f}s -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
