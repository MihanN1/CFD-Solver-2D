<#
    Everything Windows can produce for a release, in one run:

        every executable  ->  dist\Fluid Solver <ver> windows-<arch> <feature>\
        both installers   ->  dist\Fluid Solver <ver> windows-<arch> setup.exe
        the release folder->  release\<ver>\

    It does not tag anything, does not upload anything and does not touch git.

        pwsh -File scripts\make-release.ps1 -Version 0.1
        pwsh -File scripts\make-release.ps1 -Version 0.1 -SkipCuda
        pwsh -File scripts\make-release.ps1 -Version 0.1 -Only Package

    Needs: Visual Studio with the C++ workload, and Inno Setup 6 for the
    installers. The CUDA rows additionally need the CUDA Toolkit with its
    Visual Studio Integration component. Anything missing is reported and
    skipped rather than failing the run, and the summary at the end says what
    did not get built.
#>

[CmdletBinding()]
param(
    [string] $Version   = "0.1",
    [string] $Generator = "Visual Studio 18 2026",
    # CUDA 12.x covers sm_50..sm_90. CUDA 13 dropped Maxwell, Pascal and Volta,
    # so trim this to "75;80;86;89;90" if that is the toolkit installed.
    [string] $CudaArchs = "50;60;61;70;75;80;86;89;90",
    [switch] $SkipCuda,
    [switch] $Skip32,
    # Build, Installers, Package, or All
    [ValidateSet("All","Build","Installers","Package")]
    [string] $Only = "All"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo "dist"
$rel  = Join-Path $repo "release\$Version"
$problems = New-Object System.Collections.Generic.List[string]

function Say($msg, $colour = "Cyan") { Write-Host $msg -ForegroundColor $colour }

# ---------------------------------------------------------------- helpers ---
function Find-Vcomp($Bits) {
    $roots = @()
    if ($env:VCToolsRedistDir) { $roots += $env:VCToolsRedistDir }
    $roots += "${env:ProgramFiles}\Microsoft Visual Studio"
    $roots += "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem $root -Recurse -Filter vcomp140.dll -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match "\\$Bits\\" } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Find-Iscc {
    $c = Get-Command iscc -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                     "${env:ProgramFiles}\Inno Setup 6\ISCC.exe")) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

# ------------------------------------------------------------ 1. the exes ---
function Build-Row($Arch, $Avx2, $OpenMp, $Cuda) {
    $tags = @()
    if ($Avx2)   { $tags += "avx2" }
    if ($OpenMp) { $tags += "omp"  }
    if ($Cuda)   { $tags += "cuda" }
    $feature = if ($tags.Count) { $tags -join "-" } else { "plain" }
    $label   = if ($Arch -eq "x64") { "windows-x64" } else { "windows-x86" }
    $name    = "Fluid Solver $Version $label $feature"
    $build   = Join-Path $repo "build-$label-$feature"

    Write-Host "  $name ... " -NoNewline
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

    $cmakeArch = if ($Arch -eq "x64") { "x64" } else { "Win32" }
    $args = @("-S", $repo, "-B", $build, "-G", $Generator, "-A", $cmakeArch,
              "-DCFD_STATIC=ON",
              "-DCFD_ENABLE_AVX2=$(if($Avx2){'ON'}else{'OFF'})",
              "-DCFD_ENABLE_OPENMP=$(if($OpenMp){'ON'}else{'OFF'})",
              "-DCFD_ENABLE_CUDA=$(if($Cuda){'ON'}else{'OFF'})")
    # Without this a missing toolkit quietly produces a CPU-only binary that
    # would then be published under a name promising CUDA.
    if ($Cuda) { $args += @("-DCFD_ENABLE_CUDA_EXPLICIT=ON", "-DCFD_CUDA_ARCHITECTURES=$CudaArchs") }

    & cmake @args 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -ne 0) { Write-Host "configure failed" -ForegroundColor Yellow; $problems.Add("$name - configure failed"); return }
    & cmake --build $build --config Release --parallel 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -ne 0) { Write-Host "build failed" -ForegroundColor Yellow; $problems.Add("$name - build failed"); return }

    $exe = Join-Path $build "bin\Release\Fluid Solver.exe"
    if (-not (Test-Path $exe)) { Write-Host "no executable" -ForegroundColor Yellow; $problems.Add("$name - no executable produced"); return }

    $rowDir = Join-Path $dist $name
    Remove-Item $rowDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $rowDir | Out-Null
    Copy-Item $exe (Join-Path $rowDir "Fluid Solver.exe") -Force

    # MSVC has no static OpenMP runtime; the build does not start without this.
    if ($OpenMp) {
        $bits = if ($Arch -eq "x64") { "x64" } else { "x86" }
        $dll = Find-Vcomp $bits
        if ($dll) { Copy-Item $dll $rowDir -Force }
        else { $problems.Add("$name - vcomp140.dll not found, this build will not start") }
    }

    $mb = [math]::Round((Get-Item (Join-Path $rowDir "Fluid Solver.exe")).Length / 1MB, 2)
    Write-Host "ok, $mb MB" -ForegroundColor Green
}

function Build-All {
    Say "Building the executables"
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    foreach ($avx2 in $true, $false) {
        foreach ($omp in $true, $false) {
            foreach ($cuda in $true, $false) {
                if ($cuda -and $SkipCuda) { continue }
                Build-Row "x64" $avx2 $omp $cuda
            }
            if (-not $Skip32) { Build-Row "Win32" $avx2 $omp $false }
        }
    }
}

# ------------------------------------------------------ 2. the installers ---
function Build-Installers {
    Say "Building the installers"
    $iscc = Find-Iscc
    if (-not $iscc) {
        Write-Host "  Inno Setup not found - skipped. Get it from jrsoftware.org." -ForegroundColor Yellow
        $problems.Add("installers - Inno Setup (iscc) is not installed")
        return
    }
    $iss = Join-Path $repo "installer\windows\fluid-solver.iss"
    foreach ($arch in "x64", "x86") {
        if ($arch -eq "x86" -and $Skip32) { continue }
        # An installer with nothing to install is worse than no installer.
        $any = Get-ChildItem $dist -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like "Fluid Solver $Version windows-$arch *" }
        if (-not $any) {
            Write-Host "  windows-$arch ... no builds to package - skipped" -ForegroundColor Yellow
            continue
        }
        Write-Host "  windows-$arch ... " -NoNewline
        & $iscc "/DAppVersion=$Version" "/DArch=$arch" "/DDistDir=$dist" $iss 2>&1 | Out-String | Write-Verbose
        if ($LASTEXITCODE -eq 0) { Write-Host "ok" -ForegroundColor Green }
        else { Write-Host "failed" -ForegroundColor Yellow; $problems.Add("windows-$arch installer - iscc failed, rerun with -Verbose") }
    }
}

# --------------------------------------------------------- 3. the release ---
function Package {
    Say "Assembling release\$Version"
    Remove-Item $rel -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $rel | Out-Null

    # One zip per variant. Each holds the executable under its plain name, so
    # unzipping anywhere and running it just works.
    Get-ChildItem $dist -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "Fluid Solver $Version *" } |
        ForEach-Object {
            $stage = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid())
            $inner = Join-Path $stage $_.Name
            New-Item -ItemType Directory -Force -Path $inner | Out-Null
            Copy-Item "$($_.FullName)\*" $inner -Recurse -Force
            foreach ($f in "README.md", "LICENSE") {
                if (Test-Path (Join-Path $repo $f)) { Copy-Item (Join-Path $repo $f) $inner }
            }
            New-Item -ItemType Directory -Force -Path (Join-Path $inner "output") | Out-Null
            Compress-Archive -Path $inner -DestinationPath (Join-Path $rel "$($_.Name).zip") -Force
            Remove-Item $stage -Recurse -Force
            Write-Host "  $($_.Name).zip"
        }

    Get-ChildItem $dist -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*setup.exe" -or $_.Extension -in ".pkg", ".run" } |
        ForEach-Object { Copy-Item $_.FullName $rel; Write-Host "  $($_.Name)" }

    # The two standalone files: what the release was built from, and how to
    # use it. lib\sfml is the UI's dependency and dwarfs everything else.
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid())
    $srcDir = Join-Path $stage "Fluid-Solver-Source-Code"
    New-Item -ItemType Directory -Force -Path $srcDir | Out-Null
    $skip = @(".git", ".github", ".vs", ".vscode", "out", "dist", "release", "sfml")
    Get-ChildItem $repo -Force | Where-Object {
        $_.Name -notin $skip -and $_.Name -notlike "build*" -and $_.Name -notlike "bt-*"
    } | ForEach-Object { Copy-Item $_.FullName $srcDir -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item (Join-Path $srcDir "lib\sfml") -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $srcDir "output") -Filter *.vtk -ErrorAction SilentlyContinue | Remove-Item -Force
    Compress-Archive -Path $srcDir -DestinationPath (Join-Path $rel "Fluid-Solver-Source-Code.zip") -Force
    Remove-Item $stage -Recurse -Force
    Write-Host "  Fluid-Solver-Source-Code.zip"

    Copy-Item (Join-Path $repo "README.md") $rel -Force
    Write-Host "  README.md"

    Get-ChildItem $rel -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
        "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
    } | Set-Content (Join-Path $rel "SHA256SUMS.txt") -Encoding ascii
}

# ------------------------------------------------------------------- run ---
Say "Fluid Solver $Version - Windows release"
Write-Host ""
if ($Only -in "All","Build")      { Build-All;        Write-Host "" }
if ($Only -in "All","Installers") { Build-Installers; Write-Host "" }
if ($Only -in "All","Package")    { Package;          Write-Host "" }

Say "Done"
if (Test-Path $rel) {
    Get-ChildItem $rel | ForEach-Object {
        "{0,10:N0} KB  {1}" -f ($_.Length / 1KB), $_.Name | Write-Host
    }
}
if ($problems.Count) {
    Write-Host ""
    Say "$($problems.Count) thing(s) did not work:" "Yellow"
    $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "Linux and macOS rows are not built here - run scripts/make-release.sh"
    Write-Host "on those, drop their output into dist\, and rerun with -Only Package."
}
