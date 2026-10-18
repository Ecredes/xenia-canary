# Builds a fix branch and its upstream base side by side, both with the
# telemetry branch merged in, for an A/B run in game-mcp. Run from the
# xenia-canary repo, in PowerShell (the build needs vswhere). See README.md.
# Exit: 0 pair ready, 1 telemetry overlaps the fix's diff, 2 broken.
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Fix,
  [string]$Base,
  [string]$Telemetry,
  [string]$Name,
  [ValidateSet('release', 'debug', 'checked')][string]$Config = 'release',
  [switch]$Reuse,
  [switch]$NoBuild,
  [switch]$AllowOverlap,
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
# Any unexpected error is "broken", never PowerShell's default exit 1, which
# reads as "overlap".
trap {
  Write-Error $_ -ErrorAction Continue
  exit 2
}

$pyArgs = @('-m', 'ab_build', $Fix, '--config', $Config)
if ($Base) { $pyArgs += @('--base', $Base) }
if ($Telemetry) { $pyArgs += @('--telemetry', $Telemetry) }
if ($Name) { $pyArgs += @('--name', $Name) }
if ($Reuse) { $pyArgs += '--reuse' }
if ($NoBuild) { $pyArgs += '--no-build' }
if ($AllowOverlap) { $pyArgs += '--allow-overlap' }
if ($Clean) { $pyArgs += '--clean' }

$env:PYTHONPATH = $PSScriptRoot
python @pyArgs
$code = $LASTEXITCODE
if ($code -notin 0, 1, 2) { exit 2 }
exit $code
