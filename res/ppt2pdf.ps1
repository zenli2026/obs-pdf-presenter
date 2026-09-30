# ppt2pdf.ps1 —— 调用本机 PowerPoint 把 PPT/PPTX 转成 PDF
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File ppt2pdf.ps1 -InputPath "x.pptx" -OutputPath "x.pdf"
param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
$ErrorActionPreference = "Stop"
$pp = $null
$pres = $null
try {
    $pp = New-Object -ComObject PowerPoint.Application
    # Open(FileName, ReadOnly=$true, Untitled=$false, WithWindow=$true)
    $pres = $pp.Presentations.Open($InputPath, $true, $false, $true)
    # 32 = ppSaveAsPDF，直接另存为 PDF（每页幻灯片 = 一页 PDF）
    $pres.SaveAs($OutputPath, 32)
    $pres.Close()
    Write-Output "OK"
    exit 0
} catch {
    Write-Error "PPT转换失败: $($_.Exception.Message)"
    exit 1
} finally {
    if ($pres) { try { $pres.Close() } catch {} }
    if ($pp)   { try { $pp.Quit() } catch {} }
    # 确保 PowerPoint 进程退出，避免残留占用
    Get-Process POWERPNT -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
