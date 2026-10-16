# Runs the fork check on the current branch of the repo in the current
# directory. See README.md.
param(
  [string]$Base,
  [switch]$AllLines,
  [switch]$Full,
  [string]$Tools
)
$ErrorActionPreference = 'Stop'
if ($AllLines -and $Full) { throw '-AllLines and -Full are mutually exclusive.' }

$repo = (git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE) { throw 'Run this from inside the xenia-canary repo.' }
$commit = (git rev-parse HEAD).Trim()
if ($Base) {
  $baseSha = (git rev-parse --verify "$Base^{commit}").Trim()
} else {
  $baseSha = (git merge-base upstream/canary_experimental HEAD).Trim()
}
if ($LASTEXITCODE -or -not $baseSha) { throw 'Could not resolve the base commit.' }
$dirty = [bool](git status --porcelain --untracked-files=no)

$scope = if ($Full) { 'full' } elseif ($AllLines) { 'files' } else { 'changed' }
$wslRepo = (wsl -d Ubuntu-24.04 -- wslpath -a "$repo").Trim()
$wslTooling = (wsl -d Ubuntu-24.04 -- wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()

$cliArgs = @('--win-repo', "'$wslRepo'", '--commit', $commit, '--base', $baseSha,
             '--scope', $scope)
if ($Tools) { $cliArgs += @('--tools', $Tools) }
if ($dirty) { $cliArgs += '--dirty' }

wsl -d Ubuntu-24.04 -- bash -lc "cd '$wslTooling' && python3 -m fork_check $($cliArgs -join ' ')"
exit $LASTEXITCODE
