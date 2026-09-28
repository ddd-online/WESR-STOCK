const ble = require('../../utils/ble')
const app = getApp()

Page({
  data: { aps: [], scanning: false, ssid: '', pass: '', tip: '', ok: '' },

  onShow() {
    this.setData({ tip: ble.connected() ? '' : '还没连上板子，先去「设备」页连一下' })
  },

  onScan() {
    this.setData({ scanning: true, tip: '', ok: '' })
    /* 板子要断开重连 WiFi 才能扫，所以给 15 秒 */
    ble.send({ cmd: 'scan' }, 15000).then((m) => {
      if (m.ev !== 'scanResult') return this.setData({ scanning: false, tip: '板子回了：' + m.ev })
      this.setData({ aps: m.aps || [], scanning: false })
    }).catch((e) => this.setData({ scanning: false, tip: e.message }))
  },

  onPick(e) { this.setData({ ssid: e.currentTarget.dataset.ssid, tip: '' }) },
  onSsid(e) { this.setData({ ssid: e.detail.value }) },
  onPass(e) { this.setData({ pass: e.detail.value }) },

  onSave() {
    const { ssid, pass } = this.data
    if (!ssid) return this.setData({ tip: '先点一个网络，或手填 SSID' })
    wx.showLoading({ title: '连接中，最多 20 秒' })
    /* 板子要真的去连，20 秒超时；这里给 30 秒 */
    ble.send({ cmd: 'setWifi', ssid, pass }, 30000).then((m) => {
      if (m.ev === 'err') {
        wx.hideLoading()
        return this.setData({
          tip: m.code === 'E_WIFI' ? '连不上：密码错或路由器太远（板子还在配网模式，可以重试）'
                                   : '板子拒绝：' + m.code + ' ' + m.msg,
        })
      }
      /* 连上了，再问一次 state 拿新 IP */
      return ble.send({ cmd: 'hello' }).then((s) => {
        wx.hideLoading()
        app.globalData.status = s
        this.setData({ ok: '已连上，板子 IP：' + s.ip + '（' + s.rssi + ' dBm）', tip: '' })
      })
    }).catch((e) => {
      wx.hideLoading()
      this.setData({ tip: e.message })
    })
  },
})
