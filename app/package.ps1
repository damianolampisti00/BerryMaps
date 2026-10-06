# Builds the client and packages a .bar, bumping <buildId> in bar-descriptor.xml
# first so BB10 accepts it as an in-place update (version must increase).
#
# Usage:  powershell -File package.ps1
#         powershell -File package.ps1 -Install               (also copies to the
#                     phone's Downloads and installs it, over SSH as root)
#         powershell -File package.ps1 -Install -PhoneIp 192.168.1.xxx
#                     (IP changes on reboot/DHCP; defaults to the last known one)
param(
    [switch]$Install,
    [string]$PhoneIp = '',
    [string]$RootKey = ''
)
$ErrorActionPreference = 'Stop'
$c = $PSScriptRoot
$h = 'C:\bbndk\ndk\host_10_3_1_12\win32\x86'
$jre = 'C:\bbndk\features\com.qnx.tools.jre.win32.x86_64_1.7.0.51\jre'  # system Java 25 crashes the packager
$env:QNX_HOST = $h
$env:QNX_TARGET = 'C:\bbndk\ndk\target_10_3_1_995\qnx6'
$env:JAVA_HOME = $jre
$env:PATH = "$jre\bin;$h\usr\bin;$env:PATH"

# --- bump build id ---
$desc = Get-Content "$c\bar-descriptor.xml" -Raw
$build = [int]([regex]::Match($desc, '<buildId>(\d+)</buildId>').Groups[1].Value) + 1
$desc = $desc -replace '<buildId>\d+</buildId>', "<buildId>$build</buildId>"
$ver = [regex]::Match($desc, '<versionNumber>([^<]+)</versionNumber>').Groups[1].Value
Set-Content "$c\bar-descriptor.xml" $desc -Encoding UTF8 -NoNewline
$full = "$ver.$build"

# --- build ---
New-Item -ItemType Directory -Force "$c\build" | Out-Null
Push-Location "$c\build"
if (-not (Test-Path Makefile)) {
    qmake ..\BerryMaps.pro -spec blackberry-armv7le-qcc CONFIG+=device CONFIG+=release
}
make -j4
if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'make failed' }
Pop-Location

# --- stage + package ---
$s = "$c\stage"
Remove-Item $s -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$s\arm\o.le-v7" | Out-Null
Copy-Item "$c\build\o.le-v7\BerryMaps.so" "$s\arm\o.le-v7\"
Copy-Item "$c\assets" "$s\assets" -Recurse
Copy-Item "$c\icon.png", "$c\bar-descriptor.xml" $s
Push-Location $s
$bar = "$c\BerryMaps-$full-armv7.bar"
blackberry-nativepackager -package $bar bar-descriptor.xml -configuration Device-Release -devMode
Pop-Location
Write-Host "Created $bar (version $full)"

if (-not $Install) { exit 0 }

# --- copy to the phone and install, over SSH as root (see tools/installbar.sh) ---
# Done through bash/ssh, not PowerShell's own ssh/scp: PowerShell's scp reads
# "C:\..." (colon before the first slash) as a scp-style "host:path", not a local
# Windows path, and piping a *binary* file through PowerShell's own pipeline to a
# native process risks corrupting it (text/newline reinterpretation). Git Bash's
# `<` redirection has neither problem, so this whole step runs as one bash script.
$barName = Split-Path $bar -Leaf
$installer = Join-Path (Split-Path $c) 'tools\installbar.sh'
$toPosix = { param($w) '/' + $w.Substring(0, 1).ToLower() + $w.Substring(2).Replace('\', '/') }
$barPosix = & $toPosix $bar
$installerPosix = & $toPosix $installer
$keyPosix = & $toPosix $RootKey

$bashScript = @"
set -e
# An array, not a "SSH=`"ssh -i ...`"" string: expanding a quoted string doesn't
# re-parse the quotes inside it, so the literal quote characters would end up as
# part of the -i argument (ssh would then look for a file named with quotes in it).
SSH=(ssh -i "$keyPosix" -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa -o KexAlgorithms=diffie-hellman-group14-sha1 -o Ciphers=aes128-cbc -o MACs=hmac-sha1 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@$PhoneIp)
echo "Copying $barName and installbar.sh to the phone ($PhoneIp)..."
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/$barName" < "$barPosix"
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/installbar.sh" < "$installerPosix"
echo "Installing..."
`"`${SSH[@]}`" "cat /accounts/1000/shared/downloads/installbar.sh | sh -s -- $barName"
"@
# A plain file, not a pipe: PowerShell's default pipe-to-native-stdin encoding adds a
# UTF-8 BOM that garbles bash's first line, and there is no TTY here for `bash -i`.
$scriptFile = Join-Path $env:TEMP 'bm_install.sh'
[System.IO.File]::WriteAllText($scriptFile, $bashScript.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
$scriptFilePosix = & $toPosix $scriptFile
$bashExe = 'C:\Program Files\Git\bin\bash.exe'
& $bashExe $scriptFilePosix
if ($LASTEXITCODE -ne 0) { throw "install over SSH failed (exit $LASTEXITCODE) -- see the output above; is the phone reachable at $PhoneIp?" }
Remove-Item $scriptFile -ErrorAction SilentlyContinue
Write-Host "Installed $barName on the phone."
