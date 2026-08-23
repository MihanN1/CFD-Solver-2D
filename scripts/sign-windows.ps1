<#
    Authenticode-signs the files it is given.

        pwsh -File scripts\sign-windows.ps1 dist\*.exe
        pwsh -File scripts\sign-windows.ps1 -Require "dist\Fluid Solver 0.2 windows setup.exe"

    Where the certificate comes from, in the order it is looked for:

      CFD_SIGN_THUMBPRINT   a certificate already in this machine's store. This
                            is the one to use with a hardware token or an
                            EV certificate, which cannot be exported to a file
                            at all.
      CFD_SIGN_PFX          a .pfx file, with CFD_SIGN_PFX_PASSWORD beside it.
                            In CI the .pfx is a base64 secret written to a
                            temporary file by the workflow.

    And what it is timestamped with:

      CFD_SIGN_TIMESTAMP_URL   default http://timestamp.digicert.com

    Timestamping is not optional in practice. Without it every signature stops
    validating the day the certificate expires, and a release published today
    starts warning users in a year. With it the signature stays valid for as
    long as the timestamp authority's own certificate does.

    With no certificate configured this reports that and exits 0, so an
    unsigned build is still a build. -Require turns that into an error, which
    is what a release workflow wants.

    A certificate that IS configured and then fails to sign is always an error,
    with or without -Require: half a release signed is worse than none of it.
#>

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Paths,
    [switch] $Require
)

$ErrorActionPreference = "Stop"

# PowerShell 7.3 added $PSNativeCommandUseErrorActionPreference, and 7.4 turns it
# on by default: a native program exiting non-zero then throws, because
# ErrorActionPreference is Stop above. That would turn "signtool could not sign
# this one file" into an unhandled exception before the count below ever runs -
# so the exit codes are read by hand here, which is what this script is for.
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# Write-Error under ErrorActionPreference=Stop prints a stack trace and the
# offending source line, which buries the message in a CI log. These say the
# thing and stop.
function Stop-WithMessage([string] $Message) {
    [Console]::Error.WriteLine($Message)
    exit 1
}

function Write-NoteAndExit([string] $Message) {
    Write-Host $Message -ForegroundColor Yellow
    exit 0
}

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    # The Windows SDK installs one per version and puts none of them on PATH.
    # Newest first: an older signtool cannot produce a SHA-256 timestamp.
    $roots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem $root -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match "\\(x64|x86)\\" } |
               Sort-Object FullName -Descending |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$targets = @()
foreach ($pattern in $Paths) {
    if (-not $pattern) { continue }
    $targets += Get-ChildItem $pattern -File -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName }
}
$targets = $targets | Select-Object -Unique
if (-not $targets) {
    Write-Host "sign-windows: nothing to sign" -ForegroundColor DarkGray
    exit 0
}

$thumbprint = $env:CFD_SIGN_THUMBPRINT
$pfx = $env:CFD_SIGN_PFX
$pfxPassword = $env:CFD_SIGN_PFX_PASSWORD
$timestamp = if ($env:CFD_SIGN_TIMESTAMP_URL) {
    $env:CFD_SIGN_TIMESTAMP_URL
} else {
    "http://timestamp.digicert.com"
}

if (-not $thumbprint -and -not $pfx) {
    $message = @"
sign-windows: no code-signing certificate configured, so nothing was signed.

  Set CFD_SIGN_THUMBPRINT to a certificate in this machine's store, or
  CFD_SIGN_PFX (with CFD_SIGN_PFX_PASSWORD) to a .pfx file.

An unsigned executable still runs. What it costs is the SmartScreen prompt on
download - "Windows protected your PC", with the publisher shown as unknown -
until enough people have clicked through it. A standard code-signing
certificate replaces the unknown publisher with your name and lets that
reputation accumulate; an EV certificate skips the wait entirely.
"@
    if ($Require) { Stop-WithMessage $message }
    Write-NoteAndExit $message
}

$signtool = Find-SignTool
if (-not $signtool) {
    $message = "sign-windows: signtool.exe was not found. Install the Windows SDK (Signing Tools component)."
    if ($Require) { Stop-WithMessage $message }
    Write-NoteAndExit $message
}

$arguments = @("sign", "/fd", "sha256", "/tr", $timestamp, "/td", "sha256", "/v")
if ($thumbprint) {
    $arguments += @("/sha1", $thumbprint)
} else {
    if (-not (Test-Path $pfx)) {
        Stop-WithMessage "sign-windows: CFD_SIGN_PFX points at $pfx, which does not exist."
    }
    $arguments += @("/f", $pfx)
    if ($pfxPassword) { $arguments += @("/p", $pfxPassword) }
}

$failed = 0
foreach ($target in $targets) {
    Write-Host "  signing $([System.IO.Path]::GetFileName($target)) ... " -NoNewline
    & $signtool @arguments $target 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -eq 0) {
        Write-Host "ok" -ForegroundColor Green
    } else {
        Write-Host "failed" -ForegroundColor Yellow
        $failed++
    }
}

if ($failed -gt 0) {
    # A certificate was configured and signtool still refused: that is a fault,
    # not a choice, so it is an error either way. Shipping a release where some
    # files are signed and some are not is worse than shipping none signed.
    Stop-WithMessage "sign-windows: $failed file(s) could not be signed. Rerun with -Verbose for signtool's own output."
}

Write-Host "sign-windows: signed $($targets.Count) file(s), timestamped by $timestamp" -ForegroundColor Green
exit 0
