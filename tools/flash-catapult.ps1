<#
.SYNOPSIS
    ESP32 + 2804 无刷电机项目 —— 一键检测串口 / 编译 / 烧录 / 验证

.DESCRIPTION
    这个脚本把「板子在哪个串口 -> 是不是真的 ESP32 -> 该用哪个 FQBN ->
    编译 -> 烧录 -> 烧完读串口验证」整条链子串起来, 并且每一步都会校验,
    不靠猜。

    为什么要自己检测芯片:
        接线照片上写的是 ESP32-S3, 但插在电脑上的这块实测是
        ESP32-D0WD-V3 (即 ESP32-WROOM-32E)。选错 FQBN 会烧出跑不起来的固件。
        所以本脚本先问 esptool 要答案, 再用答案决定编译目标。

.EXAMPLE
    .\flash-catapult.ps1 -List                 # 只看串口和芯片, 什么都不烧
    .\flash-catapult.ps1                       # 烧第一阶段 AS5600 测试 (默认)
    .\flash-catapult.ps1 -Stage 2              # 烧第二阶段 FOC 闭环测试
    .\flash-catapult.ps1 -CompileOnly          # 只编译不烧录
    .\flash-catapult.ps1 -SkipVerify           # 烧完不去读串口
    .\flash-catapult.ps1 -UploadSpeed 115200   # 上传老是失败时降速重试
#>
[CmdletBinding()]
param(
    [ValidateSet('1', '2')]
    [string]$Stage = '1',

    [string]$Port,
    [string]$Fqbn,

    [switch]$List,
    [switch]$CompileOnly,
    [switch]$SkipVerify,

    [ValidateSet('921600', '460800', '230400', '115200')]
    [string]$UploadSpeed,

    [int]$VerifySeconds = 10
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# 固定路径 (本机 Arduino 是便携安装, 都不在 PATH 上)
# ---------------------------------------------------------------------------
$ArduinoHome = 'D:\Download\arduino'
$Cli         = Join-Path $ArduinoHome 'resources\app\lib\backend\resources\arduino-cli.exe'
$ConfigFile  = Join-Path $ArduinoHome 'data\arduino-cli.yaml'
$Esptool     = Join-Path $ArduinoHome 'data\packages\esp32\tools\esptool_py\5.3.1\esptool.exe'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$FirmwareDir = Join-Path $ProjectRoot 'firmware\catapult'
$SerialWatch = Join-Path $ProjectRoot 'host\serial_watch.py'

$SketchName = if ($Stage -eq '1') { 'Stage1_AS5600Test' } else { 'Stage2_FOCClosedLoop' }
$SketchPath = Join-Path $FirmwareDir $SketchName

foreach ($p in @($Cli, $ConfigFile, $Esptool)) {
    if (-not (Test-Path $p)) { throw "找不到必需的程序: $p" }
}

# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------
function Invoke-Esptool {
    param([string]$Command, [string]$OnPort)
    & $Esptool --port $OnPort $Command 2>&1
}

function Get-ChipInfo {
    <# 返回 @{ Ok; Chip; FlashMB; Mac; Raw } #>
    param([string]$OnPort)

    $out = Invoke-Esptool -Command 'flash-id' -OnPort $OnPort
    $text = ($out | Out-String)
    $chip = if ($text -match 'Chip type:\s*(.+)') { $Matches[1].Trim() } else { $null }
    $mb   = if ($text -match 'Detected flash size:\s*(\d+)MB') { [int]$Matches[1] } else { 0 }
    $mac  = if ($text -match '(?m)^MAC:\s*(\S+)') { $Matches[1] } else { $null }
    return [pscustomobject]@{
        Ok      = [bool]$chip
        Chip    = $chip
        FlashMB = $mb
        Mac     = $mac
        Raw     = $text
    }
}

function Get-PortFriendlyName {
    <# 给 COMx 找一个人类可读的名字, 用来识别"这是蓝牙串口, 不是开发板" #>
    param([string]$ComName)
    try {
        $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like "*($ComName)*" } |
               Select-Object -First 1
        if ($dev) { return $dev.Name }
    } catch { }
    return ''
}

function Get-SerialCandidates {
    $json = & $Cli board list --config-file $ConfigFile --format json | ConvertFrom-Json
    $json.detected_ports |
        Where-Object { $_.port.protocol -eq 'serial' } |
        ForEach-Object {
            # 注意: 不能用 $pid 当变量名 ($pid 是 PowerShell 只读自动变量)
            # arduino-cli 给的 VID/PID 形如 "0x1A86" / "0x7523", 要先去掉 0x 前缀,
            # 否则下面的正则永远匹配不上 —— 这正是最初误选到蓝牙串口的原因。
            $usbVid = "$($_.port.properties.vid)"
            $usbPid = "$($_.port.properties.pid)"
            $usbId  = ("$usbVid`:$usbPid").ToLower() -replace '0x', ''
            if ($usbId -eq ':' -or $usbId -eq '') { $usbId = '' }

            $friendly = Get-PortFriendlyName -ComName $_.port.address
            $isBt = $friendly -match 'BTHENUM|Bluetooth|蓝牙'

            $score = 0
            if     ($usbId -match '^1a86:7523$')                    { $score = 100 }  # CH340
            elseif ($usbId -match '^1a86:55d4$|^1a86:55d3$')         { $score = 90 }   # CH9102 / CH343
            elseif ($usbId -match '^10c4:ea60$|^10c4:ea70$')         { $score = 90 }   # CP210x
            elseif ($usbId -match '^303a:')                          { $score = 95 }   # 乐鑫原生 USB
            elseif ($usbId -match '^0403:')                          { $score = 70 }   # FTDI
            if ($_.matching_boards) { $score = [Math]::Max($score, 60) }
            if ($isBt) { $score = -100 }   # 蓝牙虚拟串口一定不是开发板

            [pscustomobject]@{
                Address  = $_.port.address
                Label    = $_.port.label
                Friendly = $friendly
                UsbId    = $usbId
                IsBt     = $isBt
                Score    = $score
            }
        } | Sort-Object Score -Descending
}

function Get-FqbnForChip {
    param([string]$Chip)
    if (-not $Chip) { return 'esp32:esp32:esp32' }
    switch -Regex ($Chip) {
        'ESP32-S3'  { return 'esp32:esp32:esp32s3' }
        'ESP32-S2'  { return 'esp32:esp32:esp32s2' }
        'ESP32-C3'  { return 'esp32:esp32:esp32c3' }
        'ESP32-C6'  { return 'esp32:esp32:esp32c6' }
        'ESP32-P4'  { return 'esp32:esp32:esp32p4' }
        'ESP32'     { return 'esp32:esp32:esp32' }   # 经典款含 D0WD-V3 / WROOM-32E
        default     { return 'esp32:esp32:esp32' }
    }
}

function Write-Head {
    param([string]$Text)
    Write-Host ''
    Write-Host ('=' * 64) -ForegroundColor DarkCyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host ('=' * 64) -ForegroundColor DarkCyan
}

# ---------------------------------------------------------------------------
# 1. 列串口 (只读, 不动板子)
# ---------------------------------------------------------------------------
if ($List) {
    Write-Head '串口与芯片检测'
    $cands = @(Get-SerialCandidates)
    if (-not $cands) {
        Write-Host '没有检测到任何串口。' -ForegroundColor Yellow
        Write-Host '  -> 板子插上了吗? 数据线是不是只能充电的那种? CH340 驱动装了吗?'
        return
    }
    foreach ($c in $cands) {
        Write-Host ''
        Write-Host ("串口 {0}" -f $c.Address) -ForegroundColor White
        if ($c.Friendly) { Write-Host ("  设备   : {0}" -f $c.Friendly) }
        if ($c.UsbId)    { Write-Host ("  USB ID : {0}" -f $c.UsbId) }
        if ($c.IsBt) {
            Write-Host '  判断   : 蓝牙虚拟串口, 不是开发板, 已排除' -ForegroundColor DarkGray
            continue
        }
        Write-Host '  正在问芯片型号 ...'
        $info = Get-ChipInfo -OnPort $c.Address
        if ($info.Ok) {
            Write-Host ("  芯片   : {0}" -f $info.Chip) -ForegroundColor Green
            Write-Host ("  Flash  : {0} MB" -f $info.FlashMB)
            Write-Host ("  MAC    : {0}" -f $info.Mac)
            Write-Host ("  建议FQBN: {0}" -f (Get-FqbnForChip $info.Chip)) -ForegroundColor Green
        } else {
            Write-Host '  芯片   : 无应答 (可能不是 ESP32, 或串口被占用)' -ForegroundColor Yellow
        }
    }
    Write-Host ''
    Write-Host '提示: 如果某个串口被占用, 先关掉 Arduino IDE 的串口监视器。' -ForegroundColor DarkGray
    return
}

# ---------------------------------------------------------------------------
# 2. 选串口
# ---------------------------------------------------------------------------
Write-Head "烧录 $SketchName"

if (-not (Test-Path $SketchPath)) { throw "找不到固件目录: $SketchPath" }

if (-not $Port) {
    $cands = @(Get-SerialCandidates)
    if (-not $cands) {
        throw '没有找到串口。板子插上了吗? 驱动装了吗? 可以用 -List 排查。'
    }

    $best = $cands[0]
    # 只有认得出 USB 转串口芯片的串口才敢自动选 —— 否则宁可报错让人来定
    if ($best.Score -lt 50) {
        Write-Host '没有找到明确的开发板串口, 不敢乱猜。当前串口:' -ForegroundColor Yellow
        foreach ($c in $cands) {
            $name = if ($c.Friendly) { $c.Friendly } else { '(无描述)' }
            Write-Host ("  {0,-6} 得分={1,-5} {2}" -f $c.Address, $c.Score, $name)
        }
        throw "请用 -Port COMx 手动指定要烧录的串口 (例如 .\flash-catapult.ps1 -Stage $Stage -Port COM9)"
    }

    $Port = $best.Address
    $desc = if ($best.Friendly) { $best.Friendly } else { $best.Label }
    Write-Host ("自动选择串口: {0}   {1}" -f $Port, $desc) -ForegroundColor Yellow
    Write-Host ("  识别依据: USB ID {0}" -f $best.UsbId) -ForegroundColor DarkGray
    $others = @($cands | Select-Object -Skip 1)
    if ($others.Count -gt 0) {
        Write-Host ("  其它候选: " + (($others | ForEach-Object { $_.Address }) -join ', ')) -ForegroundColor DarkGray
    }
} else {
    Write-Host "使用指定串口: $Port"
}

# ---------------------------------------------------------------------------
# 3. 问芯片 (这一步会复位板子, 属正常现象)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '正在读取芯片信息 ...'
$info = Get-ChipInfo -OnPort $Port
if (-not $info.Ok) {
    Write-Host $info.Raw -ForegroundColor DarkGray
    throw "无法与 $Port 上的芯片通信。请确认: 1) 板子电源正常 2) 串口没被占用 3) 数据线完好"
}

Write-Host ("  芯片  : {0}" -f $info.Chip) -ForegroundColor Green
Write-Host ("  Flash : {0} MB" -f $info.FlashMB)
Write-Host ("  MAC   : {0}" -f $info.Mac)

if (-not $Fqbn) {
    $Fqbn = Get-FqbnForChip $info.Chip
    Write-Host ("  自动选定 FQBN: {0}" -f $Fqbn) -ForegroundColor Green
} else {
    Write-Host ("  使用指定 FQBN: {0}" -f $Fqbn) -ForegroundColor Yellow
}

# 接线照片写的是 ESP32-S3, 实际是经典 ESP32 —— 这里主动提示一次
if ($info.Chip -notmatch 'S3' ) {
    Write-Host ''
    Write-Host '  注意: 接线照片上标注的是 ESP32-S3, 但实测这块是经典 ESP32。' -ForegroundColor Yellow
    Write-Host '        已按实测芯片选择编译目标 (这一步很关键, 选错会烧出跑不起来的固件)。' -ForegroundColor Yellow
}

$effectiveFqbn = $Fqbn
if ($UploadSpeed) {
    $effectiveFqbn = "$Fqbn`:UploadSpeed=$UploadSpeed"
    Write-Host ("  上传波特率指定为: {0}" -f $UploadSpeed) -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 4. 编译 (+ 烧录)
# ---------------------------------------------------------------------------
Write-Head "编译 $SketchName"

$compileArgs = @('compile', '--config-file', $ConfigFile,
                 '--fqbn', $effectiveFqbn,
                 '--warnings', 'default',
                 $SketchPath)

if (-not $CompileOnly) {
    $compileArgs += @('--upload', '--port', $Port)
}

& $Cli @compileArgs
if ($LASTEXITCODE -ne 0) {
    if (-not $CompileOnly) {
        Write-Host ''
        Write-Host '编译或烧录失败。如果错误发生在下载阶段(而不是编译阶段),' -ForegroundColor Yellow
        Write-Host '可以试试降速:  .\flash-catapult.ps1 -Stage ' -NoNewline -ForegroundColor Yellow
        Write-Host $Stage -NoNewline -ForegroundColor Yellow
        Write-Host ' -UploadSpeed 115200' -ForegroundColor Yellow
    }
    throw "arduino-cli 执行失败 (退出码 $LASTEXITCODE)"
}

if ($CompileOnly) {
    Write-Host ''
    Write-Host '只编译不烧录 —— 完成。' -ForegroundColor Green
    return
}

Write-Host ''
Write-Host '烧录成功。' -ForegroundColor Green

# ---------------------------------------------------------------------------
# 5. 烧完读串口验证
# ---------------------------------------------------------------------------
if ($SkipVerify) {
    Write-Host '已跳过串口验证 (-SkipVerify)。'
    return
}

Write-Head '串口验证 (复位后读取板子输出)'

if (-not (Test-Path $SerialWatch)) {
    Write-Host "找不到 $SerialWatch, 跳过验证。" -ForegroundColor Yellow
    return
}

Write-Host "读取 $VerifySeconds 秒 ..." -ForegroundColor DarkGray
Write-Host ''

if ($Stage -eq '1') {
    python $SerialWatch --port $Port --seconds $VerifySeconds
} else {
    # 第二阶段需要人工输入 c 才校准, 这里只发一个 c 之前先不碰它,
    # 保持"只观察"最安全: 只读不发送。
    python $SerialWatch --port $Port --seconds $VerifySeconds
}

Write-Host ''
Write-Head '完成'
if ($Stage -eq '1') {
    Write-Host '第一阶段验证要点 (guide 第 4 节):'
    Write-Host '  - 手转一圈, raw 应覆盖约 0~4095 并正常跨零回绕'
    Write-Host '  - 转子不动时读数基本稳定'
    Write-Host '  - MD=1 表示检测到磁铁; ML=1 磁场偏弱; MH=1 磁场偏强'
    Write-Host '  这些都正常后, 再考虑第二阶段。'
} else {
    Write-Host '第二阶段说明:'
    Write-Host '  源码里 CONFIG_CONFIRMED 默认是 false, 所以现在烧进去是【安全锁定】状态,'
    Write-Host '  只会打印 LOCKED, 电机不会动。'
    Write-Host '  等你核对完接线、EN 极性、实测母线电压之后, 把源码里'
    Write-Host '    constexpr bool CONFIG_CONFIRMED = false;'
    Write-Host '  改成 true 并重新烧录, 才会允许校准和试转。'
}
