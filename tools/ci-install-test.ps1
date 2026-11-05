# CI only: installs the freshly built UzAlpha on the (clean, already elevated)
# build machine, checks what Windows recorded, then removes it and checks that
# nothing is left registered. Do not run it on a PC you use: it installs and
# uninstalls UzAlpha.
$ErrorActionPreference = 'Stop'
if (-not $env:CI) { throw 'This script is meant for the CI machine only.' }

$exe = Join-Path $PWD 'build\app\UzAlpha.exe'
$clsid = '{D6C739FF-D990-418B-8DDE-DA75436B5F08}'
$profiles = @{ auto = '{81E0DDC7-68E8-4A0F-8CF3-4CBCCE454F13}'; manual = '{B514FA1D-9D1B-431C-A0E9-BFC72B190B43}' }
$tipKey = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$startMenu = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\UzAlpha.lnk"
$desktop = "$env:PUBLIC\Desktop\UzAlpha.lnk"
$failures = @()
function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures += $what }
}
function InputList { (Get-WinUserLanguageList | ForEach-Object { $_.InputMethodTips }) -join ' | ' }

Write-Host '=== install'
$p = Start-Process $exe -ArgumentList '/install', '/quiet' -Wait -PassThru
Check "installer exit code is 0 (was $($p.ExitCode))" ($p.ExitCode -eq 0)

foreach ($f in "$env:ProgramFiles\UzAlpha\UzAlpha.exe", "$env:ProgramFiles\UzAlpha\x64\UzAlphaTip.dll",
               "$env:ProgramFiles\UzAlpha\x86\UzAlphaTip.dll", $startMenu, $desktop) {
    Check "file exists: $f" (Test-Path $f)
}
Check 'nothing is put into System32' (-not (Test-Path "$env:SystemRoot\System32\kbduzalp.dll"))

$tips = InputList
Write-Host "      this user's input list: $tips"
foreach ($name in $profiles.Keys) {
    $key = "$tipKey\LanguageProfile\0x00000443\$($profiles[$name])"
    Check "the $name profile is registered" (Test-Path $key)
    if (Test-Path $key) {
        $pp = Get-ItemProperty $key
        Write-Host "      $($pp.Description): icon $($pp.IconFile),$($pp.IconIndex)"
        Check "the $name profile is enabled by default" ($pp.Enable -eq 1)
        Check "the $name profile has its code icon" ($pp.IconFile -like '*UzAlphaTip.dll')
    }
    Check "the $name keyboard is in the user's list" ($tips -match [regex]::Escape($profiles[$name]))
}

Write-Host '=== starting with Windows'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$settingsKey = 'HKCU:\Software\UzAlpha'
$installedExe = "$env:ProgramFiles\UzAlpha\UzAlpha.exe"
$startupCommand = "`"$installedExe`" /tray"
function StartupEntry { (Get-ItemProperty $runKey -ErrorAction SilentlyContinue).UzAlpha }
# Starts the installed app the way Windows does at sign-in, gives it time to
# settle, and stops it again. Returns whether it was still running.
function RunAppBriefly {
    $app = Start-Process $installedExe -ArgumentList '/tray' -PassThru
    Start-Sleep -Seconds 8
    $alive = -not $app.HasExited
    if ($alive) { Stop-Process -Id $app.Id -Force; $app.WaitForExit() }
    return $alive
}
Write-Host "      startup entry: $(StartupEntry)"
Check 'the install wrote the startup entry for the installed copy' ((StartupEntry) -eq $startupCommand)
Remove-ItemProperty $runKey -Name UzAlpha
Check 'the app keeps running in the tray' (RunAppBriefly)
Check 'a lost startup entry is written again when the app starts' ((StartupEntry) -eq $startupCommand)
Set-ItemProperty $settingsKey -Name Autostart -Value 0 -Type DWord
Remove-ItemProperty $runKey -Name UzAlpha
[void](RunAppBriefly)
Check 'the entry stays away when the user turned starting with Windows off' ($null -eq (StartupEntry))
Set-ItemProperty $settingsKey -Name Autostart -Value 1 -Type DWord
[void](RunAppBriefly)
Check 'and comes back when it is turned on again' ((StartupEntry) -eq $startupCommand)

Write-Host '--- legacy keyboard list (information)'
$pre = Get-ItemProperty 'HKCU:\Keyboard Layout\Preload' -ErrorAction SilentlyContinue
$sub = Get-ItemProperty 'HKCU:\Keyboard Layout\Substitutes' -ErrorAction SilentlyContinue
$hid = Get-ItemProperty 'HKCU:\Software\Microsoft\CTF\HiddenDummyLayouts' -ErrorAction SilentlyContinue
if ($pre) { $pre.PSObject.Properties | Where-Object Name -match '^\d+$' | ForEach-Object { Write-Host "      Preload $($_.Name) = $($_.Value)" } }
if ($sub) { $sub.PSObject.Properties | Where-Object Name -match '^[0-9a-fA-F]{8}$' | ForEach-Object { Write-Host "      Substitute $($_.Name) -> $($_.Value)" } }
if ($hid) { $hid.PSObject.Properties | Where-Object Name -match '^[0-9a-fA-F]{8}$' | ForEach-Object { Write-Host "      Hidden dummy layout $($_.Name) = $($_.Value)" } }

Write-Host '--- how the text input system sees the profiles'
$diagLog = Join-Path $PWD 'build\tests\x64\ci_diag.log'
Start-Process 'build\tests\x64\tsf_diag.exe' -ArgumentList 'profile', $diagLog -Wait | Out-Null
if (Test-Path $diagLog) { Get-Content $diagLog | Select-Object -First 16 | ForEach-Object { Write-Host "      $_" } }
$appLog = "$env:LOCALAPPDATA\UzAlpha\uzalpha.log"
if (Test-Path $appLog) { Write-Host '--- app log'; Get-Content $appLog | ForEach-Object { Write-Host "      $_" } }

Write-Host '=== uninstall'
$p = Start-Process "$env:ProgramFiles\UzAlpha\UzAlpha.exe" -ArgumentList '/uninstall', '/quiet' -Wait -PassThru
Check "uninstaller exit code is 0 (was $($p.ExitCode))" ($p.ExitCode -eq 0)
Check 'the text service registration is gone' (-not (Test-Path $tipKey))
Check 'the COM class is gone' (-not (Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid"))
Check 'the Apps entry is gone' (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\UzAlpha'))
Check 'the shortcuts are gone' (-not (Test-Path $startMenu) -and -not (Test-Path $desktop))
Check 'the startup entry is gone' ($null -eq (StartupEntry))
$tips = InputList
Write-Host "      this user's input list: $tips"
Check "the keyboards are out of the user's list" ($tips -notmatch [regex]::Escape($clsid))

if ($failures.Count) { throw "$($failures.Count) check(s) failed: $($failures -join '; ')" }
Write-Host 'install test passed'
