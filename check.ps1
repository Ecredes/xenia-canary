# Runs the fork check on the current branch of the repo in the current
# directory. See README.md. Exit: 0 clean, 1 findings, 2 checker broken.
param(
  [string]$Base,
  [switch]$AllLines,
  [switch]$Full,
  # `-Tools a,b` arrives as an array in PowerShell.
  [string[]]$Tools
)
$ErrorActionPreference = 'Stop'
# Any unexpected error (wsl.exe missing, a $null result) is "checker broken",
# never PowerShell's default exit 1, which reads as "findings".
trap {
  Write-Error $_ -ErrorAction Continue
  exit 2
}

function Fail([string]$message) {
  Write-Error $message -ErrorAction Continue
  exit 2
}

if ($AllLines -and $Full) { Fail '-AllLines and -Full are mutually exclusive.' }

$repo = git rev-parse --show-toplevel
if ($LASTEXITCODE -or -not $repo) { Fail 'Run this from inside the xenia-canary repo.' }
$commit = git rev-parse --verify HEAD
if ($LASTEXITCODE -or -not $commit) { Fail 'Could not resolve HEAD (no commits yet?).' }
if ($Base) {
  $baseSha = git rev-parse --verify "$Base^{commit}"
} else {
  $baseSha = git merge-base upstream/canary_experimental HEAD
}
if ($LASTEXITCODE -or -not $baseSha) { Fail 'Could not resolve the base commit.' }
$dirty = [bool](git status --porcelain --untracked-files=no)

$toolList = ''
if ($Tools) {
  $toolList = ($Tools -join ',')
  # The value is pasted into a bash command line below.
  if ($toolList -notmatch '^[a-z,-]+$') { Fail "Invalid -Tools value: $toolList" }
}

$scope = if ($Full) { 'full' } elseif ($AllLines) { 'files' } else { 'changed' }
$wslRepo = wsl -d Ubuntu-24.04 -- wslpath -a "$($repo.Trim())"
$wslTooling = wsl -d Ubuntu-24.04 -- wslpath -a ($PSScriptRoot -replace '\\', '/')
if ($LASTEXITCODE -or -not $wslRepo -or -not $wslTooling) { Fail 'Could not reach the Ubuntu-24.04 WSL distro.' }
# Both paths go inside single quotes in bash, and wsl.exe expands `$`.
foreach ($p in @($wslRepo, $wslTooling)) {
  if ($p -match "['`$]") { Fail "Unsupported character in path: $p" }
}

$cliArgs = @('--win-repo', "'$($wslRepo.Trim())'", '--commit', $commit.Trim(),
             '--base', $baseSha.Trim(), '--scope', $scope)
if ($toolList) { $cliArgs += @('--tools', $toolList) }
if ($dirty) { $cliArgs += '--dirty' }

# `|| exit 2`: a failed cd must not read as exit 1 ("findings").
wsl -d Ubuntu-24.04 -- bash -lc "cd '$($wslTooling.Trim())' || exit 2; python3 -m fork_check $($cliArgs -join ' ')"
$code = $LASTEXITCODE
if ($code -notin 0, 1, 2) { exit 2 }
exit $code
