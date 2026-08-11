[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$ProgrammerCli,
    [Parameter(Mandatory = $true)] [ValidatePattern('^[0-9A-Fa-f]+$')] [string]$StLinkSerial,
    [ValidateSet('Development', 'Production')] [string]$ExpectedProfile = 'Development'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ProgrammerCli -PathType Leaf)) {
    throw "STM32_Programmer_CLI was not found: $ProgrammerCli"
}

$serial = $StLinkSerial.ToUpperInvariant()
$output = (& $ProgrammerCli -c "port=SWD sn=$serial mode=HotPlug" -ob displ 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "Option-byte read failed (exit $LASTEXITCODE).`n$output"
}

function Read-HexField([string]$Name) {
    $match = [regex]::Match($output, "(?m)^\s*$([regex]::Escape($Name))\s*:\s*0x([0-9A-Fa-f]+)")
    if (-not $match.Success) { throw "Option-byte field was not reported: $Name" }
    return [Convert]::ToUInt32($match.Groups[1].Value, 16)
}

$wrp = 0..15 | ForEach-Object { Read-HexField "nWRP$_" }
$report = [ordered]@{
    stLinkSerial = $serial
    expectedProfile = $ExpectedProfile
    rdp = ('0x{0:X2}' -f (Read-HexField 'RDP'))
    bootloaderWriteProtected = ($wrp[0] -eq 0)
    otherSectorsWritable = (($wrp | Select-Object -Skip 1 | Where-Object { $_ -ne 1 }).Count -eq 0)
    pcropBank1Disabled = ((Read-HexField 'PROT_AREA_START1') -gt (Read-HexField 'PROT_AREA_END1'))
    pcropBank2Disabled = ((Read-HexField 'PROT_AREA_START2') -gt (Read-HexField 'PROT_AREA_END2'))
}

$expectedRdp = if ($ExpectedProfile -eq 'Production') { '0xBB' } else { '0xAA' }
$expectedWrp = $ExpectedProfile -eq 'Production'
$report.compliant =
    ($report.rdp -eq $expectedRdp) -and
    ($report.bootloaderWriteProtected -eq $expectedWrp) -and
    $report.otherSectorsWritable -and
    $report.pcropBank1Disabled -and
    $report.pcropBank2Disabled

[pscustomobject]$report | ConvertTo-Json -Depth 3
if (-not $report.compliant) { exit 1 }
