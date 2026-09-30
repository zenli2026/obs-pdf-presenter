# ppt2pdf.ps1 —— 调用本机 PowerPoint 把 PPT/PPTX 转成 PDF
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File ppt2pdf.ps1 -InputPath "x.pptx" -OutputPath "x.pdf"
param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference = "Stop"
# 错误日志（供插件排障）
$logDir = Join-Path $env:TEMP "obs_pdf_presenter"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$logFile = Join-Path $logDir "ppt2pdf_error.log"
Set-Content -Path $logFile -Value ("=== run " + (Get-Date -Format "HH:mm:ss") + " ===")
Add-Content -Path $logFile -Value ("InputPath=" + $InputPath)
Add-Content -Path $logFile -Value ("OutputPath=" + $OutputPath)
$pp = $null
$pres = $null
try {
    $pp = New-Object -ComObject PowerPoint.Application
    Add-Content -Path $logFile -Value ("COM created: " + $pp.Version)
    # Open(FileName, ReadOnly=$true, Untitled=$false, WithWindow=$true)
    $pres = $pp.Presentations.Open($InputPath, $true, $false, $true)
    Add-Content -Path $logFile -Value ("Opened, slides=" + $pres.Slides.Count)
    # 32 = ppSaveAsPDF，直接另存为 PDF（每页幻灯片 = 一页 PDF）
    $pres.SaveAs($OutputPath, 32)
    $pres.Close()
    Write-Output "OK"
    Add-Content -Path $logFile -Value ("SUCCESS -> " + $OutputPath)
    exit 0
} catch {
    $msg = "ERROR: " + $_.Exception.Message
    Write-Error $msg
    Add-Content -Path $logFile -Value $msg
    if ($_.Exception.InnerException) { Add-Content -Path $logFile -Value ("INNER: " + $_.Exception.InnerException.Message) }
    Add-Content -Path $logFile -Value ("SCRIPTSTACK: " + $_.ScriptStackTrace)
    exit 1
} finally {
    if ($pres) { try { $pres.Close() } catch {} }
    if ($pp)   { try { $pp.Quit() } catch {} }
    # 确保 PowerPoint 进程退出，避免残留占用
    Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
