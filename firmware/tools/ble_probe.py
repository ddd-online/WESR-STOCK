#!/usr/bin/env python
"""BLE 配网自检客户端（开发工具，不是产品的一部分）。

板子只有一颗物理 KEY，进配网模式得人手长按；这个脚本让 PC 当"假手机"，
把 spec §10 的命令跑一遍 —— 协议有没有通、回包对不对，不用等小程序。

用法（板子先长按 KEY 进配网模式）：
    python tools/ble_probe.py                 # 跑全部命令
    python tools/ble_probe.py --only hello    # 只跑一条
    python tools/ble_probe.py --scan          # 只扫广播，不连接

依赖：pip install bleak
"""
import argparse
import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

# spec §10 冻结的三个 UUID（nRF52840 那套 Nordic UART Service）
SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # PC 写（小程序 → 板子）
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 板子 notify（订阅）

CHUNK = 20          # 固定 20 字节分片，不做 MTU 协商（和 iOS 一致）
NAME_PREFIX = "WESR"


class Nus:
    """按 '\n' 重组的 NUS 通道。"""

    def __init__(self, client):
        self.client = client
        self._buf = bytearray()
        self._rx = asyncio.Queue()

    def _on_notify(self, _char, data: bytearray):
        self._buf += data
        while b"\n" in self._buf:
            line, _, rest = bytes(self._buf).partition(b"\n")
            self._buf = bytearray(rest)
            self._rx.put_nowait(line.decode("utf-8", "replace"))

    async def start(self):
        await self.client.start_notify(TX, self._on_notify)

    async def send(self, obj, timeout=6.0):
        raw = (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        for i in range(0, len(raw), CHUNK):
            await self.client.write_gatt_char(RX, raw[i:i + CHUNK], response=False)
        text = await asyncio.wait_for(self._rx.get(), timeout)
        return json.loads(text)

    async def wait_event(self, timeout=2.5):
        """等板子主动推的事件（订阅成功后会立刻来一条 status）；超时返回 None。"""
        try:
            return json.loads(await asyncio.wait_for(self._rx.get(), timeout))
        except asyncio.TimeoutError:
            return None


async def discover(timeout=12.0):
    hits = {}

    def cb(dev, adv):
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        name = dev.name or ""
        if SVC in uuids or name.upper().startswith(NAME_PREFIX):
            hits[dev.address] = (dev, name, uuids)

    async with BleakScanner(detection_callback=cb):
        await asyncio.sleep(timeout)
    return hits


async def connect(dev, tries=3):
    """Windows 上第一次 connect 偶尔会在读 GATT 服务时超时，重试就好。"""
    for i in range(tries):
        client = BleakClient(dev, timeout=25)
        try:
            await client.connect()
            return client
        except Exception as e:                      # noqa: BLE001 - 只用来打印重试
            print(f"  连接失败（第 {i + 1}/{tries} 次）：{type(e).__name__}: {e}")
            try:
                await client.disconnect()
            except Exception:                       # noqa: BLE001
                pass
            await asyncio.sleep(2)
    return None


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="只跑一条命令")
    ap.add_argument("--scan", action="store_true", help="只扫广播")
    ap.add_argument("--stocks", help="用这份 JSON 数组当 setStocks 的参数（默认 2 只夹具）")
    ap.add_argument("--addr", help="直接连这个 MAC，跳过扫描（Windows 扫描偶尔抽风时用）")
    ap.add_argument("--cmd", help="只发这条原始 JSON（例如恢复板子设置：--cmd '{\"cmd\":\"setInterval\",\"sec\":15}'）")
    args = ap.parse_args()

    if args.addr:
        addr, dev = args.addr, args.addr
        print(f"直连 {addr}")
    else:
        print("扫描中（8 秒）… 板子要先长按 KEY 3 秒进配网模式")
        hits = {}
        for i in range(3):                      # Windows 上扫描偶发空手而归
            hits = await discover(8.0)
            if hits:
                break
            print(f"  第 {i + 1} 次没扫到，重试…")
        for a, (_, name, uuids) in hits.items():
            print(f"  找到 {a}  name={name!r}  services={uuids}")
        if not hits:
            print("没扫到 WESR-STOCK：① 板子在配网模式吗（状态条反白）② 蓝牙开了吗")
            return 1
        if args.scan:
            return 0
        addr, (dev, name, _) = next(iter(hits.items()))
    print(f"连接 {addr} …")
    client = await connect(dev)
    if not client:
        print("连不上：⑧ 板子还在配网模式吗（3 分钟没操作会自动退出）")
        return 3
    try:
        nus = Nus(client)
        await nus.start()
        print("已连接，TX 已订阅")
        boot = await nus.wait_event()
        if boot:
            print(f"  ← 设备主动推送 {json.dumps(boot, ensure_ascii=False)}")
        else:
            print("  ← 订阅后没等到主动推送（板子固件可能是旧的）")

        items = json.loads(args.stocks) if args.stocks else [
            {"code": "sh600519", "name": "贵州茅台"},
            {"code": "sz000858", "name": "五粮液", "mark": "酒"},
        ]
        if args.cmd:
            raw = json.loads(args.cmd)
            steps = [(raw.get("cmd", "?"), raw)]
        else:
            steps = [
                ("hello", {"cmd": "hello"}),
                ("scan", {"cmd": "scan"}),
                ("getCfg", {"cmd": "getCfg"}),
                ("setStocks", {"cmd": "setStocks", "items": items}),
                ("setInterval", {"cmd": "setInterval", "sec": 30}),
                # 用手机/PC 的当前时间：板子本来就跟 SNTP 对齐，传假时间会把表拨乱
                ("timeSync", {"cmd": "timeSync", "unix": int(time.time())}),
                ("exit", {"cmd": "exit"}),      # 退出配网：板子会拆掉蓝牙栈把内存还回去
            ]
        if args.only and not args.cmd:
            steps = [s for s in steps if s[0] == args.only] or [(args.only, {"cmd": args.only})]

        ok = fail = 0
        for label, obj in steps:
            print(f"→ {json.dumps(obj, ensure_ascii=False)}")
            try:
                reply = await nus.send(obj)
            except asyncio.TimeoutError:
                print("  ← 超时（6 秒没回包）")
                fail += 1
                continue
            print(f"  ← {json.dumps(reply, ensure_ascii=False)}")
            if reply.get("ev") == "err":
                fail += 1
            else:
                ok += 1
        print(f"\n{ok} 条有正常回包，{fail} 条失败")
        return 0 if fail == 0 else 2
    finally:
        await client.disconnect()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
