# Read-only check of the WebBee path from this PC. It changes nothing: no adapter, route or
# DNS setting is touched. Run it with WLAN off (only the WebBee USB link up), then turn WLAN on
# again and send the output file. Needs no administrator rights.
#
#   powershell -ExecutionPolicy Bypass -File scripts\test-webbee-route.ps1

$out = Join-Path $PSScriptRoot "webbee-route-test.txt"
$lines = New-Object System.Collections.Generic.List[string]
function Add($s) { $lines.Add($s); Write-Host $s }

Add ("Test run: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))

Add "--- adapters (Up only)"
Get-NetAdapter | Where-Object Status -eq "Up" | ForEach-Object { Add ("  " + $_.Name + "  " + $_.LinkSpeed) }

Add "--- default routes"
Get-NetRoute -DestinationPrefix "0.0.0.0/0" | ForEach-Object {
    Add ("  via " + $_.NextHop + " on " + $_.InterfaceAlias + " metric " + $_.RouteMetric)
}

Add "--- which interface carries the internet (1.1.1.1:443)"
$r = Test-NetConnection 1.1.1.1 -Port 443 -InformationLevel Detailed -WarningAction SilentlyContinue
Add ("  leaves via " + $r.InterfaceAlias + ", source " + $r.SourceAddress.IPAddress + ", TCP ok: " + $r.TcpTestSucceeded)

Add "--- WebBee web UI (172.32.0.93:80)"
$w = Test-NetConnection 172.32.0.93 -Port 80 -WarningAction SilentlyContinue
Add ("  reachable: " + $w.TcpTestSucceeded)

Add "--- DNS for example.com (the PC's own DNS)"
try {
    Resolve-DnsName example.com -Type A -ErrorAction Stop | Where-Object Type -eq "A" | ForEach-Object { Add ("  " + $_.IPAddress) }
} catch { Add ("  failed: " + $_.Exception.Message) }

Add "--- DNS for example.com asked of the WebBee directly"
try {
    Resolve-DnsName example.com -Type A -Server 172.32.0.93 -ErrorAction Stop | Where-Object Type -eq "A" | ForEach-Object { Add ("  " + $_.IPAddress) }
} catch { Add ("  failed: " + $_.Exception.Message) }

Add "--- HTTP to a site through the WebBee's NAT (1.1.1.1:80, no redirect expected)"
$n = Test-NetConnection 1.1.1.1 -Port 80 -WarningAction SilentlyContinue
Add ("  TCP ok: " + $n.TcpTestSucceeded)

$lines | Set-Content -Path $out -Encoding UTF8
Write-Host ""
Write-Host ("Saved to " + $out)
