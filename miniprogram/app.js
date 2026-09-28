App({
  globalData: {
    deviceId: '',      // 当前连着的板子（三个页面共用一条 BLE 连接）
    status: null,      // 板子最近一次推/回的 status，配网页拿它显示 IP
  },
})
