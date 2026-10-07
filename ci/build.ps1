$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = Split-Path -Parent $PSScriptRoot
$Deps = Join-Path $Root ".deps"
$Out  = Join-Path $Root "out"

$FFmpegUrl  = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/ffmpeg-n8.1.2-34-g9b6c8969e0-win64-lgpl-shared-8.1.zip"
$FFmpegSha  = "c222a490dde4e7059f45495deef6bfb98dbcacc2b43df5b607546252037aa95c"
$CineFormUrl = "https://github.com/gopro/cineform-sdk/archive/11574d0295771edccadd17af14af74a539f924c7.zip"
$CineFormSha = "1fd8be2bacbf6cfa2bd5b568100dba492dfa474f090991ae84a853fd794efd3b"

function Step($text) { Write-Host ""; Write-Host "==== $text" }

function Fetch($url, $sha, $file) {
	if (-not (Test-Path $file)) {
		Write-Host "download $url"
		Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
	}
	$got = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
	if ($got -ne $sha) { throw "SHA-256 mismatch for $file`n  expected $sha`n  got      $got" }
}

function InVS($cmdline) {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $vs) { throw "Visual Studio with C++ tools not found" }
	$bat = Join-Path $env:TEMP ("sympathy-build-" + [guid]::NewGuid().ToString("N") + ".cmd")
	Set-Content -Path $bat -Encoding ASCII -Value @(
		"@call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul || exit /b 1",
		$cmdline
	)
	cmd /c "`"$bat`""
	$rc = $LASTEXITCODE
	Remove-Item $bat
	if ($rc -ne 0) { throw "failed ($rc): $cmdline" }
}

New-Item -ItemType Directory -Force $Deps, $Out | Out-Null

Step "FFmpeg"
$ffZip = Join-Path $Deps "ffmpeg.zip"
$ffDir = Join-Path $Deps "ffmpeg"
Fetch $FFmpegUrl $FFmpegSha $ffZip
if (-not (Test-Path (Join-Path $ffDir "lib\avcodec.lib"))) {
	$tmp = Join-Path $Deps "ffmpeg-unzip"
	if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
	Expand-Archive $ffZip $tmp
	if (Test-Path $ffDir) { Remove-Item -Recurse -Force $ffDir }
	Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $ffDir
	Remove-Item -Recurse -Force $tmp
}
if (-not (Select-String -Path (Join-Path $ffDir "LICENSE.txt") -Pattern "LESSER GENERAL PUBLIC LICENSE" -Quiet)) {
	throw "FFmpeg build is not LGPL"
}
$ffLibs = "avcodec-62", "avformat-62", "avutil-60", "swresample-6", "swscale-9"
$ffStamp = Join-Path $ffDir "lib\msvc.done"
if (-not (Test-Path $ffStamp)) {
	foreach ($n in $ffLibs) {
		$base = $n.Split("-")[0]
		InVS "lib /nologo /machine:x64 /def:`"$ffDir\lib\$n.def`" /name:$n.dll /out:`"$ffDir\lib\$base.lib`""
	}
	Set-Content $ffStamp "1"
}

Step "CineForm"
$cfZip = Join-Path $Deps "cineform.zip"
$cfDir = Join-Path $Deps "cineform"
Fetch $CineFormUrl $CineFormSha $cfZip
if (-not (Test-Path (Join-Path $cfDir "CMakeLists.txt"))) {
	$tmp = Join-Path $Deps "cineform-unzip"
	if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
	Expand-Archive $cfZip $tmp
	if (Test-Path $cfDir) { Remove-Item -Recurse -Force $cfDir }
	Move-Item (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName $cfDir
	Remove-Item -Recurse -Force $tmp
}
$cfLib = Join-Path $cfDir "build\Release\CFHDCodecStatic.lib"
if (-not (Test-Path $cfLib)) {
	InVS "cmake -S `"$cfDir`" -B `"$cfDir\build`" -G `"Visual Studio 17 2022`" -A x64 -DBUILD_STATIC=ON -DBUILD_LIBS=ON -DBUILD_SEPARATED=OFF -DBUILD_TOOLS=OFF && cmake --build `"$cfDir\build`" --config Release --target CFHDCodecStatic"
}

Step "plugin"
$pluginOut = Join-Path $Out "plugin"
InVS "msbuild `"$Root\src\sympathy.vcxproj`" /p:Configuration=Release /p:Platform=x64 /p:AshenFFmpeg=`"$ffDir`" /p:AshenCineForm=`"$cfDir`" /p:PREMSDKBUILDPATH=`"$pluginOut`" /m /v:minimal /nologo"
$deps = Join-Path $Out "dependents.txt"
InVS "dumpbin /nologo /dependents `"$pluginOut\sympathy.prm`" > `"$deps`""
$text = Get-Content $deps -Raw
$delay = $text.Substring([Math]::Max(0, $text.IndexOf("delay load dependencies")))
foreach ($n in $ffLibs) {
	if ($text.IndexOf("delay load dependencies") -lt 0 -or $delay -notmatch [regex]::Escape("$n.dll")) {
		throw "$n.dll is not delay-loaded"
	}
}
Remove-Item $deps

Step "payload"
$payload = Join-Path $Out "payload"
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory $payload | Out-Null
Copy-Item (Join-Path $pluginOut "sympathy.prm") (Join-Path $payload "Sympathy.prm")
foreach ($dll in $ffLibs) {
	Copy-Item (Join-Path $ffDir "bin\$dll.dll") $payload
}
Copy-Item (Join-Path $Root "redist\*.txt") $payload
Copy-Item (Join-Path $Root "COPYING.LESSER") (Join-Path $payload "sympathy license.txt")
Copy-Item (Join-Path $Root "COPYING") (Join-Path $payload "gpl license.txt")

Step "installer"
$inst = Join-Path $Root "installer"
$obj  = Join-Path $Out "obj"
New-Item -ItemType Directory -Force $obj | Out-Null
InVS "cl /nologo /O2 /EHsc /W4 /MT /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fo`"$obj\\`" /Fe`"$obj\pack.exe`" `"$inst\pack\pack.cpp`" `"$inst\pack\util.cpp`""

Push-Location (Join-Path $inst "tauri")
try {
	cmd /c "npm ci --no-audit --no-fund"
	if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
	cmd /c "npx tauri build --no-bundle"
	if ($LASTEXITCODE -ne 0) { throw "tauri build failed" }
} finally { Pop-Location }

$raw = Join-Path $obj "setup_raw.exe"
Copy-Item (Join-Path $inst "tauri\src-tauri\target\release\sympathy-setup.exe") $raw -Force
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $inst "tools\noicon.ps1") $raw
if ($LASTEXITCODE -ne 0) { throw "noicon failed" }

$setup = Join-Path $Out "Sympathy Setup.exe"
& (Join-Path $obj "pack.exe") $payload $raw $setup
if ($LASTEXITCODE -ne 0) { throw "pack failed" }

Step "done"
Get-ChildItem (Join-Path $pluginOut "sympathy.prm"), $setup | ForEach-Object {
	"{0}  {1,10}  {2}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Length, $_.Name
}
