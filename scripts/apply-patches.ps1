# Applies PolyTrack's iPlug2 changes (patches/iPlug2.patch) to the pinned iPlug2 submodule.
# Safe to run more than once: an already applied patch is skipped.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
git -C $root submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { Write-Error "git submodule update failed"; exit 1 }
$patch = Join-Path $root 'patches\iPlug2.patch'
$dir = Join-Path $root 'iPlug2'
$null = cmd /c "git -C `"$dir`" apply --reverse --check --whitespace=nowarn `"$patch`" 2>nul"
if ($LASTEXITCODE -eq 0) { Write-Host "iPlug2 : already applied"; exit 0 }
cmd /c "git -C `"$dir`" apply --whitespace=nowarn `"$patch`" 2>nul"
if ($LASTEXITCODE -ne 0) { Write-Error "iPlug2 : patch does not apply. Is the submodule at the pinned commit? (git submodule status)"; exit 1 }
Write-Host "iPlug2 : applied"
