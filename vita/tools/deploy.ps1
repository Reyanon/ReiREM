# Windows deploy (same as tools/deploy.sh). Use when the Vita is unreachable from WSL NAT.
#   powershell -File vita/tools/deploy.ps1 [-Eboot build-vita/eboot.bin] [-VitaIp 192.168.0.33]
# If unreachable, report failure and do not retry (docs/RULES.md 5-1).
param(
	[string]$Eboot = "$PSScriptRoot\..\..\build-vita\eboot.bin",
	[string]$VitaIp = $(if ($env:VITA_IP) { $env:VITA_IP } else { '192.168.0.33' })
)
$ErrorActionPreference = 'Stop'
$TitleId = 'M92A00001'

function Send-VitaCmd([string]$cmd) {
	$t = New-Object Net.Sockets.TcpClient
	try {
		$t.Connect($VitaIp, 1338)
		$s = $t.GetStream()
		$b = [Text.Encoding]::ASCII.GetBytes("$cmd`n")
		$s.Write($b, 0, $b.Length)
		Start-Sleep -Milliseconds 300
	} finally { $t.Close() }
}

try { Send-VitaCmd "quit $TitleId" } catch {
	Write-Error "deploy: ${VitaIp}:1338 unreachable (check vitacompanion)"
	exit 1
}
Start-Sleep -Seconds 2
# Right after the app quits the file may be locked (550), so retry the upload once
curl.exe -s -S -T $Eboot "ftp://${VitaIp}:1337/ux0:/app/$TitleId/"
if ($LASTEXITCODE -ne 0) {
	Start-Sleep -Seconds 2
	curl.exe -s -S -T $Eboot "ftp://${VitaIp}:1337/ux0:/app/$TitleId/"
	if ($LASTEXITCODE -ne 0) { Write-Error "deploy: upload failed (curl $LASTEXITCODE)"; exit 1 }
}
Send-VitaCmd "launch $TitleId"
Write-Output "deploy: $Eboot -> ${VitaIp} launched"
