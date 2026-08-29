# =========================================================================
# mod-bot-social: Patch installer for mod-playerbots
# =========================================================================
param(
    [switch]$SkipPause
)

$ErrorActionPreference = "Stop"

function Info($msg)  { Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Ok($msg)    { Write-Host "[OK]   $msg" -ForegroundColor Green }
function Warn($msg)  { Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Fail($msg)  { Write-Host "[FAIL] $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "[mod-bot-social] Playerbots Patch Installer" -ForegroundColor Cyan
Write-Host ""

$scriptDir = Split-Path -Parent $PSCommandPath
$modulesRoot = Split-Path -Parent $scriptDir
$playerbotsDir = Join-Path $modulesRoot "mod-playerbots"

# --- Check if mod-playerbots exists ---
if (-not (Test-Path (Join-Path $playerbotsDir ".git"))) {
    Fail "mod-playerbots not found at $playerbotsDir"
    Fail "Clone mod-playerbots first, then re-run this script."
    if (-not $SkipPause) { pause }
    exit 1
}

# --- Check git is available ---
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "git.exe not found in PATH."
    Fail "Install Git for Windows from https://git-scm.com/download/win"
    if (-not $SkipPause) { pause }
    exit 1
}

# --- Apply patches ---
$patchesDir = Join-Path $scriptDir "patches"
$appliedCount = 0
$failedCount = 0

$patches = Get-ChildItem -LiteralPath $patchesDir -Filter "*.patch" -ErrorAction SilentlyContinue

if (-not $patches) {
    Ok "No patches found in $patchesDir - nothing to do"
    if (-not $SkipPause) { pause }
    exit 0
}

foreach ($patch in $patches) {
    Info "Processing $($patch.Name)..."
    
    # Extract target slug from filename (02-playerbots-...)
    if ($patch.BaseName -notmatch '^\d{2}-(.+?)(?:-.+)?$') {
        Warn "Cannot parse target from $($patch.Name) - skipping"
        continue
    }
    $slug = $matches[1]
    
    if ($slug -ne "playerbots") {
        Warn "Patch targets $slug, not playerbots - skipping"
        continue
    }
    
    # Extract target file from patch (+++ b/...).
    # git writes an optional timestamp after a TAB on that header line, so we
    # stop at the first TAB rather than swallowing the rest of the line -
    # otherwise the path carries illegal characters and Test-Path throws.
    $targetFile = $null
    foreach ($line in (Get-Content -LiteralPath $patch.FullName)) {
        if ($line -match '^\+\+\+\s+b/([^\t]+)') {
            $targetFile = $matches[1].Trim()
            break
        }
    }
    
    if (-not $targetFile) {
        Warn "Cannot determine target file from $($patch.Name) - skipping"
        continue
    }
    
    $fullTarget = Join-Path $playerbotsDir $targetFile
    
    # Check for applied markers (hardcoded per patch; could parse manifest.json)
    $alreadyApplied = $false
    if (Test-Path $fullTarget) {
        $content = Get-Content -LiteralPath $fullTarget -Raw -ErrorAction SilentlyContinue
        if ($content) {
            # Marker for 02-playerbots-guildcache-nullcheck.patch
            if ($content -match [regex]::Escape("outlive its leader's character row")) {
                $alreadyApplied = $true
            }
            # Marker for 01-playerbots-reactdelay.patch
            if ($content -match [regex]::Escape("Koennen pro Bot statt einheitlich")) {
                $alreadyApplied = $true
            }
        }
    }
    
    if ($alreadyApplied) {
        Ok "Already applied - skipping"
        $appliedCount++
        continue
    }
    
    # Try git apply. We use 'git -C <dir>' (not Push-Location) because
    # the latter can leave the shell in a state where subsequent git calls
    # hang waiting for a TTY - a known pain point on Windows.
    $applied = $false
    & git -C $playerbotsDir apply --check --whitespace=nowarn $patch.FullName 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        & git -C $playerbotsDir apply --whitespace=nowarn $patch.FullName 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Ok "Applied via git apply"
            $appliedCount++
            $applied = $true
        }
    }
    
    if (-not $applied) {
        # Fallback: copy pre-patched file. The mirror tree sits under
        # patches/mod-<slug>/<path>, not patches/<slug>/<path> - we mirror
        # the actual modules/ layout so the convention is obvious.
        $prepatchedFile = Join-Path $patchesDir ("mod-" + $slug) $targetFile
        if (Test-Path $prepatchedFile) {
            $targetDir = Split-Path -Parent $fullTarget
            if (-not (Test-Path $targetDir)) {
                New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
            }
            Copy-Item -LiteralPath $prepatchedFile -Destination $fullTarget -Force
            Ok "Applied via pre-patched copy"
            $appliedCount++
        } else {
            Fail "Could not apply - no pre-patched file at $prepatchedFile"
            $failedCount++
        }
    }
}

Write-Host ""
Write-Host "[SUMMARY] Applied: $appliedCount / Failed: $failedCount" -ForegroundColor Cyan
Write-Host ""

if ($failedCount -gt 0) {
    Warn "Some patches failed. See README.md for manual installation steps."
}

if (-not $SkipPause) { pause }
exit 0
