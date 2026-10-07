const ble = require('../../utils/ble')
const app = getApp()

Page({
  data: { devices: [], scanning: false, deviceId: '', status: null, tip: '' },

  onShow() {
    /* 板子订阅成功会主动推一条 status，这里收下来直接显示 */
    ble.setListener((m) => {
      if (m.ev === 'status') {
        app.globalData.status = m
        this.setData({ status: m })
      }
    })
    this.setData({ deviceId: app.globalData.deviceId, status: app.globalData.status })
  },

  onScan() {
    this.setData({ scanning: true, tip: '' })
    ble.scan(6000).then((list) => {
      this.setData({
        devices: list,
        scanning: false,
        tip: list.length ? '' : '没扫到 WESR-STOCK：先长按 KEY 3 秒让板子进配网模式（屏幕底部状态条反白），再扫。板子 3 分钟没操作会自己退出配网',
      })
    }).catch((e) => this.setData({ scanning: false, tip: e.message }))
  },

  onConnect(e) {
    const id = e.currentTarget.dataset.id
    wx.showLoading({ title: '连接中' })
    ble.connect(id).then(() => {
      app.globalData.deviceId = id
      this.setData({ deviceId: id, devices: [], tip: '' })
      wx.hideLoading()
    }).catch((err) => {
      wx.hideLoading()
      this.setData({ tip: err.message })
    })
  },

  onRefresh() {
    ble.send({ cmd: 'hello' }).catch((e) => this.setData({ tip: e.message }))
  },

  onDisconnect() {
    ble.disconnect()
    app.globalData.deviceId = ''
    app.globalData.status = null
    this.setData({ deviceId: '', status: null, tip: '' })
  },

  /* 板子 3 分钟没操作也会自己退出；这个按钮是给"我现在就想关机走人"用的 */
  onExitPairing() {
    ble.send({ cmd: 'exit' }).then(() => {
      ble.disconnect()
      app.globalData.deviceId = ''
      app.globalData.status = null
      this.setData({ deviceId: '', status: null, tip: '已让板子退出配网模式' })
    }).catch((e) => this.setData({ tip: e.message }))
  },
})
