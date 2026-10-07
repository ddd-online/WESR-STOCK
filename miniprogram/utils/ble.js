/* Nordic UART Service 客户端：小程序 ↔ 板子。
   协议：UTF-8 JSON + '\n' 结尾，双向固定 20 字节分片（spec §10）。

   ponytail: 不引任何 npm 包（NUS 库、UTF-8 库都不需要）—— 三个 UUID + 手写
   编解码 + 分包重组就是全部。纯函数部分在 Node 下也能跑，见 tools/selftest.js。 */

const SVC = '6E400001-B5A3-F393-E0A9-E50E24DCCA9E'
const RX = '6E400002-B5A3-F393-E0A9-E50E24DCCA9E'   // 小程序写 → 板子收
const TX = '6E400003-B5A3-F393-E0A9-E50E24DCCA9E'   // 板子 notify → 小程序订阅

const CHUNK = 20          // 固定 20 字节，不做 MTU 协商（iOS 不支持 setBLEMTU）
const MAX_MSG = 4096      // 单条报文上限，超了丢本次会话（和固件一致）
const DEF_TIMEOUT = 8000

/* ---------------- 纯函数：UTF-8 与分片（Node 可测） ---------------- */

function utf8Encode(str) {
  const out = []
  for (let i = 0; i < str.length; i++) {
    let c = str.charCodeAt(i)
    if (c < 0x80) {
      out.push(c)
    } else if (c < 0x800) {
      out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f))
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      const c2 = str.charCodeAt(++i)                       // 代理对 → 4 字节
      const cp = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00)
      out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f),
               0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
    } else {
      out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f))
    }
  }
  return new Uint8Array(out)
}

function utf8Decode(bytes) {
  let s = ''
  for (let i = 0; i < bytes.length;) {
    const b = bytes[i++]
    if (b < 0x80) {
      s += String.fromCharCode(b)
    } else if (b < 0xe0) {
      s += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i++] & 0x3f))
    } else if (b < 0xf0) {
      s += String.fromCharCode(((b & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) |
                               (bytes[i++] & 0x3f))
    } else {
      const cp = ((b & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) |
                 ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f)
      const u = cp - 0x10000
      s += String.fromCharCode(0xd800 + (u >> 10), 0xdc00 + (u & 0x3ff))
    }
  }
  return s
}

/* 一条报文 → 若干 ≤20 字节的 ArrayBuffer（末尾带 '\n'） */
function encodeChunks(text) {
  const bytes = utf8Encode(text + '\n')
  const out = []
  for (let i = 0; i < bytes.length; i += CHUNK) {
    const slice = bytes.subarray(i, i + CHUNK)
    const buf = new ArrayBuffer(slice.length)
    new Uint8Array(buf).set(slice)
    out.push(buf)
  }
  return out
}

/* 接收重组器：喂字节，吐完整报文（去掉 '\n'）。
   超 4KB 还没换行就丢掉这条，并在下一个换行处重新同步 —— 和固件 ble_proto.c 同一套语义。 */
function Reassembler() {
  this.buf = []
  this.drop = false
}

Reassembler.prototype.push = function (bytes) {
  const lines = []
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i]
    if (this.drop) {
      if (b === 0x0a) this.drop = false      // 残片到此为止
      continue
    }
    if (this.buf.length >= MAX_MSG) {
      this.buf = []
      this.drop = true
      if (b === 0x0a) this.drop = false
      continue
    }
    if (b === 0x0a) {
      lines.push(utf8Decode(this.buf))
      this.buf = []
    } else {
      this.buf.push(b)
    }
  }
  return lines
}

/* ---------------- 微信 BLE 那一层 ---------------- */

let deviceId = null
let serviceId = null
let rxChar = null
let txChar = null
let listener = null
let rx = new Reassembler()
let pending = null            // { resolve, reject, timer }

function setListener(fn) {
  listener = fn
}

function connected() {
  return !!deviceId
}

function fail(msg) {
  return new Promise((_, reject) => reject(new Error(msg)))
}

/* 开发者工具模拟器没有蓝牙协议栈：openBluetoothAdapter 必失败，报回来的是
   「适配器不可用」，看起来像板子的问题。这里单认出来，别让用户去查板子。 */
function inDevtools() {
  try {
    return wx.getSystemInfoSync().platform === 'devtools'
  } catch (e) {
    return false                 // 拿不到就当真机，走正常流程
  }
}

/* 扫广播：按服务 UUID 过滤，返回 [{deviceId, name, rssi}]。
   板子在配网模式下广播 NUS 服务 UUID（固件 ble_task.cpp advertise()），
   广播名是截短的 "WESR"，完整名 "WESR-STOCK" 在 scan rsp 里 —— 两种情况名字都以 WESR 开头。 */
function scan(ms) {
  return new Promise((resolve, reject) => {
    if (inDevtools()) {
      return reject(new Error('开发者工具模拟器没有蓝牙：请点工具栏「真机调试」，' +
                              '或用「预览」拿手机扫'))
    }
    const found = {}
    let done = false
    const onFound = (res) => {
      (res.devices || []).forEach((d) => {
        const name = d.name || d.localName || ''
        if (name.indexOf('WESR') === 0 || (d.advertisServiceUUIDs || [])
            .some((u) => u.toUpperCase() === SVC)) {
          found[d.deviceId] = {
            deviceId: d.deviceId,
            name: name || '(无名)',
            rssi: d.RSSI || 0,
          }
        }
      })
    }
    /* 每条出口都必须清监听 + 停扫描，否则第二次扫会叠一份回调，
       而且板子那边会一直被连着扫（费电、也抢 WiFi 天线） */
    const stop = (err) => {
      if (done) return
      done = true
      clearTimeout(timer)
      if (wx.offBluetoothDeviceFound) wx.offBluetoothDeviceFound(onFound)
      wx.stopBluetoothDevicesDiscovery()
      if (err) reject(err)
      else resolve(Object.keys(found).map((k) => found[k])
        .sort((a, b) => b.rssi - a.rssi))
    }
    const timer = setTimeout(() => stop(null), ms || 6000)

    wx.onBluetoothDeviceFound(onFound)
    wx.openBluetoothAdapter({
      success: () => {
        wx.startBluetoothDevicesDiscovery({
          services: [SVC],
          allowDuplicatesKey: false,
          fail: (e) => stop(new Error('扫描失败（errCode ' + (e && e.errCode) + '）：' +
                                      ((e && e.errMsg) || '未知'))),
        })
      },
      /* errCode 10001 = 适配器不可用，真机上就是蓝牙没开；别的错原样报出来，
         免得又出现"手机蓝牙明明是开的，却被告知没开" */
      fail: (e) => stop(new Error((e && e.errCode) === 10001
        ? '蓝牙没开：请到系统设置里打开蓝牙'
        : '打不开蓝牙适配器（errCode ' + (e && e.errCode) + '）：' + ((e && e.errMsg) || '未知'))),
    })
  })
}

/* 连上 + 订阅：解析出 NUS 的三个 UUID，之后 send() 才有用 */
function connect(id) {
  return new Promise((resolve, reject) => {
    wx.createBLEConnection({
      deviceId: id,
      timeout: 10000,
      success: () => {
        wx.getBLEDeviceServices({
          deviceId: id,
          success: (s) => {
            const svc = (s.services || []).find(
              (x) => x.uuid.toUpperCase() === SVC)
            if (!svc) return reject(new Error('这不是配网中的板子（没找到 NUS 服务）'))
            serviceId = svc.uuid
            wx.getBLEDeviceCharacteristics({
              deviceId: id,
              serviceId,
              success: (c) => {
                const pick = (u) => (c.characteristics || []).find(
                  (x) => x.uuid.toUpperCase() === u)
                const r = pick(RX), t = pick(TX)
                if (!r || !t) return reject(new Error('NUS 特征值不全'))
                rxChar = r.uuid
                txChar = t.uuid
                deviceId = id
                rx = new Reassembler()
                wx.onBLECharacteristicValueChange((res) => {
                  rx.push(new Uint8Array(res.value)).forEach((line) => {
                    let msg
                    try {
                      msg = JSON.parse(line)
                    } catch (e) {
                      return                      // 半条/脏数据直接丢
                    }
                    if (pending) {                // 发给 send() 的回包
                      const p = pending
                      pending = null
                      clearTimeout(p.timer)
                      p.resolve(msg)
                    }
                    if (listener) listener(msg)
                  })
                })
                wx.notifyBLECharacteristicValueChange({
                  deviceId, serviceId, characteristicId: txChar, state: true,
                  success: () => resolve(),
                  fail: (e) => reject(new Error('订阅失败：' + (e.errMsg || ''))),
                })
              },
              fail: (e) => reject(new Error('读特征值失败：' + (e.errMsg || ''))),
            })
          },
          fail: (e) => reject(new Error('读服务失败：' + (e.errMsg || ''))),
        })
      },
      fail: (e) => reject(new Error('连接失败（靠近板子再试）：' + (e.errMsg || ''))),
    })
  })
}

function disconnect() {
  if (deviceId) wx.closeBLEConnection({ deviceId })
  deviceId = null
}

/* 发一条命令，等它的回包。板子订阅成功时会主动推一条 status，
   所以调用方如果收到的是推送，send() 的等待不受影响 —— 那个阶段还没人调 send。 */
function send(obj, timeout) {
  if (!deviceId) return fail('还没连上板子')
  if (pending) return fail('上一条命令还没回话，稍等一下')   // 免得两个 Promise 互相顶掉
  const chunks = encodeChunks(JSON.stringify(obj))
  return new Promise((resolve, reject) => {
    pending = {
      resolve,
      reject,
      timer: setTimeout(() => {
        pending = null
        reject(new Error('板子 8 秒没回话'))
      }, timeout || DEF_TIMEOUT),
    }
    const writeNext = (i) => {
      if (i >= chunks.length) return
      wx.writeBLECharacteristicValue({
        deviceId,
        serviceId,
        characteristicId: rxChar,
        value: chunks[i],
        writeType: 'write',          // 带响应：20 字节连发不会丢，也不用加延时
        success: () => writeNext(i + 1),
        fail: (e) => {
          if (pending) {
            const p = pending
            pending = null
            clearTimeout(p.timer)
          }
          reject(new Error('发送失败：' + (e.errMsg || '')))
        },
      })
    }
    writeNext(0)
  })
}

module.exports = {
  SVC, RX, TX, CHUNK, MAX_MSG,
  utf8Encode, utf8Decode, encodeChunks, Reassembler,   // 给 selftest 用
  setListener, connected, scan, connect, disconnect, send,
}
