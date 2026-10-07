const ble = require('../../utils/ble')

const INTERVALS = [5, 10, 15, 30, 60]
const MAX = 8
const LOOKUP_MS = 400        // 敲完到发请求的等待：连着改几位不用每个中间态都查一次

/* 名称来源：腾讯分时接口的响应里自带名称（qt 数组第 1 项，中文是 \uXXXX 转义 ——
   JSON.parse 之后就是中文，**没有 GBK 问题**）。板子第 2/3 页用的就是这两个主机；
   某些网络里 web.ifzq 连不上而 proxy.finance 可以，所以留一份回退（和固件一致）。
   注意：不走板子。配网模式下 NimBLE 占掉内部 RAM，板子那边 TLS 起不来
   （firmware/README §9），所以名称只能在手机侧取。 */
const NAME_HOSTS = [
  'https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=',
  'https://proxy.finance.qq.com/ifzqgtimg/appstock/app/minute/query?code=',
]

let lookupTimer = null

/* 6 位纯代码 → 市场前缀（按首位）：6/9 沪（60 主板、68 科创、900 沪 B）、
   0/2/3 深（00 主板、30 创业、200 深 B）、4/8 北交所。 */
function prefixOf(digits) {
  const c = digits[0]
  if (c === '6' || c === '9') return 'sh'
  if (c === '0' || c === '2' || c === '3') return 'sz'
  if (c === '4' || c === '8') return 'bj'
  return ''
}

/* 用户可能只敲 6 位数字，也可能敲全 sh600519；统一成 "<sh|sz|bj> + 6 位"。
   认不出来返回 ''（半截代码、乱敲都算认不出来）。 */
function normCode(v) {
  const s = String(v == null ? '' : v).trim().toLowerCase().replace(/\s+/g, '')
  if (/^(sh|sz|bj)\d{6}$/.test(s)) return s
  if (/^\d{6}$/.test(s)) {
    const p = prefixOf(s)
    return p ? p + s : ''
  }
  return ''
}

/* 服务端把短名字**用空格补到固定宽度**："五粮液" → "五 粮 液"、"万科Ａ" → "万  科Ａ"
   （实测：五粮液 3 个字补 2 个空格、万科Ａ 补 2 个空格，凑到 8 个显示宽度）。
   还要把全角 Ａ-Ｚ/０-９ 折成半角：板子的字库范围是 ASCII + CJK（tools/gen_fonts.ps1），
   全角字母没有字形，留在名字里板子上就是一块空白。 */
function cleanName(s) {
  return String(s == null ? '' : s)
    .replace(/\s+/g, '')
    .replace(/[\uff01-\uff5e]/g, (c) => String.fromCharCode(c.charCodeAt(0) - 0xfee0))
}

/* 取名称。查不到/网络不通一律 reject，调用方决定怎么提示（不挡下发） */
function requestName(host, code) {
  const url = host + code
  return new Promise((resolve, reject) => {
    wx.request({
      url,
      timeout: 5000,
      success: (res) => {
        const d = res && res.data && res.data.data && res.data.data[code]
        const qt = d && d.qt && d.qt[code]
        const name = cleanName(qt && qt[1])
        if (res.statusCode === 200 && name) {
          resolve(name)
        } else {
          reject(new Error('接口里没有这个代码'))
        }
      },
      fail: (e) => reject(new Error((e && e.errMsg) || '请求失败')),
    })
  })
}

function fetchName(code) {
  return requestName(NAME_HOSTS[0], code)
    .catch(() => requestName(NAME_HOSTS[1], code))
}

/* 域名白名单是最常见的失败原因，单独给一句人话（详见 miniprogram/README） */
function hintOf(err) {
  const m = (err && err.message) || '取名称失败'
  if (/domain|not in|白名单/i.test(m)) {
    return '域名不在白名单里，去小程序后台加 request 合法域名'
  }
  return m
}

Page({
  data: { items: [], interval: 15, idx: INTERVALS.indexOf(15), intervals: INTERVALS, tip: '', ok: '', dup: '' },

  /* 给 selftest 直接调（纯函数，不碰 wx） */
  normCode,
  cleanName,

  onShow() {
    if (ble.connected()) this.load()
    else this.setData({ tip: '还没连上板子，先去「设备」页连一下' })
  },

  /* 从板子读回当前配置（也是"板子现在到底记着什么"的唯一真相） */
  load() {
    ble.send({ cmd: 'getCfg' }).then((m) => {
      if (m.ev !== 'cfg') return
      this.setData({
        items: m.items.map((x) => ({ code: x.code, name: x.name, mark: x.mark })),
        interval: m.interval,
        idx: Math.max(0, INTERVALS.indexOf(m.interval)),
        tip: '', ok: '',
      })
    }).catch((e) => this.setData({ tip: e.message }))
  },

  onField(e) {
    const { i, k } = e.currentTarget.dataset
    const v = e.detail.value
    if (k === 'code') {
      const code = normCode(v)
      /* 只在真的补了前缀时才回写，免得每敲一下都 setData 同一个框（会跟光标打架） */
      if (code && code !== v) this.setData({ ['items[' + i + '].code']: code, ok: '' })
      else this.setData({ ok: '' })
      if (code) this.scheduleLookup(code)
      return
    }
    if (k === 'name') {
      /* 用户自己动过名字：以后不再被自动查询覆盖 */
      return this.setData({ ['items[' + i + '].name']: v, ['items[' + i + '].auto']: false, ok: '' })
    }
    this.setData({ ['items[' + i + '].' + k]: v, ok: '' })
  },

  scheduleLookup(code) {
    clearTimeout(lookupTimer)
    lookupTimer = setTimeout(() => this.lookupName(code), LOOKUP_MS)
  },

  lookupName(code) {
    return fetchName(code).then((name) => {
      if (name && this.applyName(code, name)) this.setData({ tip: '' })
    }).catch((err) => {
      if (this.needsName(code)) {
        this.setData({ tip: '没取到「' + code + '」的名称（' + hintOf(err) + '），手动填一下' })
      }
    })
  },

  /* 这一行还缺名字吗（空着，或上次就是自动填的） */
  needsName(code) {
    return this.data.items.some((x) => x.code === code && (!x.name || x.auto))
  },

  /* 落到所有"同代码、且名字空着或上次也是自动填的"的行上 —— 手填过的名字不动 */
  applyName(code, name) {
    const patch = {}
    let hit = 0
    this.data.items.forEach((x, i) => {
      if (x.code !== code || (x.name && !x.auto)) return
      patch['items[' + i + '].name'] = name
      patch['items[' + i + '].auto'] = true
      hit++
    })
    if (hit) this.setData(patch)
    return hit
  },

  onAdd() {
    if (this.data.items.length >= MAX) return this.setData({ tip: '最多 ' + MAX + ' 只' })
    this.setData({ items: this.data.items.concat([{ code: '', name: '', mark: '' }]), tip: '' })
  },

  onDel(e) {
    const items = this.data.items.slice()
    items.splice(e.currentTarget.dataset.i, 1)
    this.setData({ items, ok: '' })
  },

  onMove(e) {
    const { i, d } = e.currentTarget.dataset
    const j = i + Number(d)
    if (j < 0 || j >= this.data.items.length) return
    const items = this.data.items.slice()
    const t = items[i]; items[i] = items[j]; items[j] = t
    this.setData({ items, ok: '' })
  },

  onInterval(e) {
    const idx = Number(e.detail.value)
    this.setData({ idx, interval: INTERVALS[idx], ok: '' })
  },

  onSave() {
    const items = this.data.items
      .filter((x) => x.code.trim() && x.name.trim())
      .map((x) => {
        const o = { code: x.code.trim(), name: x.name.trim() }
        if (x.mark && x.mark.trim()) o.mark = x.mark.trim()   // 不给就由板子取名字第一个字
        return o
      })
    if (!items.length) return this.setData({ tip: '至少要有 1 只股票' })

    wx.showLoading({ title: '下发中' })
    ble.send({ cmd: 'setStocks', items }).then((m) => {
      if (m.ev === 'err') {
        wx.hideLoading()
        return this.setData({ tip: '板子拒绝：' + m.code + ' ' + m.msg })
      }
      return ble.send({ cmd: 'setInterval', sec: this.data.interval }).then(() => {
        wx.hideLoading()
        this.setData({ ok: '已下发 ' + items.length + ' 只，刷新 ' + this.data.interval + ' 秒' })
        this.load()          // 读回板子实际存的（首字是板子补的，这里能看到）
      })
    }).catch((e) => {
      wx.hideLoading()
      this.setData({ tip: e.message })
    })
  },

  onReload() { this.load() },
})
