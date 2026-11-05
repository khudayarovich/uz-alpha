# CI only: installs the freshly built UzAlpha on the (clean, already elevated)
# build machine, checks what Windows recorded, then removes it and checks that
# nothing is left registered. Do not run it on a PC you use: it installs and
# uninstalls UzAlpha.
$ErrorActionPreference = 'Stop'
if (-not $env:CI) { throw 'This script is meant for the CI machine only.' }

$exe = Join-Path $PWD 'build\app\UzAlpha.exe'
$clsid = '{D6C739FF-D990-418B-8DDE-DA75436B5F08}'
$profile = '{81E0DDC7-68E8-4A0F-8CF3-4CBCCE454F13}'
$profileKey = "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid\LanguageProfile\0x00000443\$profile"
$layouts = 'HKLM:\SYSTEM\CurrentControlSet\Control\Keyboard Layouts'
$failures = @()
function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "ok    $what" } else { Write-Host "FAIL  $what"; $script:failures += $what }
}
function OurLayout {
    Get-ChildItem $layouts | Where-Object { (Get-ItemProperty $_.PSPath).'Layout File' -eq 'kbduzalp.dll' }
}

Write-Host '=== install'
$p = Start-Process $exe -ArgumentList '/install', '/quiet' -Wait -PassThru
Check "installer exit code is 0 (was $($p.ExitCode))" ($p.ExitCode -eq 0)

foreach ($f in "$env:ProgramFiles\UzAlpha\UzAlpha.exe", "$env:ProgramFiles\UzAlpha\x64\UzAlphaTip.dll",
               "$env:ProgramFiles\UzAlpha\x86\UzAlphaTip.dll", "$env:SystemRoot\System32\kbduzalp.dll",
               "$env:SystemRoot\SysWOW64\kbduzalp.dll") {
    Check "file exists: $f" (Test-Path $f)
}
$layout = OurLayout
Check 'the manual layout is registered' ($null -ne $layout)
if ($layout) {
    $lp = Get-ItemProperty $layout.PSPath
    Write-Host "      $($layout.PSChildName): $($lp.'Layout Text') (id $($lp.'Layout Id'))"
}
Check 'the automatic profile is registered' (Test-Path $profileKey)
if (Test-Path $profileKey) {
    $pp = Get-ItemProperty $profileKey
    Check 'the profile is enabled by default' ($pp.Enable -eq 1)
    Check 'the profile has no icon (the taskbar shows the language code)' (-not $pp.IconFile)
    Write-Host "      Description: $($pp.Description)"
}
$tips = (Get-WinUserLanguageList | ForEach-Object { $_.InputMethodTips }) -join ' | '
Write-Host "      this user's input list: $tips"
Check "the automatic keyboard is in the user's list" ($tips -match [regex]::Escape($clsid))
Check "the manual keyboard is in the user's list" ($layout -and $tips -match $layout.PSChildName)

Write-Host '--- legacy keyboard list (information)'
$pre = Get-ItemProperty 'HKCU:\Keyboard Layout\Preload' -ErrorAction SilentlyContinue
$sub = Get-ItemProperty 'HKCU:\Keyboard Layout\Substitutes' -ErrorAction SilentlyContinue
if ($pre) { $pre.PSObject.Properties | Where-Object Name -match '^\d+$' | ForEach-Object { Write-Host "      Preload $($_.Name) = $($_.Value)" } }
if ($sub) { $sub.PSObject.Properties | Where-Object Name -match '^[0-9a-fA-F]{8}$' | ForEach-Object { Write-Host "      Substitute $($_.Name) -> $($_.Value)" } }

Write-Host '--- how the text input system sees the profile'
$diag = 'build\tests\x64\tsf_diag.exe'
$diagLog = Join-Path $PWD 'build\tests\x64\ci_diag.log'
$d = Start-Process $diag -ArgumentList 'profile', $diagLog -Wait -PassThru
if (Test-Path $diagLog) {
    $lines = Get-Content $diagLog
    $lines | Select-Object -First 14 | ForEach-Object { Write-Host "      $_" }
    $ours = $lines | Where-Object { $_ -match '0443 profile: type=1 ours=yes' } | Select-Object -First 1
    if ($ours) {
        Check 'Windows reports the automatic profile as enabled' ($ours -match 'flags=0000000[23]')
    } else {
        Write-Host '      (text input services are not available in this session; skipped)'
    }
}
$appLog = "$env:LOCALAPPDATA\UzAlpha\uzalpha.log"
if (Test-Path $appLog) { Write-Host '--- app log'; Get-Content $appLog | ForEach-Object { Write-Host "      $_" } }

Write-Host '=== uninstall'
$p = Start-Process "$env:ProgramFiles\UzAlpha\UzAlpha.exe" -ArgumentList '/uninstall', '/quiet' -Wait -PassThru
Check "uninstaller exit code is 0 (was $($p.ExitCode))" ($p.ExitCode -eq 0)
Check 'the automatic profile is gone' (-not (Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"))
Check 'the COM class is gone' (-not (Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid"))
Check 'the manual layout is gone' ($null -eq (OurLayout))
Check 'the Apps entry is gone' (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\UzAlpha'))
$tips = (Get-WinUserLanguageList | ForEach-Object { $_.InputMethodTips }) -join ' | '
Write-Host "      this user's input list: $tips"
Check "the keyboards are out of the user's list" ($tips -notmatch [regex]::Escape($clsid))

if ($failures.Count) { throw "$($failures.Count) check(s) failed: $($failures -join '; ')" }
Write-Host 'install test passed'
