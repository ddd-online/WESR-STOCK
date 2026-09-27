param(
    # 默认用 Noto Sans SC：单体 TTF（lv_font_conv 不支持 .ttc 集合）、OFL 许可可再分发
    [string]$FontPath = 'C:\Windows\Fonts\NotoSansSC-VF.ttf'
)
# 生成 LVGL 位图字体（构建产物，不入 git —— 见 .gitignore）
# 依赖：node/npx（会用 npx 临时拉 lv_font_conv）
#   - font_cn16.c   ：全 CJK 区段 16px，用于股票名/首字/中文标签
#   - font_numN.c   ：数字与符号各字号，用于时间、价格、涨跌幅
# 用法：powershell -File firmware/tools/gen_fonts.ps1 [-FontPath C:\Windows\Fonts\simhei.ttf]
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot '..\components\wesr\ui'
$sys = $FontPath
if (-not (Test-Path $sys)) { throw "找不到字体：$sys（可换 C:\Windows\Fonts\simhei.ttf）" }

# 用码点范围而不是字面符号：PowerShell 传多字节字符容易被代码页搞坏
#   0x20-0x7F ASCII（数字、. : % + - 等）
#   0x25B2 ▲ / 0x25BC ▼ / 0x2014 — / 0xB7 · / 0xB0 °
$punct    = '0x25B2,0x25BC,0x2014,0xB7,0xB0,0x2026,0x2192,0x2018-0x201D'
$numRange = "0x20-0x7F,$punct"
# 中文这一档也带上 ASCII：日期/编号这类混排（"9月27日"、"3/8"）要用到数字
$cnRange  = "0x20-0x7F,0x4E00-0x9FA5,$punct,0x3000,0x3001-0x3002,0xFF01,0xFF08,0xFF09,0xFF0C,0xFF1A,0xFF1B,0xFF1F"

foreach ($sz in 78, 25, 19, 16, 14, 12, 11, 9) {
    Write-Host "生成 font_num$sz.c ..."
    npx --yes lv_font_conv --font $sys --size $sz --bpp 1 --no-compress `
        -r $numRange --format lvgl --lv-include lvgl.h `
        -o (Join-Path $out "font_num$sz.c") --force-fast-kern-format
}

Write-Host "生成 font_cn16.c（全 CJK 区段，约 2.1 万字，会慢一分钟）..."
npx --yes lv_font_conv --font $sys --size 16 --bpp 1 --no-compress `
    -r $cnRange --format lvgl --lv-include lvgl.h `
    -o (Join-Path $out 'font_cn16.c') --force-fast-kern-format

Write-Host "生成 font_cn12.c（状态条、昨收、开高低收量这类小字用）..."
npx --yes lv_font_conv --font $sys --size 12 --bpp 1 --no-compress `
    -r $cnRange --format lvgl --lv-include lvgl.h `
    -o (Join-Path $out 'font_cn12.c') --force-fast-kern-format

Write-Host "完成。"
