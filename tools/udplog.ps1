# Riceve i log WHBLogUdp della console Wii U (broadcast UDP porta 4405).
# Uso: .\tools\udplog.ps1            (Ctrl+C per uscire)
#      .\tools\udplog.ps1 -LogFile stress-hw.log
param([string]$LogFile = "")

$udp = New-Object System.Net.Sockets.UdpClient(4405)
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
Write-Host "[udplog] listening on UDP 4405 (Wii U WHBLogUdp)..." -ForegroundColor Cyan
try {
    while ($true) {
        $bytes = $udp.Receive([ref]$remote)
        $line = [System.Text.Encoding]::ASCII.GetString($bytes).TrimEnd("`0", "`r", "`n")
        $stamp = (Get-Date).ToString("HH:mm:ss.fff")
        $out = "[$stamp] $line"
        Write-Host $out
        if ($LogFile) { Add-Content -Path $LogFile -Value $out }
    }
} finally {
    $udp.Close()
}
