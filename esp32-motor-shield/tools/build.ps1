<#
.SYNOPSIS
    用本机那套便携版 Arduino 编译 / 上传 esp32-motor-shield 的两个固件。

.DESCRIPTION
    本机 Arduino 装在一个便携目录里, arduino-cli 也藏在 IDE 内部, 不在 PATH 上,
    所以这里把它固定下来, 你只关心 -Sketch 和 -Fqbn 就行。

    为什么必须指定 --config-file:
        Arduino 的数据目录被搬到了 D:\Download\arduino\data (C 盘空间不够),
        默认路径下找不到 esp32 核心。配置里还有 board_manager 的 file:// 索引。

.EXAMPLE
    .\build.ps1 -Sketch MotorLinkCheck                # 只编译, 验证代码
    .\build.ps1 -Sketch MotorLinkCheck -Upload        # 编译并烧录(需要插上板子)
    .\build.ps1 -Sketch MotorDriver -Fqbn esp32:esp32:esp32doit-devkit-v1
    .\build.ps1 -ListBoards                           # 看看当前插着哪个串口
#>
[CmdletBinding()]
param(
    [ValidateSet('MotorLinkCheck', 'MotorDriver', 'All')]
    [string]$Sketch = 'All',

    [string]$Fqbn = 'esp32:esp32:esp32',

    [string]$Port,

    [switch]$Upload,

    [switch]$ListBoards,

    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'

$ArduinoHome = 'D:\Download\arduino'
$Cli         = Join-Path $ArduinoHome 'resources\app\lib\backend\resources\arduino-cli.exe'
$ConfigFile  = Join-Path $ArduinoHome 'data\arduino-cli.yaml'
$FirmwareDir = Join-Path $PSScriptRoot '..\firmware'

if (-not (Test-Path $Cli))        { throw "找不到 arduino-cli: $Cli" }
if (-not (Test-Path $ConfigFile)) { throw "找不到 arduino-cli 配置: $ConfigFile" }

function Invoke-Cli {
    param([string[]]$CliArgs)
    & $Cli @CliArgs --config-file $ConfigFile
    if ($LASTEXITCODE -ne 0) { throw "arduino-cli 执行失败 (退出码 $LASTEXITCODE): $($CliArgs -join ' ')" }
}

if ($ListBoards) {
    Write-Host "=== 已安装的核心 ===" -ForegroundColor Cyan
    Invoke-Cli @('core', 'list')
    Write-Host "`n=== 当前插着的板子 ===" -ForegroundColor Cyan
    Invoke-Cli @('board', 'list')
    Write-Host "`n常见 ESP32 FQBN:" -ForegroundColor Cyan
    Write-Host "  esp32:esp32:esp32                ESP32 Dev Module  (最常用, 默认)"
    Write-Host "  esp32:esp32:esp32doit-devkit-v1  DOIT ESP32 DEVKIT V1"
    Write-Host "  esp32:esp32:esp32s3              ESP32-S3 Dev Module"
    Write-Host "  esp32:esp32:esp32c3              ESP32-C3 Dev Module"
    Write-Host "  提示: 用 'arduino-cli board listall esp32' 列出全部"
    return
}

$targets = if ($Sketch -eq 'All') { @('MotorLinkCheck', 'MotorDriver') } else { @($Sketch) }

# 没指定串口就自动挑一个像样的: 优先 CH340 / CP210x / USB-SERIAL
if ($Upload -and -not $Port) {
    $ports = & $Cli board list --config-file $ConfigFile --format json | ConvertFrom-Json
    $detected = $ports.detected_ports |
        Where-Object { $_.port.protocol -eq 'serial' } |
        Where-Object { $_.matching_boards -or $_.port.properties.pid -match '7523|ea60|55d4|1001' }

    if (-not $detected) {
        $detected = $ports.detected_ports | Where-Object { $_.port.protocol -eq 'serial' }
    }
    if (-not $detected) {
        throw "没有找到任何串口。板子插上了吗? 驱动装了吗 (CH341SER / CP210x)? 运行 -ListBoards 看看。"
    }
    $Port = @($detected)[0].port.address
    Write-Host "自动选择串口: $Port" -ForegroundColor Yellow
}

$results = @()
foreach ($t in $targets) {
    $path = Join-Path $FirmwareDir $t
    if (-not (Test-Path $path)) { throw "找不到 sketch 目录: $path" }

    Write-Host "`n=========== 编译 $t  ($Fqbn) ===========" -ForegroundColor Cyan
    $cliArgs = @('compile', '--fqbn', $Fqbn, '--warnings', 'default', $path)
    if ($Upload) { $cliArgs += @('--upload', '--port', $Port) }
    Invoke-Cli $cliArgs

    $results += [pscustomobject]@{ Sketch = $t; Fqbn = $Fqbn; Uploaded = [bool]$Upload }
}

Write-Host "`n=========== 结果 ===========" -ForegroundColor Green
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

if ($Upload) {
    Write-Host "烧录完成。下一步:" -ForegroundColor Green
    Write-Host "  MotorLinkCheck -> 打开串口监视器(115200), 看自检报告; 或运行 host\motor_link_check.py"
    Write-Host "  MotorDriver    -> 运行 host\motor_link_check.py --drive 1 200 试转"
} elseif ($Monitor) {
    Invoke-Cli @('monitor', '--port', $Port, '--config', 'baudrate=115200')
}
