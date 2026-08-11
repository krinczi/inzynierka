[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string]$ProgrammerCli,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]+$')]
    [string]$StLinkSerial,

    [Parameter(Mandatory = $true)]
    [string]$ConfirmStLinkSerial,

    [ValidateRange(1, 100)]
    [int]$Iterations = 10,

    [ValidateRange(0, 60000)]
    [int]$MinimumDelayMs = 250,

    [ValidateRange(0, 60000)]
    [int]$MaximumDelayMs = 3000,

    [ValidateRange(1000, 120000)]
    [int]$RecoveryDelayMs = 10000,

    [string]$ReportDirectory = (Join-Path $PSScriptRoot '..\..\Release\recovery-report'),

    [switch]$ExecuteResets
)

$ErrorActionPreference = 'Stop'
$expectedSerial = $StLinkSerial.ToUpperInvariant()
if ($ConfirmStLinkSerial.ToUpperInvariant() -cne $expectedSerial) {
    throw 'ConfirmStLinkSerial must exactly match StLinkSerial.'
}
if ($MinimumDelayMs -gt $MaximumDelayMs) {
    throw 'MinimumDelayMs cannot exceed MaximumDelayMs.'
}
if (-not (Test-Path -LiteralPath $ProgrammerCli -PathType Leaf)) {
    throw "STM32_Programmer_CLI was not found: $ProgrammerCli"
}

Write-Host "Target ST-LINK: $expectedSerial"
Write-Host "Iterations: $Iterations; random delay: $MinimumDelayMs..$MaximumDelayMs ms"
if (-not $ExecuteResets) {
    Write-Host 'DRY RUN: no reset and no target-memory access was performed.'
    Write-Host 'Use -ExecuteResets and confirm the PowerShell prompt only with a staged, signed candidate image.'
    return
}

$target = "ST-LINK $expectedSerial"
if (-not $PSCmdlet.ShouldProcess($target, "perform $Iterations reset-recovery iterations")) {
    return
}

New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$inspector = Join-Path $PSScriptRoot 'inspect-control-log.mjs'
$summary = [System.Collections.Generic.List[object]]::new()

for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    $delay = Get-Random -Minimum $MinimumDelayMs -Maximum ($MaximumDelayMs + 1)
    Start-Sleep -Milliseconds $delay

    & $ProgrammerCli -c "port=SWD sn=$expectedSerial mode=HotPlug" -hardRst
    if ($LASTEXITCODE -ne 0) {
        throw "Hardware reset failed in iteration $iteration (exit $LASTEXITCODE)."
    }

    Start-Sleep -Milliseconds $RecoveryDelayMs
    $dump = Join-Path $ReportDirectory ("control-log-{0:D3}.bin" -f $iteration)
    & $ProgrammerCli -c "port=SWD sn=$expectedSerial mode=HotPlug" -u 0x08100000 0x20000 $dump
    if ($LASTEXITCODE -ne 0) {
        throw "Control-log read failed in iteration $iteration (exit $LASTEXITCODE)."
    }

    $inspection = (& node $inspector --input $dump | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $null -eq $inspection.latest) {
        throw "No valid OTA control record after iteration $iteration."
    }
    $summary.Add([pscustomobject]@{
        iteration = $iteration
        delayMs = $delay
        sequence = $inspection.latest.sequence
        state = $inspection.latest.state
        confirmedVersion = $inspection.latest.confirmedVersion
        primaryVersion = $inspection.latest.primaryVersion
        secondaryVersion = $inspection.latest.secondaryVersion
    })

    # A target-memory upload can leave the core halted on some
    # STM32CubeProgrammer/ST-LINK combinations. Always release the tested
    # device before the next iteration and before returning it to service.
    & $ProgrammerCli -c "port=SWD sn=$expectedSerial mode=HotPlug" -hardRst
    if ($LASTEXITCODE -ne 0) {
        throw "Target release reset failed in iteration $iteration (exit $LASTEXITCODE)."
    }
}

$report = Join-Path $ReportDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $report -Encoding utf8
Write-Host "Recovery report: $report"
