const ble = require('../../utils/ble')

const INTERVALS = [5, 10, 15, 30, 60]
const MAX = 8

Page({
  data: { items: [], interval: 15, idx: INTERVALS.indexOf(15), intervals: INTERVALS, tip: '', ok: '', dup: '' },

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
    this.setData({ ['items[' + i + '].' + k]: e.detail.value, ok: '' })
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
