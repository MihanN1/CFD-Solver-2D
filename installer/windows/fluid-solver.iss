; Inno Setup script for Fluid Solver.
;
; One installer per architecture. It carries every feature variant built for
; that architecture and the user picks one at install time; the chosen build is
; copied in as "Fluid Solver.exe", so shortcuts, the UI and any script the user
; writes never have to know which variant is installed.
;
; Build it with:
;   iscc /DAppVersion=0.1 /DArch=x64 /DDistDir=..\..\dist installer\windows\fluid-solver.iss
;
; DistDir must contain the folders build-windows.ps1 produced:
;   Fluid Solver <ver> windows-<arch> <feature>\Fluid Solver.exe

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
Name: "full";   Description: "Solver, desktop UI and example models"
Name: "solver"; Description: "Solver only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "solver"; Description: "Fluid Solver (required)"; Types: full solver custom; Flags: fixed
Name: "ui";     Description: "Desktop UI";              Types: full
Name: "models"; Description: "Example models";          Types: full

[Tasks]
Name: "desktopicon";  Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "startmenu";    Description: "Create a Start Menu shortcut"; GroupDescription: "{cm:AdditionalIcons}"
Name: "addtopath";    Description: "Add Fluid Solver to PATH (lets you run it from any terminal)"; Flags: unchecked
Name: "associatevtk"; Description: "Open .vtk files with the Fluid Solver UI"; Components: ui; Flags: unchecked

[Files]
; Exactly one of these is installed, chosen on the custom page below.
Source: "{#Variant('avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp-cuda'); Components: solver
Source: "{#Variant('avx2-omp')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp');      Components: solver
Source: "{#Variant('avx2-cuda')}\*";     DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-cuda');     Components: solver
Source: "{#Variant('avx2')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2');          Components: solver
Source: "{#Variant('omp-cuda')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp-cuda');      Components: solver
Source: "{#Variant('omp')}\*";           DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp');           Components: solver
Source: "{#Variant('cuda')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('cuda');          Components: solver
Source: "{#Variant('plain')}\*";         DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('plain');         Components: solver

Source: "{#DistDir}\ui-windows-{#Arch}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist; Components: ui
Source: "..\..\models\*";                  DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist; Components: models
Source: "..\..\README.md";                 DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE";                   DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; The solver writes here. Created up front so a fresh install has it, and
; marked so an uninstall does not take the user's results with it.
Name: "{app}\output"; Flags: uninsneveruninstall

[Icons]
Name: "{group}\{#AppName}";         Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: startmenu
Name: "{group}\{#AppName} UI";      Filename: "{app}\cfd_mask_ui_optimized.exe"; WorkingDir: "{app}"; Tasks: startmenu; Components: ui
Name: "{group}\Output folder";      Filename: "{app}\output"; Tasks: startmenu
Name: "{autodesktop}\{#AppName}";   Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\{#AppName} UI"; Filename: "{app}\cfd_mask_ui_optimized.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Components: ui

[Registry]
Root: HKA; Subkey: "Software\Classes\.vtk"; ValueType: string; ValueName: ""; ValueData: "FluidSolver.vtk"; Flags: uninsdeletevalue; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk"; ValueType: string; ValueName: ""; ValueData: "VTK solution frame"; Flags: uninsdeletekey; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\Fluid Solver.exe,0"; Tasks: associatevtk
Root: HKA; Subkey: "Software\Classes\FluidSolver.vtk\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\cfd_mask_ui_optimized.exe"" ""%1"""; Tasks: associatevtk
Root: HKA; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NeedsPath('{app}'); Tasks: addtopath

[Run]
Filename: "{app}\Fluid Solver.exe"; Description: "Run Fluid Solver"; Flags: nowait postinstall skipifsilent
Filename: "{app}\README.md"; Description: "Open the README"; Flags: shellexec nowait postinstall skipifsilent unchecked

[Code]
var
  VariantPage: TInputOptionWizardPage;
  DetectedIndex: Integer;

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

function VariantName(Index: Integer): String;
begin
  case Index of
    0: Result := 'avx2-omp-cuda';
    1: Result := 'avx2-omp';
    2: Result := 'avx2-cuda';
    3: Result := 'avx2';
    4: Result := 'omp-cuda';
    5: Result := 'omp';
    6: Result := 'cuda';
    7: Result := 'plain';
  else
    Result := 'plain';
  end;
end;

function Recommended: Integer;
begin
  if HasAvx2 and HasNvidia then Result := 0
  else if HasAvx2            then Result := 1
  else if HasNvidia          then Result := 4
  else                            Result := 5;
end;

procedure InitializeWizard;
var
  Summary: String;
  I: Integer;
begin
  DetectedIndex := Recommended;

  Summary := 'This machine: ';
  if HasAvx2 then Summary := Summary + 'AVX2 supported' else Summary := Summary + 'no AVX2';
  if HasNvidia then Summary := Summary + ', NVIDIA driver present' else Summary := Summary + ', no NVIDIA driver';
  Summary := Summary + '.' + #13#10 +
    'Recommended: ' + VariantName(DetectedIndex) + '. Pick another one only if you ' +
    'want to compare them - every build produces the same numbers, they differ ' +
    'in speed.';

  VariantPage := CreateInputOptionPage(wpSelectComponents,
    'Solver build', 'Which build of the solver should be installed?',
    Summary, True, False);

  VariantPage.Add('AVX2 + OpenMP + CUDA - fastest, needs an NVIDIA GPU');
  VariantPage.Add('AVX2 + OpenMP - fastest without a GPU');
  VariantPage.Add('AVX2 + CUDA - single-threaded CPU, GPU pressure solve');
  VariantPage.Add('AVX2 - vector kernels, single thread');
  VariantPage.Add('OpenMP + CUDA - no vector kernels');
  VariantPage.Add('OpenMP - threads only');
  VariantPage.Add('CUDA - GPU pressure solve only');
  VariantPage.Add('Plain - maximum compatibility, runs on anything');

  VariantPage.SelectedValueIndex := DetectedIndex;
end;

function WantVariant(Name: String): Boolean;
begin
  Result := VariantName(VariantPage.SelectedValueIndex) = Name;
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
begin
  Result := True;
  if CurPageID <> VariantPage.ID then
    Exit;

  // A CUDA build without a driver still starts - it says so and runs on the
  // CPU - but the user chose it for a reason, so it is worth saying now.
  if (Pos('cuda', VariantName(VariantPage.SelectedValueIndex)) > 0) and (not HasNvidia) then
    if MsgBox('No NVIDIA driver was found on this machine, so the CUDA build ' +
              'will fall back to the CPU every time it runs.' + #13#10#13#10 +
              'Install it anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;

  if (Pos('avx2', VariantName(VariantPage.SelectedValueIndex)) > 0) and (not HasAvx2) then
  begin
    MsgBox('This CPU does not support AVX2. That build would stop with an ' +
           '"illegal instruction" error on the first run.' + #13#10#13#10 +
           'Pick a build without AVX2.', mbError, MB_OK);
    Result := False;
  end;
end;
