# Audits the third_party submodules (osv-scanner) and GitHub Actions workflows
# (zizmor) at a commit of the repo in the current directory. See README.md.
# Exit: 0 clean, 1 findings, 2 checker broken.
param(
  # Any commit reachable from a local branch or HEAD.
  [string]$Ref = 'HEAD'
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

$repo = git rev-parse --show-toplevel
if ($LASTEXITCODE -or -not $repo) { Fail 'Run this from inside the xenia-canary repo.' }
$commit = git rev-parse --verify "$Ref^{commit}"
if ($LASTEXITCODE -or -not $commit) { Fail "Could not resolve $Ref." }

$wslRepo = wsl -d Ubuntu-24.04 -- wslpath -a "$($repo.Trim())"
$wslTooling = wsl -d Ubuntu-24.04 -- wslpath -a ($PSScriptRoot -replace '\\', '/')
if ($LASTEXITCODE -or -not $wslRepo -or -not $wslTooling) { Fail 'Could not reach the Ubuntu-24.04 WSL distro.' }
# Both paths go inside single quotes in bash, and wsl.exe expands `$`.
foreach ($p in @($wslRepo, $wslTooling)) {
  if ($p -match "['`$]") { Fail "Unsupported character in path: $p" }
}

# `|| exit 2`: a failed cd must not read as exit 1 ("findings").
wsl -d Ubuntu-24.04 -- bash -lc "cd '$($wslTooling.Trim())' || exit 2; python3 -m fork_check audit --win-repo '$($wslRepo.Trim())' --commit $($commit.Trim())"
$code = $LASTEXITCODE
if ($code -notin 0, 1, 2) { exit 2 }
exit $code
