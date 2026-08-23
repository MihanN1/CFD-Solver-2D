; Inno Setup script for Fluid Solver.
;
; ONE installer for every Windows architecture and every feature variant built
; for them. It works out what this machine can use - AVX2, an NVIDIA driver,
; more than one core - and installs the matching build as "Fluid Solver.exe",
; so shortcuts, the UI and any script the user writes never have to know which
; one it is. Nothing is asked unless the user opens "Let me choose" himself.
;
; Build it with:
;   iscc /DAppVersion=0.2 /DDistDir=..\..\dist installer\windows\fluid-solver.iss
;
; and, for a smaller download carrying one architecture only:
;   iscc /DAppVersion=0.2 /DArch=x64 /DDistDir=..\..\dist installer\windows\fluid-solver.iss
;
; DistDir must contain the folders make-release.ps1 produced:
;   Fluid Solver <ver> windows-<arch> <feature>\Fluid Solver.exe
;   Fluid Solver <ver> windows-<arch> <feature>-ui\...   (optional, the UI)
;
; <arch> is x64, x86 or arm64. The three are compiled into the same file and
; picked at run time, because a person downloading a solver should not have to
; know what is inside their laptop. An arm64 machine takes the arm64 build when
; there is one and the x64 build otherwise - Windows 11 on ARM emulates x64,
; slower but working - and an x64 machine falls back to x86 the same way.
;
; The "-ui" folder is a complete install of its own - solver, UI, DLLs and
; output\ in one place, because the UI is a shell that starts the solver. So
; ticking the UI installs that folder INSTEAD of the plain one, never both.
;
; Only the variants actually present in DistDir are compiled in, and the manual
; page offers a switch only when the payload has builds on BOTH sides of it -
; which is how a 32-bit-only payload ends up without a CUDA box without
; anything being written twice.

#ifndef AppVersion
  #define AppVersion "0.2"
#endif
; "all" - the default - is every architecture in one file. x64, x86 or arm64
; builds a single-architecture installer instead.
#ifndef Arch
  #define Arch "all"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist"
#endif
#define AppName "Fluid Solver"
#define Publisher "MihanN1"
#define AppUrl "https://github.com/MihanN1/CFD-Solver-2D"

#define WantArch(str A) ((Arch == "all") || (Arch == A))
#define VariantDir(str A, str F) \
    DistDir + "\" + AppName + " " + AppVersion + " windows-" + A + " " + F
#define UiDir(str A, str F) VariantDir(A, F) + "-ui"
; Both halves matter: an architecture the caller excluded must not be compiled
; in even when its folder is sitting in DistDir.
#define HaveV(str A, str F) (WantArch(A) && DirExists(VariantDir(A, F)))
#define HaveU(str A, str F) (WantArch(A) && DirExists(UiDir(A, F)))

#define HaveArch(str A) ( \
    HaveV(A, 'avx2-omp-cuda') || HaveV(A, 'avx2-omp') || HaveV(A, 'avx2-cuda') || \
    HaveV(A, 'avx2') || HaveV(A, 'omp-cuda') || HaveV(A, 'omp') || \
    HaveV(A, 'cuda') || HaveV(A, 'plain'))
#define HaveUiArch(str A) ( \
    HaveU(A, 'avx2-omp-cuda') || HaveU(A, 'avx2-omp') || HaveU(A, 'avx2-cuda') || \
    HaveU(A, 'avx2') || HaveU(A, 'omp-cuda') || HaveU(A, 'omp') || \
    HaveU(A, 'cuda') || HaveU(A, 'plain'))

#define HaveAny (HaveArch('x64') || HaveArch('x86') || HaveArch('arm64'))
#define HaveUi (HaveUiArch('x64') || HaveUiArch('x86') || HaveUiArch('arm64'))

#if !HaveAny
  #error "No 'Fluid Solver <ver> windows-<arch> <feature>' folder found in DistDir. Run scripts\make-release.ps1 first."
#endif

; The name says what is in it. A single-architecture build keeps the old name
; so an existing link to it still means the same thing.
#if Arch == "all"
  #define OutputName AppName + " " + AppVersion + " windows setup"
#else
  #define OutputName AppName + " " + AppVersion + " windows-" + Arch + " setup"
#endif

[Setup]
AppId={{7C4A1F62-3B58-4E2D-9A7E-0F1D2C3B4A50}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#Publisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
VersionInfoVersion={#AppVersion}.0.0
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir={#DistDir}
OutputBaseFilename={#OutputName}
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\logo\fluid-solver.ico
WizardImageFile=..\..\logo\wizard-164x314.bmp,..\..\logo\wizard-410x797.bmp
WizardSmallImageFile=..\..\logo\wizard-small-55x55.bmp,..\..\logo\wizard-small-138x140.bmp
UninstallDisplayIcon={app}\Fluid Solver.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
; Per-user by default and no UAC prompt. This is deliberate: the solver writes
; its frames into an "output" folder beside its own executable, and a standard
; user cannot write inside Program Files. lowest keeps that promise. The user
; can still point the installer at Program Files, and the solver then falls
; back to the per-user data directory and says so.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Windows 7 and newer. (Unrelated to Inno's own version - but note that the
; "x86compatible"/"arm64" spellings below need Inno Setup 6.3 or newer to
; compile at all.)
MinVersion=6.1
#if (Arch == "all") || (Arch == "x64") || (Arch == "arm64")
ArchitecturesAllowed=x86compatible or arm64
ArchitecturesInstallIn64BitMode=x64compatible or arm64
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian";  MessagesFile: "compiler:Languages\Russian.isl"

[Types]
#if HaveUi
Name: "full";   Description: "Solver, desktop UI and example models"
#else
Name: "full";   Description: "Solver and example models"
#endif
Name: "solver"; Description: "Solver only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "solver"; Description: "Fluid Solver (required)"; Types: full solver custom; Flags: fixed
#if HaveUi
Name: "ui";     Description: "Desktop UI - configures runs, launches the solver and draws the frames"; Types: full
#endif
Name: "models"; Description: "Example models"; Types: full

[Tasks]
Name: "desktopicon";  Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startmenu";    Description: "Create a Start Menu shortcut"; GroupDescription: "{cm:AdditionalIcons}"
; There is deliberately no "Pin to the taskbar" task here any more. Windows 11
; removed the shell verb that made it possible, and every installer that still
; offers the box is either failing silently or lying. The last page says how to
; do it by hand instead, which is two clicks and always works.
#if HaveUi
; Where a shortcut goes and what it starts are two different questions, so they
; are two groups. This second one only appears when the UI is actually being
; installed - without it there is a single program to point at and nothing to
; choose between. Both boxes apply to both places above at once.
Name: "iconui";      Description: "Fluid Solver UI - the window"; GroupDescription: "Which program the shortcuts start:"; Components: ui
Name: "iconconsole"; Description: "Fluid Solver - the console version, where every parameter is typed at the prompt"; GroupDescription: "Which program the shortcuts start:"; Components: ui; Flags: unchecked
#endif
Name: "addtopath";    Description: "Add Fluid Solver to PATH (lets you run it from any terminal)"; Flags: unchecked
#if HaveUi
Name: "associatevtk"; Description: "Open .vtk files with the Fluid Solver UI"; Components: ui; Flags: unchecked
#endif

[Files]
; Exactly one of these is installed, chosen by ActiveArch and SelectedFeature.
; Each line only exists if that build is in DistDir. WantSolverOnly, not
; WantVariant: when the UI component is ticked the "-ui" row further down
; installs instead, because it already contains this same executable.
#if HaveV('x64', 'avx2-omp-cuda')
Source: "{#VariantDir('x64','avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|avx2-omp-cuda'); Components: solver
#endif
#if HaveV('x64', 'avx2-omp')
Source: "{#VariantDir('x64','avx2-omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|avx2-omp'); Components: solver
#endif
#if HaveV('x64', 'avx2-cuda')
Source: "{#VariantDir('x64','avx2-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|avx2-cuda'); Components: solver
#endif
#if HaveV('x64', 'avx2')
Source: "{#VariantDir('x64','avx2')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|avx2'); Components: solver
#endif
#if HaveV('x64', 'omp-cuda')
Source: "{#VariantDir('x64','omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|omp-cuda'); Components: solver
#endif
#if HaveV('x64', 'omp')
Source: "{#VariantDir('x64','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|omp'); Components: solver
#endif
#if HaveV('x64', 'cuda')
Source: "{#VariantDir('x64','cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|cuda'); Components: solver
#endif
#if HaveV('x64', 'plain')
Source: "{#VariantDir('x64','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x64|plain'); Components: solver
#endif

#if HaveV('x86', 'avx2-omp')
Source: "{#VariantDir('x86','avx2-omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x86|avx2-omp'); Components: solver
#endif
#if HaveV('x86', 'avx2')
Source: "{#VariantDir('x86','avx2')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x86|avx2'); Components: solver
#endif
#if HaveV('x86', 'omp')
Source: "{#VariantDir('x86','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x86|omp'); Components: solver
#endif
#if HaveV('x86', 'plain')
Source: "{#VariantDir('x86','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('x86|plain'); Components: solver
#endif

; No AVX2 and no CUDA on ARM: the instruction set is not there and neither is
; the toolkit, so those rows do not exist and are not looked for.
#if HaveV('arm64', 'omp')
Source: "{#VariantDir('arm64','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('arm64|omp'); Components: solver
#endif
#if HaveV('arm64', 'plain')
Source: "{#VariantDir('arm64','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('arm64|plain'); Components: solver
#endif

; The UI half of the same choice, and the one that wins when the component is
; ticked: each of these folders holds the solver too. Same Check: as the solver
; row above, so the pair always matches - a plain solver never gets an AVX2 UI.
#if HaveU('x64', 'avx2-omp-cuda')
Source: "{#UiDir('x64','avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|avx2-omp-cuda'); Components: ui
#endif
#if HaveU('x64', 'avx2-omp')
Source: "{#UiDir('x64','avx2-omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|avx2-omp'); Components: ui
#endif
#if HaveU('x64', 'avx2-cuda')
Source: "{#UiDir('x64','avx2-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|avx2-cuda'); Components: ui
#endif
#if HaveU('x64', 'avx2')
Source: "{#UiDir('x64','avx2')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|avx2'); Components: ui
#endif
#if HaveU('x64', 'omp-cuda')
Source: "{#UiDir('x64','omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|omp-cuda'); Components: ui
#endif
#if HaveU('x64', 'omp')
Source: "{#UiDir('x64','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|omp'); Components: ui
#endif
#if HaveU('x64', 'cuda')
Source: "{#UiDir('x64','cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|cuda'); Components: ui
#endif
#if HaveU('x64', 'plain')
Source: "{#UiDir('x64','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x64|plain'); Components: ui
#endif

#if HaveU('x86', 'avx2-omp')
Source: "{#UiDir('x86','avx2-omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x86|avx2-omp'); Components: ui
#endif
#if HaveU('x86', 'avx2')
Source: "{#UiDir('x86','avx2')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x86|avx2'); Components: ui
#endif
#if HaveU('x86', 'omp')
Source: "{#UiDir('x86','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x86|omp'); Components: ui
#endif
#if HaveU('x86', 'plain')
Source: "{#UiDir('x86','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('x86|plain'); Components: ui
#endif

#if HaveU('arm64', 'omp')
Source: "{#UiDir('arm64','omp')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('arm64|omp'); Components: ui
#endif
#if HaveU('arm64', 'plain')
Source: "{#UiDir('arm64','plain')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('arm64|plain'); Components: ui
#endif

Source: "..\..\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist; Components: models
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE";   DestDir: "{app}"; Flags: ignoreversion

#if HaveUi
[InstallDelete]
; An install never removes anything on its own, so ticking the UI once and
; unticking it on the next run over the same folder used to leave
; "Fluid Solver UI.exe" behind - together with a Start Menu entry and a desktop
; icon aiming at a UI the user has just declined, built for a variant that may
; no longer be the one installed. Runs before the files are copied, so a run
; that DOES want the UI puts it straight back.
Type: files; Name: "{app}\Fluid Solver UI.exe";       Check: UiNotChosen
; The shortcut rows go by what this run wants pointed at rather than by whether
; the UI is installed at all, so unticking one of the two boxes above takes the
; matching leftovers from an earlier run with it.
Type: files; Name: "{group}\{#AppName} UI.lnk";       Check: NoUiIcon
Type: files; Name: "{autodesktop}\{#AppName} UI.lnk"; Check: NoUiIcon
Type: files; Name: "{group}\{#AppName}.lnk";          Check: NoConsoleIcon
Type: files; Name: "{autodesktop}\{#AppName}.lnk";    Check: NoConsoleIcon
#endif

[Dirs]
; The solver writes here. Created up front so a fresh install has it, and
; marked so an uninstall does not take the user's results with it.
Name: "{app}\output"; Flags: uninsneveruninstall

[Icons]
Name: "{group}\{#AppName}";       Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Fluid Solver.exe"; Tasks: startmenu;   Check: WantConsoleIcon
Name: "{group}\Output folder";    Filename: "{app}\output"; Tasks: startmenu
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Fluid Solver.exe"; Tasks: desktopicon; Check: WantConsoleIcon
#if HaveUi
Name: "{group}\{#AppName} UI";       Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Fluid Solver UI.exe"; Tasks: startmenu;   Components: ui; Check: WantUiIcon
Name: "{autodesktop}\{#AppName} UI"; Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; IconFilename: "{app}\Fluid Solver UI.exe"; Tasks: desktopicon; Components: ui; Check: WantUiIcon
#endif

[Registry]
Root: HKA; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NeedsPath('{app}'); Tasks: addtopath
#if HaveUi
Root: HKA; Subkey: "Software\Classes\.vtk"; ValueType: string; ValueName: ""; ValueData: "FluidSolver.vtk"; Flags: uninsdeletevalue; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk"; ValueType: string; ValueName: ""; ValueData: "VTK solution frame"; Flags: uninsdeletekey; Tasks: associatevtk
; The solver executable, not the UI: its icon is compiled in by src/app.rc.in
; and is therefore always there, while the UI's is written into the archive
; afterwards by scripts\stamp-ui-icon.py and can be missing if that step was
; skipped. The file still OPENS in the UI - that is the line below.
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Fluid Solver.exe,0"; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Fluid Solver UI.exe"" ""%1"""; Tasks: associatevtk
#endif

[Run]
#if HaveUi
Filename: "{app}\Fluid Solver UI.exe"; Description: "Run the Fluid Solver UI"; Flags: nowait postinstall skipifsilent; Components: ui
#endif
Filename: "{app}\Fluid Solver.exe"; Description: "Run Fluid Solver"; Flags: nowait postinstall skipifsilent
Filename: "{app}\README.md"; Description: "Open the README"; Flags: shellexec nowait postinstall skipifsilent unchecked

[Code]
var
  ChoicePage: TInputOptionWizardPage;   // automatic or manual
  FeaturePage: TInputOptionWizardPage;  // the three switches, when manual
  Available: TStringList;               // "arch|feature" entries in this file
  UiAvailable: TStringList;
  IdxAvx2, IdxOmp, IdxCuda: Integer;
  DetectedArch: String;
  AutoFeature: String;
  FeaturePageSeeded: Boolean;

const
  // PF_AVX2_INSTRUCTIONS_AVAILABLE
  PF_AVX2 = 40;

function IsProcessorFeaturePresent(Feature: DWORD): BOOL;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

// ---- what this machine is ------------------------------------------------

function HasAvx2: Boolean;
begin
  Result := IsProcessorFeaturePresent(PF_AVX2);
end;

// nvcuda.dll is installed in System32 by the NVIDIA display driver, so its
// presence means a CUDA-capable card with a working driver is in this machine.
// The solver falls back to the CPU on its own if that ever turns out wrong.
function HasNvidia: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\nvcuda.dll'));
end;

// OpenMP is worth having on anything with more than one core, and there is no
// driver or instruction set for it to be missing - the runtime ships beside
// the executable. One core is the only case where it costs more than it gives.
function CoreCount: Integer;
var
  Text: String;
begin
  Result := 1;
  Text := GetEnv('NUMBER_OF_PROCESSORS');
  if Text <> '' then
    Result := StrToIntDef(Text, 1);
  if Result < 1 then
    Result := 1;
end;

function HasManyCores: Boolean;
begin
  Result := CoreCount > 1;
end;

// ---- what the payload has ------------------------------------------------

function Key(const A, F: String): String;
begin
  Result := A + '|' + F;
end;

function HasBuild(const A, F: String): Boolean;
begin
  Result := Available.IndexOf(Key(A, F)) >= 0;
end;

function ArchPresent(const A: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to Available.Count - 1 do
    if Pos(A + '|', Available[I]) = 1 then
    begin
      Result := True;
      Exit;
    end;
end;

// Which architecture's builds this machine gets. An arm64 Windows runs x64
// through emulation, and an x64 Windows runs x86, so a payload missing the
// native rows still installs something that works rather than refusing.
function PickArch: String;
begin
  case ProcessorArchitecture of
    paArm64:
      begin
        if ArchPresent('arm64') then Result := 'arm64'
        else if ArchPresent('x64') then Result := 'x64'
        else Result := 'x86';
      end;
    paX64:
      begin
        if ArchPresent('x64') then Result := 'x64' else Result := 'x86';
      end;
  else
    Result := 'x86';
  end;
end;

// The three switches name one build: "avx2-omp-cuda" down to "plain".
function ComposeFeature(Avx2, Omp, Cuda: Boolean): String;
begin
  Result := '';
  if Avx2 then Result := Result + 'avx2-';
  if Omp  then Result := Result + 'omp-';
  if Cuda then Result := Result + 'cuda-';
  if Result = '' then
    Result := 'plain'
  else
    Result := Copy(Result, 1, Length(Result) - 1);
end;

// The best build this machine can actually use, out of the ones in the file.
// Every switch is dropped in turn rather than all at once, so a payload that
// is missing "avx2-omp" still lands on "avx2" or "omp" instead of falling all
// the way to "plain".
function BestFeatureFor(const A: String): String;
var
  WantAvx2, WantOmp, WantCuda: Boolean;
begin
  WantAvx2 := HasAvx2;
  WantOmp := HasManyCores;
  WantCuda := HasNvidia;

  Result := ComposeFeature(WantAvx2, WantOmp, WantCuda);
  if HasBuild(A, Result) then Exit;

  // CUDA first: without a driver it is the switch that buys nothing, and with
  // one the solver still runs on the CPU if the build is missing.
  if WantCuda then
  begin
    Result := ComposeFeature(WantAvx2, WantOmp, False);
    if HasBuild(A, Result) then Exit;
  end;
  if WantOmp then
  begin
    Result := ComposeFeature(WantAvx2, False, WantCuda);
    if HasBuild(A, Result) then Exit;
    Result := ComposeFeature(WantAvx2, False, False);
    if HasBuild(A, Result) then Exit;
  end;
  if WantAvx2 then
  begin
    Result := ComposeFeature(False, WantOmp, WantCuda);
    if HasBuild(A, Result) then Exit;
    Result := ComposeFeature(False, WantOmp, False);
    if HasBuild(A, Result) then Exit;
    Result := ComposeFeature(False, False, False);
    if HasBuild(A, Result) then Exit;
  end;

  // Nothing matched, which means the payload is odd rather than the machine.
  // Anything in it that this CPU can execute beats stopping here.
  if HasBuild(A, 'plain') then Result := 'plain'
  else if (not WantAvx2) and HasBuild(A, 'omp') then Result := 'omp'
  else if HasBuild(A, 'avx2-omp') then Result := 'avx2-omp'
  else Result := 'plain';
end;

function Checked(Index: Integer): Boolean;
begin
  Result := False;
  if FeaturePage = nil then Exit;
  if (Index >= 0) and (Index < FeaturePage.CheckListBox.Items.Count) then
    Result := FeaturePage.Values[Index];
end;

function AutomaticChosen: Boolean;
begin
  Result := True;
  if ChoicePage <> nil then
    Result := ChoicePage.SelectedValueIndex = 0;
end;

function SelectedFeature: String;
begin
  if AutomaticChosen or (FeaturePage = nil) then
    Result := AutoFeature
  else
    Result := ComposeFeature(Checked(IdxAvx2), Checked(IdxOmp), Checked(IdxCuda));
end;

function ActiveArch: String;
begin
  Result := DetectedArch;
end;

// One string rather than two, because a [Files] Check: is the only caller and
// "arch|feature" is exactly what Available already holds. Comparing the whole
// key at once also means there is no way for the two halves to be checked
// against different runs of the wizard.
function WantVariant(VariantKey: String): Boolean;
begin
  Result := VariantKey = ActiveArch + '|' + SelectedFeature;
end;

function UiChosen: Boolean;
begin
#if HaveUi
  Result := IsComponentSelected('ui');
#else
  Result := False;
#endif
end;

// A "<variant>-ui" archive is a complete install: it carries Fluid Solver.exe,
// Fluid Solver UI.exe, the DLLs and output\ together, because the UI is a shell
// that launches the solver and the two have to sit in one folder. So the UI
// archive REPLACES the solver archive rather than landing on top of it - laying
// one over the other would copy the same executable twice and leave the result
// depending on which [Files] line ran last.
function WantSolverOnly(VariantKey: String): Boolean;
begin
  Result := WantVariant(VariantKey) and (not UiChosen);
end;

// Spelled as a function of its own rather than "not UiChosen" written into the
// [InstallDelete] lines, so those read the same whichever Inno version compiles
// them.
function UiNotChosen: Boolean;
begin
  Result := not UiChosen;
end;

// What the shortcuts point at - the second question in [Tasks], asked in one
// place and answered for the desktop and the Start Menu alike. An install
// without the UI never sees those boxes, so it falls back to the only program
// it has.
function WantUiIcon: Boolean;
begin
  Result := UiChosen and WizardIsTaskSelected('iconui');
end;

function WantConsoleIcon: Boolean;
begin
  if not UiChosen then
    Result := True
  else
    Result := WizardIsTaskSelected('iconconsole');
end;

function NoUiIcon: Boolean;
begin
  Result := not WantUiIcon;
end;

function NoConsoleIcon: Boolean;
begin
  Result := not WantConsoleIcon;
end;

function HasUiFor(const A, F: String): Boolean;
begin
  Result := UiAvailable.IndexOf(Key(A, F)) >= 0;
end;

function DescribeMachine: String;
begin
  Result := 'This machine: ';
  case ProcessorArchitecture of
    paArm64: Result := Result + 'ARM64';
    paX64:   Result := Result + '64-bit x86';
  else       Result := Result + '32-bit x86';
  end;
  Result := Result + ', ' + IntToStr(CoreCount) + ' core(s), ';
  if HasAvx2 then Result := Result + 'AVX2' else Result := Result + 'no AVX2';
  if HasNvidia then Result := Result + ', NVIDIA driver present'
  else Result := Result + ', no NVIDIA driver';
  Result := Result + '.';
end;

procedure InitializeWizard;
var
  Summary: String;
begin
  Available := TStringList.Create;
#if HaveV('x64', 'avx2-omp-cuda')
  Available.Add('x64|avx2-omp-cuda');
#endif
#if HaveV('x64', 'avx2-omp')
  Available.Add('x64|avx2-omp');
#endif
#if HaveV('x64', 'avx2-cuda')
  Available.Add('x64|avx2-cuda');
#endif
#if HaveV('x64', 'avx2')
  Available.Add('x64|avx2');
#endif
#if HaveV('x64', 'omp-cuda')
  Available.Add('x64|omp-cuda');
#endif
#if HaveV('x64', 'omp')
  Available.Add('x64|omp');
#endif
#if HaveV('x64', 'cuda')
  Available.Add('x64|cuda');
#endif
#if HaveV('x64', 'plain')
  Available.Add('x64|plain');
#endif
#if HaveV('x86', 'avx2-omp')
  Available.Add('x86|avx2-omp');
#endif
#if HaveV('x86', 'avx2')
  Available.Add('x86|avx2');
#endif
#if HaveV('x86', 'omp')
  Available.Add('x86|omp');
#endif
#if HaveV('x86', 'plain')
  Available.Add('x86|plain');
#endif
#if HaveV('arm64', 'omp')
  Available.Add('arm64|omp');
#endif
#if HaveV('arm64', 'plain')
  Available.Add('arm64|plain');
#endif

  // The same list for the UI, which is built per variant too. It is a subset
  // of Available - possibly an empty one - and NextButtonClick uses it to catch
  // "UI ticked, but not for the build you chose" before anything is copied.
  UiAvailable := TStringList.Create;
#if HaveU('x64', 'avx2-omp-cuda')
  UiAvailable.Add('x64|avx2-omp-cuda');
#endif
#if HaveU('x64', 'avx2-omp')
  UiAvailable.Add('x64|avx2-omp');
#endif
#if HaveU('x64', 'avx2-cuda')
  UiAvailable.Add('x64|avx2-cuda');
#endif
#if HaveU('x64', 'avx2')
  UiAvailable.Add('x64|avx2');
#endif
#if HaveU('x64', 'omp-cuda')
  UiAvailable.Add('x64|omp-cuda');
#endif
#if HaveU('x64', 'omp')
  UiAvailable.Add('x64|omp');
#endif
#if HaveU('x64', 'cuda')
  UiAvailable.Add('x64|cuda');
#endif
#if HaveU('x64', 'plain')
  UiAvailable.Add('x64|plain');
#endif
#if HaveU('x86', 'avx2-omp')
  UiAvailable.Add('x86|avx2-omp');
#endif
#if HaveU('x86', 'avx2')
  UiAvailable.Add('x86|avx2');
#endif
#if HaveU('x86', 'omp')
  UiAvailable.Add('x86|omp');
#endif
#if HaveU('x86', 'plain')
  UiAvailable.Add('x86|plain');
#endif
#if HaveU('arm64', 'omp')
  UiAvailable.Add('arm64|omp');
#endif
#if HaveU('arm64', 'plain')
  UiAvailable.Add('arm64|plain');
#endif

  DetectedArch := PickArch;
  AutoFeature := BestFeatureFor(DetectedArch);

  Summary := DescribeMachine + #13#10 +
    'Picked for it: the ' + DetectedArch + ' "' + AutoFeature + '" build.' + #13#10#13#10 +
    'These change how fast a run is, not what it solves. Leave the first' + #13#10 +
    'option alone unless you have a reason not to.';

  ChoicePage := CreateInputOptionPage(wpSelectComponents,
    'Solver build', 'Which build should be installed?',
    Summary, True, False);
  ChoicePage.Add('Choose automatically for this machine (recommended)');
  ChoicePage.Add('Let me pick AVX2, OpenMP and CUDA myself');
  ChoicePage.SelectedValueIndex := 0;

  IdxAvx2 := -1;
  IdxOmp := -1;
  IdxCuda := -1;

  FeaturePage := CreateInputOptionPage(ChoicePage.ID,
    'Solver build', 'Which parts should the installed solver use?',
    'Only the switches this download has builds on both sides of are shown.' + #13#10 +
    'The boxes start where the automatic choice left them.',
    False, False);
  IdxAvx2 := FeaturePage.Add('AVX2 - vector kernels. Needs an Intel or AMD CPU from about 2013 on.');
  IdxOmp := FeaturePage.Add('OpenMP - use every core of the CPU instead of one.');
  IdxCuda := FeaturePage.Add('CUDA - run the pressure solve on an NVIDIA GPU.');
end;

// The manual page is filled in when it is reached rather than in
// InitializeWizard, because which switches are worth showing depends on the
// architecture that was picked, and a combined installer does not know that
// until it is running.
procedure CurPageChanged(CurPageID: Integer);
var
  A: String;
  HasOn, HasOff: Boolean;
  I: Integer;
  Feature: String;
begin
  if (FeaturePage = nil) or (CurPageID <> FeaturePage.ID) then Exit;
  A := ActiveArch;

  // Seeded from the automatic choice the first time the page is opened, and
  // left alone after that: going Back and Next again must not throw away what
  // the user just ticked.
  if not FeaturePageSeeded then
  begin
    FeaturePageSeeded := True;
    FeaturePage.Values[IdxAvx2] := Pos('avx2', AutoFeature) > 0;
    FeaturePage.Values[IdxOmp] := Pos('omp', AutoFeature) > 0;
    FeaturePage.Values[IdxCuda] := Pos('cuda', AutoFeature) > 0;
  end;

  // A switch is only worth a box when the payload has builds both with and
  // without it for this architecture. That is what keeps CUDA off the 32-bit
  // and ARM pages without a second copy of this file.

  HasOn := False; HasOff := False;
  for I := 0 to Available.Count - 1 do
    if Pos(A + '|', Available[I]) = 1 then
    begin
      Feature := Copy(Available[I], Length(A) + 2, Length(Available[I]));
      if Pos('avx2', Feature) > 0 then HasOn := True else HasOff := True;
    end;
  FeaturePage.CheckListBox.ItemEnabled[IdxAvx2] := HasOn and HasOff;

  HasOn := False; HasOff := False;
  for I := 0 to Available.Count - 1 do
    if Pos(A + '|', Available[I]) = 1 then
    begin
      Feature := Copy(Available[I], Length(A) + 2, Length(Available[I]));
      if Pos('omp', Feature) > 0 then HasOn := True else HasOff := True;
    end;
  FeaturePage.CheckListBox.ItemEnabled[IdxOmp] := HasOn and HasOff;

  HasOn := False; HasOff := False;
  for I := 0 to Available.Count - 1 do
    if Pos(A + '|', Available[I]) = 1 then
    begin
      Feature := Copy(Available[I], Length(A) + 2, Length(Available[I]));
      if Pos('cuda', Feature) > 0 then HasOn := True else HasOff := True;
    end;
  FeaturePage.CheckListBox.ItemEnabled[IdxCuda] := HasOn and HasOff;
end;

function NeedsPath(Dir: String): Boolean;
var
  Existing: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Existing) then
    Existing := '';
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Existing) + ';') = 0;
end;

// The manual page is skipped unless the user asked for it on the page before.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (FeaturePage <> nil) and (PageID = FeaturePage.ID) then
    Result := AutomaticChosen;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Feature, List, A: String;
  I: Integer;
begin
  Result := True;
  if FeaturePage = nil then Exit;
  if CurPageID <> FeaturePage.ID then Exit;

  A := ActiveArch;
  if Checked(IdxAvx2) and (not HasAvx2) then
  begin
    MsgBox('This CPU does not support AVX2. That build would stop with an ' +
           '"illegal instruction" error on the first run.' + #13#10#13#10 +
           'Untick AVX2.', mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // Every combination of the three is built, but a build machine that was
  // missing a toolkit produces fewer, and the installer only carries what it
  // was given.
  Feature := SelectedFeature;
  if not HasBuild(A, Feature) then
  begin
    List := '';
    for I := 0 to Available.Count - 1 do
      if Pos(A + '|', Available[I]) = 1 then
        List := List + #13#10 + '    ' +
                Copy(Available[I], Length(A) + 2, Length(Available[I]));
    // The '+' leads the line on purpose: a line whose first non-blank
    // character is '#' is a preprocessor directive to ISPP, and '#13' is not
    // one of them, so wrapping before the constant instead of after it is what
    // makes this compile at all.
    MsgBox('This installer does not carry the ' + A + ' "' + Feature + '" build.'
           + #13#10#13#10 + 'It has:' + List, mbError, MB_OK);
    Result := False;
    Exit;
  end;

#if HaveUi
  // The UI ships per variant as well, so ticking the component is not enough:
  // there has to be a UI for the build that was just chosen. Saying so here
  // beats installing a solver with nothing beside it and no explanation.
  if UiChosen and (not HasUiFor(A, Feature)) then
  begin
    List := '';
    for I := 0 to UiAvailable.Count - 1 do
      if Pos(A + '|', UiAvailable[I]) = 1 then
        List := List + #13#10 + '    ' +
                Copy(UiAvailable[I], Length(A) + 2, Length(UiAvailable[I]));
    if List = '' then
      List := #13#10 + '    (none)';
    MsgBox('There is no desktop UI built for the ' + A + ' "' + Feature + '" solver.'
           + #13#10#13#10 + 'The UI is built per variant, and this installer has one for:'
           + List + #13#10#13#10
           + 'Either pick one of those, or go back and untick the UI component.',
           mbError, MB_OK);
    Result := False;
    Exit;
  end;
#endif

  // A CUDA build without a driver still starts - it says so and runs on the
  // CPU - but the user ticked the box for a reason, so it is worth saying now.
  if Checked(IdxCuda) and (not HasNvidia) then
    if MsgBox('No NVIDIA driver was found on this machine, so the CUDA build ' +
              'will fall back to the CPU every time it runs.' + #13#10#13#10 +
              'Install it anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
end;

// The automatic path never shows the page above, so its AVX2 guard never fires
// there. BestFeatureFor already refuses to pick an AVX2 build on a CPU without
// AVX2, and this is the belt to that pair of braces: a payload so odd that
// nothing else was left would otherwise install a binary that dies on its
// first instruction.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if (Pos('avx2', SelectedFeature) > 0) and (not HasAvx2) then
    Result := 'The only build this installer has for your machine uses AVX2, ' +
              'and this CPU does not support it. The solver would stop with an ' +
              '"illegal instruction" error on the first run.';
end;

// What used to be a "Pin to the taskbar" tick box. Windows 11 removed the
// shell verb an installer could drive, so the box could only ever fail on a
// current machine; saying how to do it by hand is the honest version of the
// same feature. Shown once, at the end, and only when a shortcut was actually
// created - with nothing in the Start Menu there is nothing to right-click.
procedure CurStepChanged(CurStep: TSetupStep);
var
  What: String;
begin
  if CurStep <> ssPostInstall then Exit;
  if not WizardIsTaskSelected('startmenu') then Exit;

  if WantUiIcon then
    What := 'Fluid Solver UI'
  else
    What := 'Fluid Solver';

  MsgBox('One thing Windows will not let an installer do: pin to the taskbar.' + #13#10#13#10 +
         'Open the Start Menu, find "' + What + '", right-click it and choose ' +
         '"Pin to taskbar". Since Windows 11 that is the only way, for every ' +
         'program - it is not this one being awkward.',
         mbInformation, MB_OK);
end;
