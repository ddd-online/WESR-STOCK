/* Node 下跑：node miniprogram/tools/selftest.js
   只测不依赖 wx 的纯函数（UTF-8 编解码 + 20 字节分包 + 换行重组）——
   这和固件 test/test_logic.c 里的 test_ble_proto 是一对。 */
const assert = require('assert')
const ble = require('../utils/ble')

/* 1) UTF-8 往返：中文、emoji、ASCII */
for (const s of ['贵州茅台', '沪深300', 'a:b,c', '{"cmd":"hello"}', '🚀五粮液']) {
  assert.strictEqual(ble.utf8Decode(ble.utf8Encode(s)), s)
}
assert.deepStrictEqual(Array.from(ble.utf8Encode('贵')), [0xe8, 0xb4, 0xb5])

/* 2) 分包：每片 ≤20 字节，拼起来是原文 + '\n' */
const text = JSON.stringify({ cmd: 'setStocks', items: [
  { code: 'sh600519', name: '贵州茅台' },
  { code: 'sz000858', name: '五粮液', mark: '酒' },
] })
const chunks = ble.encodeChunks(text)
assert.ok(chunks.length > 1)
let all = []
chunks.forEach((c) => {
  const u = new Uint8Array(c)
  assert.ok(u.length <= ble.CHUNK, '分片超过 20 字节')
  all = all.concat(Array.from(u))
})
assert.strictEqual(ble.utf8Decode(all), text + '\n')
assert.strictEqual(all[all.length - 1], 0x0a)

/* 3) 重组：逐片喂进去，最后一片才出报文 */
const rx = new ble.Reassembler()
let got = []
chunks.forEach((c, i) => {
  const lines = rx.push(new Uint8Array(c))
  if (i < chunks.length - 1) assert.strictEqual(lines.length, 0)
  else got = lines
})
assert.strictEqual(got.length, 1)
assert.strictEqual(got[0], text)

/* 4) 一片里两条报文 */
const rx2 = new ble.Reassembler()
const two = ble.utf8Encode('{"ev":"ack"}\n{"ev":"status"}\n')
assert.deepStrictEqual(rx2.push(two), ['{"ev":"ack"}', '{"ev":"status"}'])

/* 5) 超 4KB 不含换行：丢这条，并在下一个换行处重新同步 */
const rx3 = new ble.Reassembler()
const junk = new Uint8Array(64).fill(0x78)
for (let i = 0; i < 100; i++) assert.deepStrictEqual(rx3.push(junk), [])  // 6400 > 4096
assert.strictEqual(rx3.buf.length, 0)
assert.strictEqual(rx3.drop, true)
assert.deepStrictEqual(rx3.push(ble.utf8Encode('tail\n')), [])            // 残片尾被丢掉
assert.strictEqual(rx3.drop, false)
assert.deepStrictEqual(rx3.push(ble.utf8Encode('{"ev":"ack"}\n')), ['{"ev":"ack"}'])

/* 6) 跨片的多字节字符（中文被切断在两片之间）不能被拆坏 */
const rx4 = new ble.Reassembler()
const bytes = ble.utf8Encode('{"ev":"cfg","mark":"贵"}\n')
const half = Math.floor(bytes.length / 2)
assert.deepStrictEqual(rx4.push(bytes.subarray(0, half)), [])
assert.deepStrictEqual(rx4.push(bytes.subarray(half)),
                       ['{"ev":"cfg","mark":"贵"}'])

console.log('  ble.js 自检通过（UTF-8 / 分包 / 重组 / 超限重同步 / 跨片中文）')

/* ---------------- 页面接线检查 ----------------
   wxml 里 bindXxx="foo" 的 foo 必须在 js 里存在 —— 这是小程序最常见的低级错，
   而开发者工具里要跑到那一步才报。这里用假 Page/wx 把三个页面 require 一遍就能查。 */
const fs = require('fs')
const path = require('path')

const root = path.join(__dirname, '..')
const appJson = JSON.parse(fs.readFileSync(path.join(root, 'app.json'), 'utf8'))

let current = ''
const pages = {}
global.getApp = () => ({ globalData: {} })
global.Page = (o) => { pages[current] = o }
global.App = () => {}
/* 任何 wx.xxx() 都当空函数：页面模块顶层不会调 wx，只在事件里调 */
global.wx = new Proxy({}, { get: () => () => {} })

/* 下面几节都要换掉 global.wx（假 BLE / 假 request），而"换 wx"是全局副作用。
   串成一条链顺序跑：否则 A 节 await 期间 B 节把 wx 顶掉，A 的 Promise 永远不落地
   （踩过一次：请求 stub 被扫描那节的 fakeWx 换走，进程直接静默退出、一行输出都没有）。 */
let sections = Promise.resolve()
function section(fn) {
  sections = sections.then(fn).catch((e) => {
    console.error(e)
    process.exit(1)
  })
}

let bindCount = 0
appJson.pages.forEach((p) => {
  const jsPath = path.join(root, p + '.js')
  const wxmlPath = path.join(root, p + '.wxml')
  const jsonPath = path.join(root, p + '.json')
  assert.ok(fs.existsSync(jsPath), p + '.js 不存在')
  assert.ok(fs.existsSync(wxmlPath), p + '.wxml 不存在')
  /* 缺 .json 时开发者工具只报「缺少 json 文件，无法注册为页面」，整个页面是白屏，
     所以这里当成硬错误挡住（wxss 可省，缺了会走 app.wxss 的全局样式） */
  assert.ok(fs.existsSync(jsonPath), p + '.json 不存在（页面无法注册）')
  JSON.parse(fs.readFileSync(jsonPath, 'utf8'))
  current = p
  require(jsPath)
  const page = pages[p]
  assert.ok(page && page.data, p + ' 没有调用 Page()')
  const wxml = fs.readFileSync(wxmlPath, 'utf8')
  const handlers = []
  const re = /bind(?:tap|input|change|confirm)="(\w+)"/g
  let m
  while ((m = re.exec(wxml)) !== null) handlers.push(m[1])
  assert.ok(handlers.length >= 3, p + '.wxml 一个事件绑定都没解析到')
  handlers.forEach((h) => {
    assert.strictEqual(typeof page[h], 'function',
      p + '.wxml 绑定了 ' + h + '，但 js 里没这个方法')
  })
  bindCount += handlers.length
  /* tabBar 上的页面必须都在 pages 里 */
  ;(appJson.tabBar.list || []).forEach((t) => {
    assert.ok(appJson.pages.indexOf(t.pagePath) >= 0, 'tabBar 指向了不存在的页面 ' + t.pagePath)
  })
})

console.log('  页面接线通过（' + appJson.pages.length + ' 个页面 / ' + bindCount + ' 个事件绑定）')

/* ---------------- 刷新间隔白名单对齐 ----------------
   白名单在两处：小程序 picker 的 INTERVALS 和固件 setInterval 的校验。两边不一致的后果是
   小程序给出板子会 E_ARG 的选项，或者板子存了 10 而 picker 高亮到 5。直接从固件源码里抠
   出白名单来比 —— 以后加间隔（比如这次的 10s）两边都得改，改漏一个这里就红。 */
const intervals = pages['pages/stocks/stocks'].data.intervals
const bleTaskPath = path.join(root, '..', 'firmware', 'components', 'wesr', 'app',
                               'ble_task.cpp')
if (fs.existsSync(bleTaskPath)) {
  const src = fs.readFileSync(bleTaskPath, 'utf8')
  const cond = (src.match(/sec != \d+(?: && sec != \d+)+/) || [])[0]
  assert.ok(cond, 'ble_task.cpp 里没找到 setInterval 的 sec 白名单')
  const accepted = (cond.match(/\d+/g) || []).map(Number).sort((a, b) => a - b)
  assert.deepStrictEqual(intervals.slice().sort((a, b) => a - b), accepted,
    '小程序 picker 的间隔和固件 setInterval 白名单不一致')

  /* picker 下标 → 秒：往中间插一项最容易错的就是这个映射（idx 会整体后移） */
  const ivData = { idx: 0, interval: 0, ok: '' }
  const ivIdx = intervals.indexOf(10)
  assert.ok(ivIdx > 0, '10 秒没进白名单')
  assert.strictEqual(pages['pages/stocks/stocks'].data.idx, intervals.indexOf(15),
    '默认 15 秒的 picker 下标不对（插项后 idx 要跟着移）')
  pages['pages/stocks/stocks'].onInterval.call({
    data: ivData,
    setData(o) { Object.assign(ivData, o) },
  }, { detail: { value: String(ivIdx) } })
  assert.strictEqual(ivData.interval, 10, 'picker 选 10 秒应该得到 interval=10')

  console.log('  刷新间隔对齐（' + intervals.join('/') + '，与固件白名单一致）')
} else {
  console.log('  跳过刷新间隔对齐（没看到固件源码）')
}

/* ---------------- 代码 → 名称（联网查） ----------------
   名称走腾讯分时接口（板子第 2/3 页用的同一个），qt 数组第 1 项就是名字。
   这里不联网：wx.request 换成假的，夹具用固件那份真实响应，
   这样"接口形状变了"这件事会被测出来，而不是等到用户敲代码才发现。 */
const stocksPage = pages['pages/stocks/stocks']

/* 1) 6 位数字 → 带前缀：按首位分市场（沪 6/9、深 0/2/3、北 4/8） */
const normCases = [
  ['600519', 'sh600519'], ['688981', 'sh688981'],      // 沪主板 / 科创板
  ['900901', 'sh900901'],                              // 沪 B
  ['000858', 'sz000858'], ['300750', 'sz300750'],      // 深主板 / 创业板
  ['200011', 'sz200011'], ['003816', 'sz003816'],      // 深 B / 00 开头新股
  ['430047', 'bj430047'], ['830799', 'bj830799'],      // 北交所
  [' 600519 ', 'sh600519'], ['sh600519', 'sh600519'],  // 空格 / 已经带前缀
  ['SH600519', 'sh600519'],                            // 大写也认
  ['60051', ''], ['6005190', ''], ['abc', ''], ['', ''], [null, ''],
  ['12345', ''],                                       // 1 开头没有对应市场
]
normCases.forEach(([input, want]) => {
  assert.strictEqual(stocksPage.normCode(input), want,
    'normCode(' + JSON.stringify(input) + ')')
})

/* 2) 从真实分时响应里取名字 + 回填语义（空名字才填、手填过的不动、自动填过的可覆盖） */
const minuteFixture = path.join(root, '..', 'firmware', 'test', 'fixtures',
                                'minute_sh600519.json')
assert.ok(fs.existsSync(minuteFixture), '缺少分时夹具 minute_sh600519.json')
const fixture = JSON.parse(fs.readFileSync(minuteFixture, 'utf8'))

/* 服务端把短名字用空格补宽、还会出现全角字母 —— 用真实抓下来的夹具测（见 test/fixtures/README.md） */
const fxDir = path.join(root, 'test', 'fixtures')
const fx = (c) => JSON.parse(fs.readFileSync(path.join(fxDir, 'name_' + c + '.json'), 'utf8'))
const fx858 = fx('sz000858')      // "五 粮 液"  → 五粮液
const fx002 = fx('sz000002')      // "万  科Ａ"  → 万科A（全角折半角）
const fx430 = fx('bj430047')      // "诺思兰德"  原样

/* 3) cleanName 直接测：补宽空格、全角折半角、首尾空白 */
const cleanCases = [
  ['五 粮 液', '五粮液'],
  ['万  科Ａ', '万科A'],
  ['诺思兰德', '诺思兰德'],
  [' 贵州茅台 ', '贵州茅台'],
  ['京东方Ａ', '京东方A'],
  ['ＴＣＬ科技', 'TCL科技'],
  ['', ''],
  [null, ''],
  [undefined, ''],
]
cleanCases.forEach(([input, want]) => {
  assert.strictEqual(stocksPage.cleanName(input), want,
    'cleanName(' + JSON.stringify(input) + ')')
})

function fakePage(items) {
  /* 方法挂在原型上（= 真机上 this 的样子），这样 lookupName → applyName/needsName
     这条链跟运行时是同一份代码，不是在这儿重写一遍 */
  const ctx = Object.create(stocksPage)
  ctx.data = { items }
  ctx.patched = {}
  ctx.setData = function (o) {
    Object.assign(this.patched, o)
    Object.keys(o).forEach((k) => {
      const m = k.match(/^items\[(\d+)\]\.(\w+)$/)
      if (m) this.data.items[Number(m[1])][m[2]] = o[k]
    })
  }
  return ctx
}

section(async () => {
  /* wx 只实现 request，别的（showLoading 之类）由 Proxy 兜底成空函数 */
  const setRequest = (fn) => {
    global.wx = new Proxy({ request: fn }, { get: (t, k) => (k in t ? t[k] : () => {}) })
  }
  /* 按 URL 里的 code 回对应夹具；没有的代码回"接口里没这个" */
  const byCode = (o) => {
    const m = o.url.match(/code=([a-z]{2}\d{6})/)
    const body = m && { sh600519: fixture, sz000858: fx858, sz000002: fx002,
                        bj430047: fx430 }[m[1]]
    if (body) o.success({ statusCode: 200, data: body })
    else o.success({ statusCode: 200, data: { code: 0, data: {} } })
  }
  setRequest(byCode)
  const p = fakePage([
    { code: 'sh600519', name: '', mark: '' },                      // 空名字 → 该填
    { code: 'sh600519', name: '手填的', mark: '' },                 // 手填过（没有 auto）→ 不动
    { code: 'sh600519', name: '旧的自动名', mark: '', auto: true },  // 自动填过 → 可以覆盖
    { code: 'sz000858', name: '', mark: '' },                      // 另一个代码 → 不动
  ])
  await stocksPage.lookupName.call(p, 'sh600519')
  assert.strictEqual(p.data.items[0].name, '贵州茅台', '空名字应该被填上')
  assert.strictEqual(p.data.items[0].auto, true)
  assert.strictEqual(p.data.items[1].name, '手填的', '手填的名字不能被覆盖')
  assert.strictEqual(p.data.items[2].name, '贵州茅台', '上次自动填的应该被刷新')
  assert.strictEqual(p.data.items[3].name, '', '别的代码不该被动')

  /* 真实服务端会把短名字补宽、还会带全角字母：填进去的必须是干净的名字 */
  const padded = [
    ['sz000858', '五粮液'],      // 夹具里是 "五 粮 液"
    ['sz000002', '万科A'],       // 夹具里是 "万  科Ａ"
    ['bj430047', '诺思兰德'],     // 北交所代码也走同一个接口
  ]
  for (const [code, want] of padded) {
    const c = fakePage([{ code, name: '', mark: '' }])
    await stocksPage.lookupName.call(c, code)
    assert.strictEqual(c.data.items[0].name, want, code + ' 的名称应该被规整')
  }

  /* 查不到：名字留空、只给一句提示，绝不抛出去挡住下发 */
  const q = fakePage([{ code: 'sh601988', name: '', mark: '' }])
  await stocksPage.lookupName.call(q, 'sh601988')
  assert.strictEqual(q.data.items[0].name, '')
  assert.ok(/没取到/.test(q.patched.tip), q.patched.tip)

  /* 域名白名单是最常见的坑，提示要说人话 */
  setRequest((o) => o.fail({ errMsg: 'request:fail url not in domain list' }))
  const r = fakePage([{ code: 'sh600519', name: '', mark: '' }])
  await stocksPage.lookupName.call(r, 'sh600519')
  assert.ok(/白名单/.test(r.patched.tip), r.patched.tip)

  console.log('  代码补前缀 + 联网取名称通过（' + normCases.length + ' 条映射 / 名字规整 ' +
              cleanCases.length + ' 条 / 回填语义 / 查不到与白名单提示）')
})

/* ---------------- 下发报文检查 ----------------
   股票页真正发给板子的 JSON 形状 —— 板子那边是按 spec §10 手写解析的，
   多发一个字段、少发一个字段都会 E_ARG。这里把 send 换成假的，看它到底发什么。 */
const bleMod = require('../utils/ble')
const sent = []
bleMod.send = (o) => { sent.push(o); return Promise.resolve({ ev: 'ack' }) }

const stocks = pages['pages/stocks/stocks']
stocks.onSave.call({
  data: {
    interval: 30,
    items: [
      { code: 'sh600519', name: '贵州茅台', mark: '' },       // 没填首字 → 不带 mark
      { code: ' sz000858 ', name: ' 五粮液 ', mark: '酒' },   // 前后空格要去掉
      { code: '', name: '空的', mark: '' },                   // 没填代码 → 丢掉
      { code: 'sh000300', name: '', mark: '' },               // 没填名字 → 丢掉
    ],
  },
  setData() {},
})

setTimeout(() => {
  assert.strictEqual(sent.length, 2, '应该只发 setStocks + setInterval')
  assert.strictEqual(sent[0].cmd, 'setStocks')
  assert.deepStrictEqual(sent[0].items, [
    { code: 'sh600519', name: '贵州茅台' },
    { code: 'sz000858', name: '五粮液', mark: '酒' },
  ])
  assert.deepStrictEqual(sent[1], { cmd: 'setInterval', sec: 30 })

  /* 配网页发的形状 */
  const wifi = pages['pages/wifi/wifi']
  sent.length = 0
  wifi.onSave.call({
    data: { ssid: 'waveware_private', pass: '12345678' },
    setData() {},
  })
  setTimeout(() => {
    assert.deepStrictEqual(sent[0], {
      cmd: 'setWifi', ssid: 'waveware_private', pass: '12345678',
    })
    console.log('  下发报文通过（setStocks 过滤 / setWifi 形状）')
  }, 0)
}, 0)

/* ---------------- 扫描报错文案检查 ----------------
   scan() 的每条失败路径都要说人话：模拟器没蓝牙、真机蓝牙没开（10001）、其它 errCode
   原样报出来。以前一律报「蓝牙没开」，在模拟器里就会让人跑去查板子（用户实测踩过）。 */
function fakeWx(opts) {
  const calls = []
  const impl = {
    getSystemInfoSync: () => ({ platform: opts.platform || 'android' }),
    onBluetoothDeviceFound: (cb) => { calls.push('on'); impl._cb = cb },
    offBluetoothDeviceFound: (cb) => {
      calls.push('off')
      assert.strictEqual(cb, impl._cb, 'off 的必须就是 on 的那个回调')
    },
    stopBluetoothDevicesDiscovery: () => calls.push('stop'),
    openBluetoothAdapter: (o) => {
      calls.push('open')
      if (opts.openFail) o.fail(opts.openFail)
      else o.success()
    },
    startBluetoothDevicesDiscovery: (o) => {
      calls.push('start')
      if (opts.startFail) return o.fail(opts.startFail)
      ;(opts.devices || []).forEach((d) => impl._cb({ devices: [d] }))
    },
    _calls: calls,
  }
  /* 没实现的 wx.xxx 一律当空函数 —— 上面几节（showLoading 之类）还在用同一个 wx */
  global.wx = new Proxy(impl, { get: (t, k) => (k in t ? t[k] : () => {}) })
  return global.wx
}

section(async () => {
  const fail = (e) => e

  /* 模拟器：直接拒，一个蓝牙 API 都不碰 */
  let w = fakeWx({ platform: 'devtools' })
  let err = await bleMod.scan(20).then(() => null, fail)
  assert.ok(/模拟器/.test(err.message), err.message)
  assert.deepStrictEqual(w._calls, [], '模拟器下不该调用任何蓝牙 API')

  /* 真机成功：按 rssi 排序，名字或服务 UUID 都能认出来，出口清监听 + 停扫描 */
  w = fakeWx({ devices: [
    { deviceId: 'a', name: 'WESR', RSSI: -70 },
    { deviceId: 'b', localName: 'WESR-STOCK', RSSI: -50 },
    { deviceId: 'c', name: 'MiBand', RSSI: -10 },              // 不是板子，丢掉
    { deviceId: 'd', name: '', advertisServiceUUIDs: [
      bleMod.SVC], RSSI: -60 },                                // 靠服务 UUID 认出来
  ] })
  const list = await bleMod.scan(30)
  assert.deepStrictEqual(list.map((x) => x.deviceId), ['b', 'd', 'a'])
  assert.deepStrictEqual(w._calls, ['on', 'open', 'start', 'off', 'stop'])

  /* 10001 = 适配器不可用（真机蓝牙没开）；别的 errCode 原样报出来，不冒充"没开" */
  w = fakeWx({ openFail: { errCode: 10001, errMsg: 'openBluetoothAdapter:fail' } })
  err = await bleMod.scan(30).then(() => null, fail)
  assert.ok(/^蓝牙没开/.test(err.message), err.message)
  assert.deepStrictEqual(w._calls, ['on', 'open', 'off', 'stop'], '失败也要清干净')

  w = fakeWx({ openFail: { errCode: 10008, errMsg: 'openBluetoothAdapter:fail system error' } })
  err = await bleMod.scan(30).then(() => null, fail)
  assert.ok(/10008/.test(err.message) && !/没开/.test(err.message), err.message)

  w = fakeWx({ startFail: { errCode: 10008, errMsg: 'startBluetoothDevicesDiscovery:fail' } })
  err = await bleMod.scan(30).then(() => null, fail)
  assert.ok(/扫描失败（errCode 10008）/.test(err.message), err.message)
  assert.deepStrictEqual(w._calls, ['on', 'open', 'start', 'off', 'stop'])

  console.log('  扫描报错文案通过（模拟器 / 排序与识别 / 10001 / 10008 / 出口清理）')
})
