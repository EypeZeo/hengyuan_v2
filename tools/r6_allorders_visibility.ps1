#Requires -Version 7.0
# SPDX-License-Identifier: proprietary

<#
.SYNOPSIS
Lists recent Binance Spot Testnet orders and samples allOrders visibility.
.DESCRIPTION
Run this script with its Python sampler and the existing R6 probe in the same tools directory.
Choose a recent order interactively or supply an order ID to sample directly.
Use -ListOnly to display recent orders. Sampling prints a redacted JSON report.
.EXAMPLE
& '.\r6_allorders_visibility.ps1' -CredentialsFile '.env.operator' -Symbol BTCUSDT -OrderId 12345
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$CredentialsFile,

    [ValidateRange(0, 9223372036854775807)]
    [long]$OrderId,

    [switch]$ListOnly,

    [ValidatePattern('^[A-Z0-9]{1,19}$')]
    [string]$Symbol = 'BTCUSDT',

    [ValidateRange(1, 300)]
    [int]$DurationSeconds = 20,

    [ValidateRange(500, 60000)]
    [int]$PollIntervalMs = 1000,

    [ValidateRange(1, 30)]
    [int]$TimeoutSeconds = 5,

    [ValidateNotNullOrEmpty()]
    [string]$CaBundle
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

if (-not (Test-Path -LiteralPath $CredentialsFile -PathType Leaf)) {
    throw 'The specified Testnet credentials file is unavailable.'
}
$credentialsPath = (Resolve-Path -LiteralPath $CredentialsFile).ProviderPath

$samplerPath = Join-Path $PSScriptRoot 'r6_allorders_visibility.py'
$probePath = Join-Path $PSScriptRoot 'r6_testnet_probe.py'
if (-not (Test-Path -LiteralPath $samplerPath -PathType Leaf)) {
    throw 'Keep the PowerShell script beside r6_allorders_visibility.py.'
}
if (-not (Test-Path -LiteralPath $probePath -PathType Leaf)) {
    throw 'Keep the existing r6_testnet_probe.py in the same directory.'
}
$python = Get-Command -Name python -CommandType Application | Select-Object -First 1
$integerFormat = [Globalization.CultureInfo]::InvariantCulture
$hasOrderId = $PSBoundParameters.ContainsKey('OrderId')
if ($ListOnly -and $hasOrderId) {
    throw 'Choose either -ListOnly or -OrderId.'
}
$nativeArguments = @($samplerPath, '--env-file', $credentialsPath)
if ($hasOrderId) {
    $nativeArguments += @('--order-id', $OrderId.ToString($integerFormat))
} elseif ($ListOnly) {
    $nativeArguments += '--list-only'
}
$nativeArguments += @(
    '--symbol', $Symbol.ToUpperInvariant()
    '--duration-seconds', $DurationSeconds.ToString($integerFormat)
    '--poll-interval-ms', $PollIntervalMs.ToString($integerFormat)
    '--timeout-seconds', $TimeoutSeconds.ToString($integerFormat)
)

$caPath = $null
if ($PSBoundParameters.ContainsKey('CaBundle')) {
    if (-not (Test-Path -LiteralPath $CaBundle -PathType Leaf)) {
        throw 'The specified CA bundle is unavailable.'
    }
    $caPath = (Resolve-Path -LiteralPath $CaBundle).ProviderPath
}
$originalCa = $env:SSL_CERT_FILE
try {
    if ($null -ne $caPath) {
        $env:SSL_CERT_FILE = $caPath
    }
    & $python.Source @nativeArguments
    $samplingExitCode = $LASTEXITCODE
} finally {
    if ($null -ne $caPath) {
        [Environment]::SetEnvironmentVariable('SSL_CERT_FILE', $originalCa, 'Process')
    }
}
exit $samplingExitCode
