# Regenerates the integration branch (upstream plus every branch in
# config/integration.txt) and builds it. Run from the xenia-canary repo, in
# PowerShell (the build needs vswhere). See README.md.
# Exit: 0 ready, 1 merge conflict to resolve, 2 broken.
[CmdletBinding(PositionalBinding = $false)]
param(
  [string]$List,
  [string]$Upstream,
  [ValidateSet('release', 'debug', 'checked')][string]$Config = 'release',
  [switch]$Resume,
  [switch]$NoFetch,
  [switch]$NoBuild,
  [switch]$Push,
  [switch]$Clean,
  # Caught here so `--resume` isn't taken as -Upstream's value.
  [Parameter(ValueFromRemainingArguments)][string[]]$Unknown
)
$ErrorActionPreference = 'Stop'
# Any unexpected error is "broken", never PowerShell's default exit 1, which
# reads as "conflict".
trap {
  Write-Error $_ -ErrorAction Continue
  exit 2
}

if ($Unknown) {
  Write-Error ("Unknown argument(s): $($Unknown -join ' '). This script takes " +
               "PowerShell flags (-Resume, -NoBuild, ...), not --resume.") `
    -ErrorAction Continue
  exit 2
}

$pyArgs = @('-m', 'integrate', '--config', $Config)
if ($List) { $pyArgs += @('--list', $List) }
if ($Upstream) { $pyArgs += @('--upstream', $Upstream) }
if ($Resume) { $pyArgs += '--resume' }
if ($NoFetch) { $pyArgs += '--no-fetch' }
if ($NoBuild) { $pyArgs += '--no-build' }
if ($Push) { $pyArgs += '--push' }
if ($Clean) { $pyArgs += '--clean' }

$env:PYTHONPATH = $PSScriptRoot
# Hints then name -Resume, not --resume.
$env:INTEGRATE_CALLER = 'powershell'
python @pyArgs
$code = $LASTEXITCODE
if ($code -notin 0, 1, 2) { exit 2 }
exit $code
