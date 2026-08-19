; Inno Setup script for Fluid Solver.
;
; One installer per architecture. It carries every feature variant built for
; that architecture; the user ticks AVX2, OpenMP and CUDA independently and the
; matching build is installed as "Fluid Solver.exe", so shortcuts, the UI and
; any script the user writes never have to know which one it is.
;
; Build it with:
;   iscc /DAppVersion=0.1 /DArch=x64 /DDistDir=..\..\dist installer\windows\fluid-solver.iss
;
; DistDir must contain the folders make-release.ps1 produced:
;   Fluid Solver <ver> windows-<arch> <feature>\Fluid Solver.exe
;   ui-windows-<arch>\...                          (optional, the desktop UI)
;
; Only the variants actually present in DistDir are compiled in, and a switch
; with no builds behind it is not offered at all - which is how the 32-bit
; installer ends up without a CUDA box without anything being written twice.
; The UI is the same: it appears as a component only when its folder is there.

#ifndef AppVersion
  #define AppVersion "0.1"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef DistDir
  #define DistDir "..\..\dist"
#endif
#define AppName "Fluid Solver"
#define Publisher "MihanN1"
#define AppUrl "https://github.com/MihanN1/CFD-Solver-2D"
#define Variant(Feature) DistDir + "\" + AppName + " " + AppVersion + " windows-" + Arch + " " + Feature
#define HaveVariant(Feature) DirExists(Variant(Feature))
#define UiDir DistDir + "\ui-windows-" + Arch
#define HaveUi DirExists(UiDir)

#define HaveAny (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('avx2-cuda') || HaveVariant('avx2') || HaveVariant('omp-cuda') || HaveVariant('omp') || HaveVariant('cuda') || HaveVariant('plain'))
#define HaveAnyAvx2 (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('avx2-cuda') || HaveVariant('avx2'))
#define HaveAnyOmp (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('omp-cuda') || HaveVariant('omp'))
#define HaveAnyCuda (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-cuda') || HaveVariant('omp-cuda') || HaveVariant('cuda'))

#if !HaveAny
  #error "No 'Fluid Solver <ver> windows-<arch> <feature>' folder found in DistDir. Run scripts\make-release.ps1 first."
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
OutputBaseFilename={#AppName} {#AppVersion} windows-{#Arch} setup
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\logo\fluid-solver.ico
WizardImageFile=..\..\logo\wizard-164x314.bmp,..\..\logo\wizard-410x797.bmp
WizardSmallImageFile=..\..\logo\wizard-small-55x55.bmp,..\..\logo\wizard-small-138x140.bmp
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
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
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
Name: "addtopath";    Description: "Add Fluid Solver to PATH (lets you run it from any terminal)"; Flags: unchecked
#if HaveUi
Name: "associatevtk"; Description: "Open .vtk files with the Fluid Solver UI"; Components: ui; Flags: unchecked
#endif

[Files]
; Exactly one of these is installed, chosen by the tick boxes below. Each line
; only exists if that build is in DistDir.
#if HaveVariant('avx2-omp-cuda')
Source: "{#Variant('avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp-cuda'); Components: solver
#endif
#if HaveVariant('avx2-omp')
Source: "{#Variant('avx2-omp')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp');      Components: solver
#endif
#if HaveVariant('avx2-cuda')
Source: "{#Variant('avx2-cuda')}\*";     DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-cuda');     Components: solver
#endif
#if HaveVariant('avx2')
Source: "{#Variant('avx2')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2');          Components: solver
#endif
#if HaveVariant('omp-cuda')
Source: "{#Variant('omp-cuda')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp-cuda');      Components: solver
#endif
#if HaveVariant('omp')
Source: "{#Variant('omp')}\*";           DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp');           Components: solver
#endif
#if HaveVariant('cuda')
Source: "{#Variant('cuda')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('cuda');          Components: solver
#endif
#if HaveVariant('plain')
Source: "{#Variant('plain')}\*";         DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('plain');         Components: solver
#endif

#if HaveUi
Source: "{#UiDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Components: ui
#endif
Source: "..\..\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist; Components: models
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE";   DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; The solver writes here. Created up front so a fresh install has it, and
; marked so an uninstall does not take the user's results with it.
Name: "{app}\output"; Flags: uninsneveruninstall

[Icons]
Name: "{group}\{#AppName}";       Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: startmenu
Name: "{group}\Output folder";    Filename: "{app}\output"; Tasks: startmenu
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: desktopicon
#if HaveUi
Name: "{group}\{#AppName} UI";       Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; Tasks: startmenu; Components: ui
Name: "{autodesktop}\{#AppName} UI"; Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Components: ui
#endif

[Registry]
Root: HKA; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NeedsPath('{app}'); Tasks: addtopath
#if HaveUi
Root: HKA; Subkey: "Software\Classes\.vtk"; ValueType: string; ValueName: ""; ValueData: "FluidSolver.vtk"; Flags: uninsdeletevalue; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk"; ValueType: string; ValueName: ""; ValueData: "VTK solution frame"; Flags: uninsdeletekey; Tasks: associatevtk
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
  FeaturePage: TInputOptionWizardPage;
  Available: TStringList;
  IdxAvx2, IdxOmp, IdxCuda: Integer;

const
  // PF_AVX2_INSTRUCTIONS_AVAILABLE
  PF_AVX2 = 40;

function IsProcessorFeaturePresent(Feature: DWORD): BOOL;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

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

function Checked(Index: Integer): Boolean;
begin
  Result := False;
  if FeaturePage = nil then Exit;
  if (Index >= 0) and (Index < FeaturePage.CheckListBox.Items.Count) then
    Result := FeaturePage.Values[Index];
end;

// The three boxes name one build: "avx2-omp-cuda" down to "plain".
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

function SelectedFeature: String;
begin
  Result := ComposeFeature(Checked(IdxAvx2), Checked(IdxOmp), Checked(IdxCuda));
end;

function WantVariant(Name: String): Boolean;
begin
  Result := SelectedFeature = Name;
end;

procedure InitializeWizard;
var
  Summary: String;
begin
  Available := TStringList.Create;
#if HaveVariant('avx2-omp-cuda')
  Available.Add('avx2-omp-cuda');
#endif
#if HaveVariant('avx2-omp')
  Available.Add('avx2-omp');
#endif
#if HaveVariant('avx2-cuda')
  Available.Add('avx2-cuda');
#endif
#if HaveVariant('avx2')
  Available.Add('avx2');
#endif
#if HaveVariant('omp-cuda')
  Available.Add('omp-cuda');
#endif
#if HaveVariant('omp')
  Available.Add('omp');
#endif
#if HaveVariant('cuda')
  Available.Add('cuda');
#endif
#if HaveVariant('plain')
  Available.Add('plain');
#endif

  Summary := 'This machine: ';
  if HasAvx2 then Summary := Summary + 'AVX2 supported' else Summary := Summary + 'no AVX2';
  if HasNvidia then Summary := Summary + ', NVIDIA driver present' else Summary := Summary + ', no NVIDIA driver';
  Summary := Summary + '.' + #13#10 +
    'Every build produces the same numbers - they differ only in speed. The boxes' + #13#10 +
    'are already ticked for what this machine can use.';

  FeaturePage := CreateInputOptionPage(wpSelectComponents,
    'Solver build', 'Which parts should the installed solver use?',
    Summary, False, False);

  IdxAvx2 := -1;
  IdxOmp := -1;
  IdxCuda := -1;

  // A switch with no builds behind it is not shown at all, which is what keeps
  // CUDA off the 32-bit installer without a second copy of this file.
#if HaveAnyAvx2
  IdxAvx2 := FeaturePage.Add('AVX2 - vector kernels. Needs an Intel or AMD CPU from about 2013 on.');
#endif
#if HaveAnyOmp
  IdxOmp := FeaturePage.Add('OpenMP - use every core of the CPU instead of one.');
#endif
#if HaveAnyCuda
  IdxCuda := FeaturePage.Add('CUDA - run the pressure solve on an NVIDIA GPU.');
#endif

  if IdxAvx2 >= 0 then FeaturePage.Values[IdxAvx2] := HasAvx2;
  if IdxOmp  >= 0 then FeaturePage.Values[IdxOmp]  := True;
  if IdxCuda >= 0 then FeaturePage.Values[IdxCuda] := HasNvidia;
end;

function NeedsPath(Dir: String): Boolean;
var
  Existing: String;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Existing) then
    Existing := '';
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Existing) + ';') = 0;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Feature, List: String;
  I: Integer;
begin
  Result := True;
  if FeaturePage = nil then Exit;
  if CurPageID <> FeaturePage.ID then Exit;

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
  if Available.IndexOf(Feature) < 0 then
  begin
    List := '';
    for I := 0 to Available.Count - 1 do
      List := List + #13#10 + '    ' + Available[I];
    MsgBox('This installer does not carry the "' + Feature + '" build.' +
           #13#10#13#10 + 'It has:' + List, mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // A CUDA build without a driver still starts - it says so and runs on the
  // CPU - but the user ticked the box for a reason, so it is worth saying now.
  if Checked(IdxCuda) and (not HasNvidia) then
    if MsgBox('No NVIDIA driver was found on this machine, so the CUDA build ' +
              'will fall back to the CPU every time it runs.' + #13#10#13#10 +
              'Install it anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
end;
