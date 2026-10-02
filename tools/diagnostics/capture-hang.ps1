<#
  Run this when Satisfactory has closed but Steam still says it is running.

  It finds the game process (also the PIDs Steam is still tracking, in case Task Manager does not list it), prints its
  threads and loaded mod/engine DLLs, and writes a memory dump to the Desktop. The dump shows exactly what the stuck
  process is waiting on; send it to whoever is debugging. Nothing is uploaded and the process is not killed.

  Usage:  powershell -ExecutionPolicy Bypass -File capture-hang.ps1
#>
$ErrorActionPreference = 'Continue'
$names = 'FactoryGameSteam-Win64-Shipping', 'FactoryGameEGS-Win64-Shipping', 'FactoryGameSteam', 'FactoryGame'
$found = @(Get-Process -Name $names -ErrorAction SilentlyContinue)

# PIDs Steam still tracks for Satisfactory (app 526870) but has not seen exit.
$steamLog = 'C:\Program Files (x86)\Steam\logs\gameprocess_log.txt'
if (Test-Path $steamLog)
{
	$added = @{}
	foreach ($line in Get-Content $steamLog -Encoding UTF8 -ErrorAction SilentlyContinue)
	{
		if ($line -match 'AppID 526870 adding PID (\d+)') { $added[$Matches[1]] = $true }
		elseif ($line -match 'AppID 526870 no longer tracking PID (\d+)') { $added.Remove($Matches[1]) }
	}
	foreach ($procId in $added.Keys)
	{
		$p = Get-Process -Id ([int]$procId) -ErrorAction SilentlyContinue
		$cim = Get-CimInstance Win32_Process -Filter "ProcessId=$procId" -ErrorAction SilentlyContinue
		"Steam still tracks PID $procId : visible=$([bool]$p) wmi=$([bool]$cim) $($cim.Name)"
		if ($p -and -not ($found | Where-Object Id -eq $p.Id)) { $found += $p }
	}
}

if ($found.Count -eq 0)
{
	'No Satisfactory process is running. If Steam still shows "Running", the process is stuck inside Windows itself'
	'(usually a driver); restarting Steam clears it, and a reboot clears the process.'
	exit
}

foreach ($p in $found)
{
	$p.Refresh()
	''
	"== $($p.ProcessName) pid=$($p.Id) threads=$($p.Threads.Count) responding=$($p.Responding) window='$($p.MainWindowTitle)'"
	$p.Threads | Group-Object { "$($_.ThreadState)/$($_.WaitReason)" } | Sort-Object Count -Descending | ForEach-Object { "   $($_.Count) x $($_.Name)" }
	"   DLLs of interest: " + (($p.Modules | Where-Object { $_.ModuleName -match 'rclone|winpthread|SharedWorld|Sentry|EOS|steam_api' } | ForEach-Object { $_.ModuleName }) -join ', ')
	$dump = Join-Path ([Environment]::GetFolderPath('Desktop')) ("satisfactory-hang-{0}-{1}.dmp" -f $p.Id, (Get-Date -Format 'yyyyMMdd-HHmmss'))
	# Built into Windows (no install). Writes a full dump; may need an elevated PowerShell.
	& rundll32.exe C:\Windows\System32\comsvcs.dll, MiniDump $p.Id $dump full
	Start-Sleep -Seconds 3
	if (Test-Path $dump) { "   dump written: $dump ($([int]((Get-Item $dump).Length / 1MB)) MB)" }
	else { '   could not write a dump (try again from an elevated PowerShell: right-click > Run as administrator)' }
}
