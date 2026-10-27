# Licensed under the TGPIO Non-Commercial License (see LICENSE).
# Builds an unsigned CAB. Does not install, sign, or submit the driver.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out\signing'
if (Test-Path $out) { throw 'Use a clean output directory: windows/out/signing already exists.' }
$null = New-Item -ItemType Directory -Path $out
function Run([string]$exe, [string[]]$arguments) {
    Write-Host ($exe + ' ' + ($arguments -join ' '))
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: $LASTEXITCODE" }
}
Start-Transcript -Path "$out\build.log" | Out-Null
try {
    $wdk = Join-Path $root 'packages\Microsoft.Windows.WDK.x64.10.0.26100.6584\c'
    $infverif = (Get-ChildItem $wdk -Recurse -Filter infverif.exe | Where-Object FullName -Match '[\\/]x64[\\/]' | Select-Object -First 1).FullName
    $inf2cat = (Get-ChildItem $wdk -Recurse -Filter inf2cat.exe | Select-Object -First 1).FullName
    Run 'msbuild.exe' @("$root\driver\tgpio.vcxproj",'/t:Rebuild','/p:Configuration=Release','/p:Platform=x64','/p:SignMode=Off','/p:EnableInf2cat=false','/v:m',"/bl:$out\driver.binlog")
    $stage = Join-Path $out 'TGPIOx64'
    $null = New-Item -ItemType Directory -Path $stage
    foreach ($name in @('tgpio.sys','tgpio.inf','tgpio.ico')) {
        Copy-Item -LiteralPath "$root\driver\x64\Release\tgpio\$name" -Destination $stage
    }
    Copy-Item -LiteralPath "$root\driver\x64\Release\tgpio.pdb" -Destination $stage
    $inf = Get-Content "$stage\tgpio.inf" -Raw
    if ($inf.Contains('$ARCH$') -or $inf -notmatch 'NTamd64.10.0...16299') { throw 'INF architecture or minimum OS mismatch.' }
    Run $infverif @('/u','/v',"$stage\tgpio.inf")
    Run $infverif @('/h','/v',"$stage\tgpio.inf")
    $targets = '10_RS3_X64,10_RS4_X64,10_RS5_X64,10_19H1_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64,10_25H2_X64'
    Run $inf2cat @("/driver:$stage","/os:$targets",'/uselocaltime','/verbose')
    Push-Location $out
    try {
        Run 'cl.exe' @('/nologo','/W4','/WX','/O2','/Zi','/Fetgpioctl.exe',"$root\tools\tgpioctl\tgpioctl.c")
        Run 'dumpbin.exe' @('/headers','/imports','TGPIOx64\tgpio.sys')
        $ddf = @('.OPTION EXPLICIT','.Set CabinetFileCountThreshold=0','.Set FolderFileCountThreshold=0','.Set FolderSizeThreshold=0','.Set MaxCabinetSize=0','.Set MaxDiskFileCount=0','.Set MaxDiskSize=0','.Set CompressionType=MSZIP','.Set Cabinet=on','.Set Compress=on','.Set CabinetNameTemplate=TGPIO-18.0.0.2-x64-unsigned.cab','.Set DiskDirectoryTemplate=.','.Set DestinationDir=TGPIOx64')
        foreach ($name in @('tgpio.sys','tgpio.inf','tgpio.cat','tgpio.ico','tgpio.pdb')) {
            if ((Get-Item "TGPIOx64\$name").Length -eq 0) { throw "Empty file: $name" }
            $ddf += ('"TGPIOx64\' + $name + '"')
        }
        $ddf | Set-Content submission.ddf -Encoding ASCII
        Run 'makecab.exe' @('/F','submission.ddf')
        Run 'expand.exe' @('-D','TGPIO-18.0.0.2-x64-unsigned.cab')
        $record = [ordered]@{
            sourceCommit = (& git -C $root rev-parse HEAD)
            configuration = 'Release x64'; sdkWdkPackage = '10.0.26100.6584'
            minimumWindowsBuild = 16299; ntTargetVersion = '0x0A000004'; kmdf = '1.15'
            driverVersion = '18.0.0.2'; driverDate = '10/27/2026'; catalogTargets = $targets
            msvc = $env:VCToolsVersion; preparedUtc = [DateTime]::UtcNow.ToString('o')
            validation = 'WDK build, InfVerif /u, InfVerif /h, Inf2Cat, MakeCab'
            runtimeTests = 'Not performed; requires supported TGPIO hardware'
        }
        $record | ConvertTo-Json | Set-Content BUILD-RECORD.json
        $lines = Get-ChildItem TGPIOx64 -File | Sort-Object Name | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  TGPIOx64/' + $_.Name }
        $lines += (Get-FileHash 'TGPIO-18.0.0.2-x64-unsigned.cab' -Algorithm SHA256).Hash.ToLowerInvariant() + '  TGPIO-18.0.0.2-x64-unsigned.cab'
        $lines | Set-Content SHA256SUMS.txt -Encoding ASCII
    } finally { Pop-Location }
} finally { Stop-Transcript | Out-Null }
