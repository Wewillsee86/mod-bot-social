<#
.SYNOPSIS
  mod-bot-social: patch installer for mod-playerbots.

.DESCRIPTION
  mod-bot-social runs on its own. Two optional features reach into
  mod-playerbots, because that is where a bot actually decides something.
  Those changes are shipped as patches under patches/.

  There is deliberately no pre-patched copy of the target files to fall
  back on. A copy would overwrite whatever mod-playerbots changed since -
  silently, fixes included. A patch that no longer fits stops and says so,
  naming the file and the line, and that failure is the useful part.

.PARAMETER PlayerbotsPath
  Path to mod-playerbots. Defaults to the sibling folder next to this module.

.PARAMETER Check
  Only test whether the patches would apply. Changes nothing.

.PARAMETER Revert
  Undo the patches instead of applying them.

.PARAMETER SkipPause
  Do not wait for a keypress at the end (used by the .bat wrapper).

.PARAMETER ConfigPath
  Path to playerbots.conf. Only read, never written. Used to check that
  mod-playerbots' own guild creation is off. Guessed if not given.

.EXAMPLE
  .\apply-playerbots-patches.ps1 -Check
  .\apply-playerbots-patches.ps1
  .\apply-playerbots-patches.ps1 -Revert
  .\apply-playerbots-patches.ps1 -PlayerbotsPath D:\ac\modules\mod-playerbots
#>

[CmdletBinding()]
param(
    [string] $PlayerbotsPath,
    [string] $ConfigPath,
    [switch] $Check,
    [switch] $Revert,
    [switch] $SkipPause
)

$ErrorActionPreference = 'Stop'

function Info($m) { Write-Host "[INFO] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[OK]   $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red }
function Note($m) { Write-Host "       $m" -ForegroundColor DarkGray }

function Stop-Here([int] $Code)
{
    if (-not $SkipPause) { Write-Host ""; Read-Host "Enter zum Schliessen" | Out-Null }
    exit $Code
}

# git writes progress to stderr. With ErrorActionPreference = Stop, PowerShell
# turns that into a terminating error even though nothing went wrong, so every
# git call goes through here.
function Invoke-Git
{
    param([Parameter(Mandatory)][string[]] $GitArgs)

    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try
    {
        $text = (& git -C $PlayerbotsPath @GitArgs 2>&1 | Out-String)
        return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text.Trim() }
    }
    finally { $ErrorActionPreference = $prev }
}

Write-Host ""
Write-Host "[mod-bot-social] Playerbots patch installer" -ForegroundColor Cyan
Write-Host ""

$scriptDir = Split-Path -Parent $PSCommandPath
if (-not $PlayerbotsPath)
{
    # modules/mod-bot-social -> modules -> modules/mod-playerbots
    $PlayerbotsPath = Join-Path (Split-Path -Parent $scriptDir) 'mod-playerbots'
}

if (-not (Test-Path -LiteralPath $PlayerbotsPath))
{
    Fail "mod-playerbots not found at $PlayerbotsPath"
    Note "Clone mod-playerbots first, or pass -PlayerbotsPath <path>."
    Stop-Here 1
}

if (-not (Get-Command git -ErrorAction SilentlyContinue))
{
    Fail "git.exe not found in PATH."
    Note "Install Git for Windows: https://git-scm.com/download/win"
    Note "git is used only for 'git apply'; the target need not be a checkout."
    Stop-Here 1
}

$patchesDir = Join-Path $scriptDir 'patches'
$patches = @(Get-ChildItem -LiteralPath $patchesDir -Filter '*.patch' -ErrorAction SilentlyContinue | Sort-Object Name)

if ($patches.Count -eq 0)
{
    Ok "No patches in $patchesDir - nothing to do"
    Stop-Here 0
}

# Reverting runs in reverse order, so patches touching the same file
# do not block each other.
if ($Revert) { $patches = @($patches | Sort-Object Name -Descending) }

$mode = if ($Revert) { 'revert' } elseif ($Check) { 'check' } else { 'apply' }
Info "Mode:   $mode"
Info "Target: $PlayerbotsPath"
Write-Host ""

$done = 0
$failed = 0

foreach ($p in $patches)
{
    # Convention: NN-<target>-<description>.patch
    if ($p.BaseName -match '^\d{2}-([^-]+)' -and $matches[1] -ne 'playerbots')
    {
        Warn "$($p.Name) targets '$($matches[1])', not playerbots - skipping"
        continue
    }

    $forward  = @('apply', '--whitespace=nowarn')
    $backward = @('apply', '--whitespace=nowarn')
    if ($Revert) { $forward += '--reverse' } else { $backward += '--reverse' }

    $fwd = Invoke-Git ($forward + @('--check', $p.FullName))

    if ($fwd.Code -ne 0)
    {
        # Maybe the tree is already in the desired state. Then the patch
        # checks out in the opposite direction. No hardcoded marker strings -
        # this works for any patch added later.
        $bwd = Invoke-Git ($backward + @('--check', $p.FullName))
        if ($bwd.Code -eq 0)
        {
            $state = if ($Revert) { 'was not applied' } else { 'already applied' }
            Ok "$($p.Name) - $state, nothing to do"
            $done++
            continue
        }

        Fail "$($p.Name) does not fit this revision of mod-playerbots"
        if ($fwd.Text) { Note $fwd.Text }
        Note "This is the good kind of failure: upstream changed something."
        Note "Inspect the conflict, then regenerate the patch:"
        Note "  git -C `"$PlayerbotsPath`" apply --3way `"$($p.FullName)`""
        $failed++
        continue
    }

    if ($Check) { Ok "$($p.Name) - would apply cleanly"; $done++; continue }

    $run = Invoke-Git ($forward + @($p.FullName))
    if ($run.Code -eq 0)
    {
        Ok "$($p.Name) - $mode done"
        $done++
    }
    else
    {
        Fail "$($p.Name) - git apply failed"
        if ($run.Text) { Note $run.Text }
        $failed++
    }
}

# ---------------------------------------------------------------------------
# mod-playerbots creates guilds itself at server start and stuffs arbitrary
# bots into them - level 1 characters that never met. BotSocial.Found is
# meant to replace exactly that, so the two must not both be on.
#
# We do NOT write playerbots.conf. It belongs to another module, and
# something that silently edits foreign settings is the kind of surprise
# you find weeks later. We read it and say so.
#
# The module repeats this check at every server start (LOG_ERROR), but by
# then playerbots has already created the guilds. Here it is still cheap.
Write-Host ""

if (-not $ConfigPath)
{
    # Common layouts, most specific first. $serverRoot guesses the sibling
    # 'server' folder next to the azerothcore checkout.
    $acRoot     = Split-Path -Parent (Split-Path -Parent $scriptDir)   # ...\modules -> ...\azerothcore-wotlk
    $serverRoot = Split-Path -Parent $acRoot
    $candidates = @(
        (Join-Path $serverRoot 'server\configs\modules\playerbots.conf'),
        (Join-Path $acRoot     'env\dist\etc\modules\playerbots.conf'),
        (Join-Path $serverRoot 'configs\modules\playerbots.conf')
    )
    $ConfigPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}

if (-not $ConfigPath -or -not (Test-Path -LiteralPath $ConfigPath))
{
    Warn "playerbots.conf not found - could not verify the guild setting."
    Note "Check by hand, or pass -ConfigPath <path to playerbots.conf>:"
    Note "  AiPlayerbot.RandomBotGuildCount = 0"
    Note "Otherwise mod-playerbots creates its own guilds at server start"
    Note "and BotSocial.Found has nothing left to do."
}
else
{
    # Last uncommented assignment wins, same as the core's config reader.
    $guildCount = $null
    foreach ($line in Get-Content -LiteralPath $ConfigPath)
    {
        if ($line -match '^\s*AiPlayerbot\.RandomBotGuildCount\s*=\s*(-?\d+)')
        {
            $guildCount = [int] $matches[1]
        }
    }

    if ($null -eq $guildCount)
    {
        Warn "AiPlayerbot.RandomBotGuildCount is not set in $ConfigPath"
        Note "The playerbots default is not 0 - set it explicitly:"
        Note "  AiPlayerbot.RandomBotGuildCount = 0"
    }
    elseif ($guildCount -ne 0)
    {
        Warn "AiPlayerbot.RandomBotGuildCount = $guildCount"
        Note "mod-playerbots will create $guildCount guilds at server start and"
        Note "put arbitrary bots in them - level 1 characters that never met."
        Note "BotSocial.Found is meant to replace that. Set it to 0 in"
        Note "  $ConfigPath"
        Note "or turn BotSocial.Found.Enable off. Both at once makes no sense."
        Note "This file is not written by the installer - change it yourself."
    }
    else
    {
        Ok "AiPlayerbot.RandomBotGuildCount = 0 - guilds are left to BotSocial.Found"
    }
}

Write-Host ""
Write-Host "[SUMMARY] ok: $done / failed: $failed" -ForegroundColor Cyan

if ($failed -gt 0)
{
    Warn "Some patches did not apply. The server still runs -"
    Warn "the affected optional features simply stay off."
    Warn "See README.md, section 'Patch doesn't apply'."
    Stop-Here 1
}

if (-not $Check)
{
    Info "Now rebuild AND install mod-playerbots. Building alone is not enough."
}
else
{
    Info "Nothing was changed."
}

Stop-Here 0
