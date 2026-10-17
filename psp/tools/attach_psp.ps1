# Attach the USB device of a PSP running PSPLink ("PSP" Type B, 054c:01c9) to WSL (usbipd-win).
#   powershell -File psp/tools/attach_psp.ps1          # attach once
#   powershell -File psp/tools/attach_psp.ps1 -Auto    # keep reattaching across replugs (leave the window open)
# First time needs a share registration as admin: usbipd bind --busid <BUSID> (skip if already done)
# Attaching fails while WSL is off, so keep one WSL instance running.
param([switch]$Auto, [string]$Distro = 'Ubuntu-24.04')

$usbipd = "C:\Program Files\usbipd-win\usbipd.exe"
if (-not (Test-Path $usbipd)) { Write-Error "usbipd-win not found: winget install dorssel.usbipd-win"; exit 1 }

# Keep WSL running (so it does not shut down while attached)
Start-Process -WindowStyle Hidden -FilePath wsl.exe -ArgumentList "-d", $Distro, "--exec", "sleep", "infinity"
Start-Sleep -Seconds 2

if ($Auto) {
	# Keep reattaching by hardware ID (BUSID may change)
	& $usbipd attach --wsl --hardware-id 054c:01c9 --auto-attach
	exit $LASTEXITCODE
}

& $usbipd attach --wsl --hardware-id 054c:01c9
if ($LASTEXITCODE -ne 0) {
	Write-Error "Attach failed: check that PSPLink is running on the PSP, USB is connected, and usbipd bind was done"
	exit 1
}
& $usbipd list | Select-String "054c"
