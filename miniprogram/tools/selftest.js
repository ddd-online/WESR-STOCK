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
