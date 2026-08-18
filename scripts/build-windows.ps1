<#
    Builds every Windows row of the release matrix on this machine.

    x64  : AVX2 {on,off} x OpenMP {on,off} x CUDA {on,off}  = 8
    Win32: AVX2 {on,off} x OpenMP {on,off}                  = 4
           (there is no 32-bit CUDA and has not been since CUDA 9)

    Needs Visual Studio with the C++ workload, and the CUDA Toolkit with its
    Visual Studio Integration component for the CUDA rows. Rows whose
    prerequisites are missing are reported and skipped, not failed.

        pwsh -File scripts/build-windows.ps1
        pwsh -File scripts/build-windows.ps1 -SkipCuda
        pwsh -File scripts/build-windows.ps1 -Generator "Visual Studio 17 2022"
#>

[CmdletBinding()]
param(
    [string]   $Generator  = "Visual Studio 18 2026",
    [string]   $Version    = "0.1.0",
    [string]   $OutDir     = "dist",
    # CUDA 12.x covers sm_50..sm_90. CUDA 13 dropped everything below Turing,
    # so drop 50;60;61;70 from this list if that is the toolkit installed.
    [string]   $CudaArchs  = "75;80;86;89;90",
    [switch]   $SkipCuda
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# vcomp140.dll is the one runtime MSVC cannot link statically, so the OpenMP
# rows carry it beside them. Newer toolsets bump the VC1xx directory name.
function Find-VcompDll {
    $roots = @()
    if ($env:VCToolsRedistDir) { $roots += $env:VCToolsRedistDir }
    $roots += Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue |
              ForEach-Object { Get-ChildItem "$($_.FullName)\*\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue } |
              ForEach-Object { $_.FullName }
    foreach ($root in $roots) {
        $hit = Get-ChildItem $root -Recurse -Filter "vcomp140.dll" -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match "\\x64\\" } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

function Build-Row {
    param($Arch, $Avx2, $OpenMp, $Cuda)

    $tags = @()
    if ($Avx2)   { $tags += "avx2" }
    if ($OpenMp) { $tags += "omp"  }
    if ($Cuda)   { $tags += "cuda" }
    $feature = if ($tags.Count) { $tags -join "-" } else { "plain" }
    $label   = if ($Arch -eq "x64") { "windows-x64" } else { "windows-x86" }
    $name    = "Fluid Solver $Version $label $feature"
    $build   = Join-Path $repo "build-$label-$feature"

    Write-Host "==> $name" -ForegroundColor Cyan
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

    $cmakeArch = if ($Arch -eq "x64") { "x64" } else { "Win32" }
    $args = @(
        "-S", $repo, "-B", $build,
        "-G", $Generator, "-A", $cmakeArch,
        "-DCFD_STATIC=ON",
        "-DCFD_ENABLE_AVX2=$(if($Avx2){'ON'}else{'OFF'})",
        "-DCFD_ENABLE_OPENMP=$(if($OpenMp){'ON'}else{'OFF'})",
        "-DCFD_ENABLE_CUDA=$(if($Cuda){'ON'}else{'OFF'})"
    )
    # Without this a missing toolkit silently produces a CPU-only binary that
    # would then be published under a name promising CUDA.
    if ($Cuda) {
        $args += "-DCFD_ENABLE_CUDA_EXPLICIT=ON"
        $args += "-DCFD_CUDA_ARCHITECTURES=$CudaArchs"
    }

    & cmake @args 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    configure failed - skipped" -ForegroundColor Yellow
        return
    }
    & cmake --build $build --config Release --parallel 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    build failed - skipped" -ForegroundColor Yellow
        return
    }

    $exe = Join-Path $build "bin\Release\Fluid Solver.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "    no executable produced - skipped" -ForegroundColor Yellow
        return
    }

    # One folder per row, so the OpenMP rows can carry their DLL and the whole
    # folder is what gets zipped or handed to the installer.
    $rowDir = Join-Path $dist $name
    New-Item -ItemType Directory -Force -Path $rowDir | Out-Null
    Copy-Item $exe (Join-Path $rowDir "Fluid Solver.exe") -Force

    if ($OpenMp) {
        $vcomp = Find-VcompDll
        if ($vcomp) {
            Copy-Item $vcomp $rowDir -Force
            Write-Host "    + vcomp140.dll" -ForegroundColor DarkGray
        } else {
            Write-Host "    vcomp140.dll not found - this build will not start without it" -ForegroundColor Yellow
        }
    }

    $size = [math]::Round((Get-Item (Join-Path $rowDir "Fluid Solver.exe")).Length / 1MB, 2)
    Write-Host "    ok, $size MB" -ForegroundColor Green
}

$rows = @()
foreach ($avx2 in $true, $false) {
    foreach ($omp in $true, $false) {
        foreach ($cuda in $true, $false) {
            if ($cuda -and $SkipCuda) { continue }
            $rows += ,@("x64", $avx2, $omp, $cuda)
        }
        $rows += ,@("Win32", $avx2, $omp, $false)
    }
}

foreach ($row in $rows) { Build-Row -Arch $row[0] -Avx2 $row[1] -OpenMp $row[2] -Cuda $row[3] }

Write-Host ""
Write-Host "Built into $dist :" -ForegroundColor Cyan
Get-ChildItem $dist -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
