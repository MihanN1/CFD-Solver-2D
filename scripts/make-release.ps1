<#
    Builds the release rows this machine can produce, then packs them.

    The whole matrix gets built. The installer lets the user turn each of the
    three switches on or off on its own, and it can only offer what is
    actually in dist\, so both sides of all three are built. AVX2 has to be
    split in any case: a binary cannot decide at runtime whether it may
    execute an AVX2 instruction, it just dies.

        windows-x64    AVX2 {on,off} x OpenMP {on,off} x CUDA {on,off} = 8
        windows-x86    AVX2 {on,off} x OpenMP {on,off}                 = 4

    There is no 32-bit CUDA and has not been since CUDA 9. "plain" - no AVX2,
    no OpenMP, no CUDA - is the row that runs on anything, and the only one
    with no vcomp140.dll beside it.

        pwsh -File scripts\make-release.ps1 -Version 0.1
        pwsh -File scripts\make-release.ps1 -Version 0.1 -WithInstallers
        pwsh -File scripts\make-release.ps1 -Version 0.1 -Only Package

    Needs Visual Studio with the C++ workload. The CUDA rows additionally need
    the CUDA Toolkit with its Visual Studio Integration component; without it
    they are reported as skipped and the rest still build. The CUDA
    architecture list is chosen from the installed toolkit version, so nothing
    has to be passed by hand.
#>

[CmdletBinding()]
param(
    [string] $Version   = "0.1",
    [string] $Generator = "Visual Studio 18 2026",
    [string] $CudaArchs = "",       # empty = decide from the installed toolkit
    [switch] $WithInstallers,       # off until the UI folders exist
    [switch] $Skip32,
    [ValidateSet("All","Build","Installers","Package")]
    [string] $Only = "All"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo "dist"
$rel  = Join-Path $repo "release\$Version"
$problems = New-Object System.Collections.Generic.List[string]

function Say($msg, $colour = "Cyan") { Write-Host $msg -ForegroundColor $colour }

# The lines that actually say what went wrong, or failing that the last few.
function Show-Tail($text) {
    $lines = $text -split "`r?`n" | Where-Object { $_.Trim() }
    $errs = $lines | Where-Object { $_ -match 'error|fatal|LNK\d|unresolved|Unsupported' }
    $show = if ($errs) { $errs | Select-Object -Last 12 } else { $lines | Select-Object -Last 12 }
    $show | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
}

# ---------------------------------------------------------------- toolkit ---
# CUDA 13 dropped Maxwell, Pascal and Volta, and nvcc errors out rather than
# warning when asked for one of them, so the list follows the toolkit.
function Resolve-CudaArchs {
    if ($CudaArchs) { return $CudaArchs }
    $nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
    if (-not $nvcc) { return "75;80;86;89;90" }
    $out = & nvcc --version 2>&1 | Out-String
    if ($out -match 'release\s+(\d+)\.') {
        if ([int]$Matches[1] -ge 13) {
            Write-Host "  CUDA $($Matches[1]).x detected: targeting Turing and newer" -ForegroundColor DarkGray
            return "75;80;86;89;90"
        }
        Write-Host "  CUDA $($Matches[1]).x detected: targeting Maxwell and newer" -ForegroundColor DarkGray
        return "50;60;61;70;75;80;86;89;90"
    }
    return "75;80;86;89;90"
}

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

# ----------------------------------------------------------- the 12 rows ---
$Rows = @()
foreach ($avx2 in $true, $false) {
    foreach ($omp in $true, $false) {
        foreach ($cuda in $true, $false) {
            $Rows += @{ Arch = "x64"; Avx2 = $avx2; OpenMp = $omp; Cuda = $cuda }
        }
        # No 32-bit CUDA exists, so that axis is not in the 32-bit rows.
        $Rows += @{ Arch = "Win32"; Avx2 = $avx2; OpenMp = $omp; Cuda = $false }
    }
}

function Build-Row($Row, $Archs) {
    $tags = @()
    if ($Row.Avx2)   { $tags += "avx2" }
    if ($Row.OpenMp) { $tags += "omp"  }
    if ($Row.Cuda)   { $tags += "cuda" }
    $feature = if ($tags.Count) { $tags -join "-" } else { "plain" }
    $label   = if ($Row.Arch -eq "x64") { "windows-x64" } else { "windows-x86" }
    $name    = "Fluid Solver $Version $label $feature"
    $build   = Join-Path $repo "build-$label-$feature"

    Write-Host "  $name ... " -NoNewline
    Remove-Item $build -Recurse -Force -ErrorAction SilentlyContinue

    $cmakeArgs = @("-S", $repo, "-B", $build, "-G", $Generator, "-A", $Row.Arch,
              "-DCFD_STATIC=ON",
              "-DCFD_ENABLE_AVX2=$(if($Row.Avx2){'ON'}else{'OFF'})",
              "-DCFD_ENABLE_OPENMP=$(if($Row.OpenMp){'ON'}else{'OFF'})",
              "-DCFD_ENABLE_CUDA=$(if($Row.Cuda){'ON'}else{'OFF'})")
    # Without these a missing toolkit or runtime quietly produces a binary that
    # would then be published under a name promising the feature it lacks.
    if ($Row.Cuda)   { $cmakeArgs += @("-DCFD_ENABLE_CUDA_EXPLICIT=ON", "-DCFD_CUDA_ARCHITECTURES=$Archs") }
    if ($Row.Cuda) {
        $nvccPath = & where nvcc 2>$null | Select-Object -First 1
        if (-not $nvccPath) {
            # Пробуем по CUDA_PATH
            $nvccPath = Get-ChildItem "$env:CUDA_PATH\bin\nvcc.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName
        }
        if ($nvccPath) {
            $cmakeArgs += "-DCMAKE_CUDA_COMPILER=`"$nvccPath`""
            Write-Host "      using nvcc: $nvccPath" -ForegroundColor DarkGray
        } else {
            Write-Host "      nvcc not found, this row will fail" -ForegroundColor Yellow
        }
    }
    if ($Row.OpenMp) { $cmakeArgs += "-DCFD_ENABLE_OPENMP_EXPLICIT=ON" }

    # The whole log goes to a file and the tail goes to the screen. Hiding a
    # compiler error behind a -Verbose nobody passes is not a summary, it is a
    # dead end.
    $logDir = Join-Path $repo "logs"
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $log = Join-Path $logDir "$label-$feature.log"

    $out = & cmake @cmakeArgs 2>&1 | Out-String
    $out | Set-Content $log
    if ($LASTEXITCODE -ne 0) {
        Write-Host "configure failed" -ForegroundColor Yellow
        Show-Tail $out
        Write-Host "      full log: $log" -ForegroundColor DarkGray
        $problems.Add("$name - configure failed, see logs\$label-$feature.log")
        return
    }
    $out = & cmake --build $build --config Release --parallel 2>&1 | Out-String
    $out | Add-Content $log
    if ($LASTEXITCODE -ne 0) {
        Write-Host "build failed" -ForegroundColor Yellow
        Show-Tail $out
        Write-Host "      full log: $log" -ForegroundColor DarkGray
        $problems.Add("$name - build failed, see logs\$label-$feature.log")
        return
    }

    $exe = Join-Path $build "bin\Release\Fluid Solver.exe"
    if (-not (Test-Path $exe)) { Write-Host "no executable" -ForegroundColor Yellow; $problems.Add("$name - no executable produced"); return }

    $rowDir = Join-Path $dist $name
    Remove-Item $rowDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $rowDir | Out-Null
    Copy-Item $exe (Join-Path $rowDir "Fluid Solver.exe") -Force

    # MSVC has no static OpenMP runtime; the build does not start without this.
    $extra = ""
    if ($Row.OpenMp) {
        $bits = if ($Row.Arch -eq "x64") { "x64" } else { "x86" }
        $dll = Find-Vcomp $bits
        if ($dll) { Copy-Item $dll $rowDir -Force; $extra = " + vcomp140.dll" }
        else { $problems.Add("$name - vcomp140.dll not found, this build will not start") }
    }

    $mb = [math]::Round((Get-Item (Join-Path $rowDir "Fluid Solver.exe")).Length / 1MB, 2)
    Write-Host "ok, $mb MB$extra" -ForegroundColor Green
}

function Build-All {
    Say "Building the executables"
    New-Item -ItemType Directory -Force -Path $dist | Out-Null

    $haveNvcc = [bool](Get-Command nvcc -ErrorAction SilentlyContinue)
    $archs = Resolve-CudaArchs

    if (-not $haveNvcc) {
        Write-Host "  (the four CUDA rows are skipped: nvcc is not on PATH)" -ForegroundColor Yellow
        $problems.Add("the four CUDA rows were skipped - nvcc is not on PATH, install the CUDA Toolkit with Visual Studio Integration")
    }

    foreach ($row in $Rows) {
        if ($row.Arch -eq "Win32" -and $Skip32) { continue }
        if ($row.Cuda -and -not $haveNvcc) { continue }
        Build-Row $row $archs
    }
}

# ------------------------------------------------------ the installers -----
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
        $any = Get-ChildItem $dist -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like "Fluid Solver $Version windows-$arch *" }
        if (-not $any) { Write-Host "  windows-$arch ... nothing to package - skipped" -ForegroundColor Yellow; continue }
        Write-Host "  windows-$arch ... " -NoNewline
        & $iscc "/DAppVersion=$Version" "/DArch=$arch" "/DDistDir=$dist" $iss 2>&1 | Out-String | Write-Verbose
        if ($LASTEXITCODE -eq 0) { Write-Host "ok" -ForegroundColor Green }
        else { Write-Host "failed" -ForegroundColor Yellow; $problems.Add("windows-$arch installer - iscc failed, rerun with -Verbose") }
    }
}

# --------------------------------------------------------- the release ----
function Package {
    Say "Assembling release\$Version"
    Remove-Item $rel -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $rel | Out-Null

    # Both shapes, not just folders: a Windows row is a directory because of the
    # DLL beside the exe, while a Linux or macOS row dropped in from the other
    # script is a single file. Filtering on -Directory is why those used to
    # vanish from the release without a word.
    Get-ChildItem $dist -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -like "Fluid Solver $Version *" -and
            $_.Name -notlike "*setup.exe" -and $_.Extension -notin ".zip", ".pkg", ".run"
        } |
        ForEach-Object {
            $stage = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid())
            $inner = Join-Path $stage $_.Name
            New-Item -ItemType Directory -Force -Path $inner | Out-Null
            if ($_.PSIsContainer) {
                Copy-Item "$($_.FullName)\*" $inner -Recurse -Force
            } else {
                Copy-Item $_.FullName (Join-Path $inner "Fluid Solver") -Force
            }
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

    # The two standalone files: what the release was built from, and how to use
    # it. lib\sfml belongs to the UI and dwarfs everything else.
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid())
    $srcDir = Join-Path $stage "Fluid-Solver-Source-Code"
    New-Item -ItemType Directory -Force -Path $srcDir | Out-Null
    # .toolchain holds a Linux virtualenv when the build ran through WSL or a
    # container. Its symlinks are unreadable from Windows and Compress-Archive
    # stops the whole run on the first one, so it never belongs in here.
    $skip = @(".git", ".github", ".vs", ".vscode", "out", "dist", "release",
              "_to_delete", ".toolchain", "logs")
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

# ------------------------------------------------------------------ run ---
Say "Fluid Solver $Version - Windows"
Write-Host ""
if ($Only -in "All","Build")      { Build-All; Write-Host "" }
if ($Only -eq "Installers" -or ($Only -eq "All" -and $WithInstallers)) { Build-Installers; Write-Host "" }
if ($Only -in "All","Package")    { Package;   Write-Host "" }

Say "Done"
if (Test-Path $rel) {
    Get-ChildItem $rel | ForEach-Object { "{0,10:N0} KB  {1}" -f ($_.Length / 1KB), $_.Name | Write-Host }
}
if (-not $WithInstallers -and $Only -eq "All") {
    Write-Host ""
    Write-Host "Installers were not built. Add -WithInstallers for those; the UI becomes a component of them once dist\ui-windows-<arch> exists." -ForegroundColor DarkGray
}
if ($problems.Count) {
    Write-Host ""
    Say "$($problems.Count) thing(s) did not work:" "Yellow"
    $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}
Write-Host ""
Write-Host "Linux and macOS rows are built by scripts/make-release.sh on those systems."
Write-Host "Drop their dist\ output in beside this one and rerun with -Only Package."
