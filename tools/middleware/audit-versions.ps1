[CmdletBinding()]
param(
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Read-MacroValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Macro
    )

    $pattern = '^\s*#define\s+' + [regex]::Escape($Macro) + '\s+"([^"]+)"'
    $match = Select-String -LiteralPath $Path -Pattern $pattern |
        Select-Object -First 1
    if ($null -eq $match) {
        throw "Macro $Macro not found in $Path"
    }
    return $match.Matches[0].Groups[1].Value
}

$mbedBuildInfo = Join-Path $ProjectRoot 'Middlewares\Third_Party\mbedTLS\include\mbedtls\build_info.h'
$freeRtosTask = Join-Path $ProjectRoot 'Middlewares\Third_Party\FreeRTOS\Source\include\task.h'
$customNetAdapter = Join-Path $ProjectRoot 'MBEDTLS\App\net_sockets.c'
$upstreamNetAdapter = Join-Path $ProjectRoot 'Middlewares\Third_Party\mbedTLS\library\net_sockets.c'
$mbedLicense = Join-Path $ProjectRoot 'Middlewares\Third_Party\mbedTLS\LICENSE'
$freeRtosLicense = Join-Path $ProjectRoot 'Middlewares\Third_Party\FreeRTOS\LICENSE.md'

$mbedVersion = Read-MacroValue -Path $mbedBuildInfo -Macro 'MBEDTLS_VERSION_STRING'
$freeRtosVersion = Read-MacroValue -Path $freeRtosTask -Macro 'tskKERNEL_VERSION_NUMBER'

$checks = [ordered]@{
    mbedTlsVersion = $mbedVersion
    freeRtosVersion = $freeRtosVersion
    customLwipAdapterPresent = Test-Path -LiteralPath $customNetAdapter
    upstreamSocketAdapterExcluded = -not (Test-Path -LiteralPath $upstreamNetAdapter)
    mbedTlsLicensePresent = Test-Path -LiteralPath $mbedLicense
    freeRtosLicensePresent = Test-Path -LiteralPath $freeRtosLicense
}

$checks | ConvertTo-Json

if ($mbedVersion -ne '3.6.7') {
    throw "Unexpected Mbed TLS version: $mbedVersion"
}
if ($freeRtosVersion -ne 'V11.3.0') {
    throw "Unexpected FreeRTOS version: $freeRtosVersion"
}
if (-not $checks.customLwipAdapterPresent -or -not $checks.upstreamSocketAdapterExcluded) {
    throw 'The project must use only the custom LwIP Mbed TLS socket adapter.'
}
if (-not $checks.mbedTlsLicensePresent -or -not $checks.freeRtosLicensePresent) {
    throw 'A middleware license file is missing.'
}

Write-Host 'Middleware baseline audit: PASS'
