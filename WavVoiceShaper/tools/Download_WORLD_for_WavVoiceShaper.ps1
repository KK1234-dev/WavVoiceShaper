# WavVoiceShaper用 WORLD 取得スクリプト
# 実行場所: WavVoiceShaper\WavVoiceShaper\tools から実行、または右クリック「PowerShellで実行」
# 取得元: https://github.com/mmorise/World

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Split-Path -Parent $ScriptDir
$ThirdPartyDir = Join-Path $ProjectDir "third_party\world"
$TempDir = Join-Path $env:TEMP ("WavVoiceShaper_WORLD_" + [guid]::NewGuid().ToString("N"))
$ZipPath = Join-Path $TempDir "World-master.zip"
$Url = "https://github.com/mmorise/World/archive/refs/heads/master.zip"

Write-Host "WORLDを取得します: $Url"
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
New-Item -ItemType Directory -Force -Path $ThirdPartyDir | Out-Null

Invoke-WebRequest -Uri $Url -OutFile $ZipPath
Expand-Archive -Path $ZipPath -DestinationPath $TempDir -Force

$Extracted = Join-Path $TempDir "World-master"
$SourceSrc = Join-Path $Extracted "src"
$DestSrc = Join-Path $ThirdPartyDir "src"

if (!(Test-Path $SourceSrc)) {
    throw "WORLDのsrcフォルダが見つかりません: $SourceSrc"
}

if (Test-Path $DestSrc) {
    Remove-Item $DestSrc -Recurse -Force
}
Copy-Item $SourceSrc $DestSrc -Recurse -Force

$LicenseCandidates = @("LICENSE", "LICENSE.txt")
foreach ($name in $LicenseCandidates) {
    $src = Join-Path $Extracted $name
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $ThirdPartyDir $name) -Force
    }
}

Remove-Item $TempDir -Recurse -Force

Write-Host "完了しました。配置先: $DestSrc"
Write-Host "Visual StudioでWavVoiceShaper.vcxprojを開いてビルドしてください。"
