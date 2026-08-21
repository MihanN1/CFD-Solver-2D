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
;   Fluid Solver <ver> windows-<arch> <feature>-ui\...   (optional, the UI)
;
; The "-ui" folder is a complete install of its own - solver, UI, DLLs and
; output\ in one place, because the UI is a shell that starts the solver. So
; ticking the UI installs that folder INSTEAD of the plain one, never both.
;
; Only the variants actually present in DistDir are compiled in, and a switch
; is offered only when the payload has builds on BOTH sides of it - which is
; how the 32-bit installer ends up without a CUDA box without anything being
; written twice. Checking one side was not enough: a dist that only produced
; AVX2 rows still showed the AVX2 box, and unticking it named a build that was
; not in the file. An axis with builds on one side only is now pinned to that
; side instead of asked about.
; The UI is the same: the component is offered only when at least one "-ui"
; folder is there, and a UI is only ever paired with the solver of its own
; variant.

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
; The UI is built per variant exactly like the solver is, so it lives beside it
; under "<variant>-ui" and is picked by the same tick boxes. HaveUi is the
; question "is there a UI for anything at all", which is what decides whether
; the component is offered; whether there is one for the variant the user
; actually chose is a separate check, in NextButtonClick.
#define UiVariant(Feature) DistDir + "\" + AppName + " " + AppVersion + " windows-" + Arch + " " + Feature + "-ui"
#define HaveUiVariant(Feature) DirExists(UiVariant(Feature))
#define HaveUi (HaveUiVariant('avx2-omp-cuda') || HaveUiVariant('avx2-omp') || HaveUiVariant('avx2-cuda') || HaveUiVariant('avx2') || HaveUiVariant('omp-cuda') || HaveUiVariant('omp') || HaveUiVariant('cuda') || HaveUiVariant('plain'))

#define HaveAny (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('avx2-cuda') || HaveVariant('avx2') || HaveVariant('omp-cuda') || HaveVariant('omp') || HaveVariant('cuda') || HaveVariant('plain'))
#define HaveAnyAvx2 (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('avx2-cuda') || HaveVariant('avx2'))
#define HaveAnyOmp (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-omp') || HaveVariant('omp-cuda') || HaveVariant('omp'))
#define HaveAnyCuda (HaveVariant('avx2-omp-cuda') || HaveVariant('avx2-cuda') || HaveVariant('omp-cuda') || HaveVariant('cuda'))

; The other side of each axis. A box is only worth showing when both of these
; are true for it; otherwise there is nothing to choose between.
#define HaveNoAvx2 (HaveVariant('omp-cuda') || HaveVariant('omp') || HaveVariant('cuda') || HaveVariant('plain'))
#define HaveNoOmp (HaveVariant('avx2-cuda') || HaveVariant('avx2') || HaveVariant('cuda') || HaveVariant('plain'))
#define HaveNoCuda (HaveVariant('avx2-omp') || HaveVariant('avx2') || HaveVariant('omp') || HaveVariant('plain'))

#define ShowAvx2 (HaveAnyAvx2 && HaveNoAvx2)
#define ShowOmp (HaveAnyOmp && HaveNoOmp)
#define ShowCuda (HaveAnyCuda && HaveNoCuda)

; What a hidden axis is worth: the only value the payload has for it. Spelled
; with #if rather than a ternary so this file needs nothing from the
; preprocessor that the rest of it does not already use.
#if HaveAnyAvx2
  #define PinAvx2Value "True"
#else
  #define PinAvx2Value "False"
#endif
#if HaveAnyOmp
  #define PinOmpValue "True"
#else
  #define PinOmpValue "False"
#endif
#if HaveAnyCuda
  #define PinCudaValue "True"
#else
  #define PinCudaValue "False"
#endif

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
; The third shortcut. Unlike the other two this one cannot be declared in
; [Icons], because Windows has no installer-level way to pin anything - see
; PinToTaskbar in [Code].
Name: "taskbar";      Description: "Pin to the taskbar"; GroupDescription: "{cm:AdditionalIcons}"
#if HaveUi
; Where a shortcut goes and what it starts are two different questions, so they
; are two groups. This second one only appears when the UI is actually being
; installed - without it there is a single program to point at and nothing to
; choose between. Both boxes apply to all three places above at once.
Name: "iconui";      Description: "Fluid Solver UI - the window"; GroupDescription: "Which program the shortcuts start:"; Components: ui
Name: "iconconsole"; Description: "Fluid Solver - the console version, where every parameter is typed at the prompt"; GroupDescription: "Which program the shortcuts start:"; Components: ui; Flags: unchecked
#endif
Name: "addtopath";    Description: "Add Fluid Solver to PATH (lets you run it from any terminal)"; Flags: unchecked
#if HaveUi
Name: "associatevtk"; Description: "Open .vtk files with the Fluid Solver UI"; Components: ui; Flags: unchecked
#endif

[Files]
; Exactly one of these is installed, chosen by the tick boxes below. Each line
; only exists if that build is in DistDir. WantSolverOnly, not WantVariant:
; when the UI component is ticked the "-ui" row further down installs instead,
; because it already contains this same executable.
#if HaveVariant('avx2-omp-cuda')
Source: "{#Variant('avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('avx2-omp-cuda'); Components: solver
#endif
#if HaveVariant('avx2-omp')
Source: "{#Variant('avx2-omp')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('avx2-omp');      Components: solver
#endif
#if HaveVariant('avx2-cuda')
Source: "{#Variant('avx2-cuda')}\*";     DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('avx2-cuda');     Components: solver
#endif
#if HaveVariant('avx2')
Source: "{#Variant('avx2')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('avx2');          Components: solver
#endif
#if HaveVariant('omp-cuda')
Source: "{#Variant('omp-cuda')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('omp-cuda');      Components: solver
#endif
#if HaveVariant('omp')
Source: "{#Variant('omp')}\*";           DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('omp');           Components: solver
#endif
#if HaveVariant('cuda')
Source: "{#Variant('cuda')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('cuda');          Components: solver
#endif
#if HaveVariant('plain')
Source: "{#Variant('plain')}\*";         DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantSolverOnly('plain');         Components: solver
#endif

; The UI half of the same choice, and the one that wins when the component is
; ticked: each of these folders holds the solver too. Same Check: as the solver
; row above, so the pair always matches - a plain solver never gets an AVX2 UI.
#if HaveUiVariant('avx2-omp-cuda')
Source: "{#UiVariant('avx2-omp-cuda')}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp-cuda'); Components: ui
#endif
#if HaveUiVariant('avx2-omp')
Source: "{#UiVariant('avx2-omp')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-omp');      Components: ui
#endif
#if HaveUiVariant('avx2-cuda')
Source: "{#UiVariant('avx2-cuda')}\*";     DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2-cuda');     Components: ui
#endif
#if HaveUiVariant('avx2')
Source: "{#UiVariant('avx2')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('avx2');          Components: ui
#endif
#if HaveUiVariant('omp-cuda')
Source: "{#UiVariant('omp-cuda')}\*";      DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp-cuda');      Components: ui
#endif
#if HaveUiVariant('omp')
Source: "{#UiVariant('omp')}\*";           DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('omp');           Components: ui
#endif
#if HaveUiVariant('cuda')
Source: "{#UiVariant('cuda')}\*";          DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('cuda');          Components: ui
#endif
#if HaveUiVariant('plain')
Source: "{#UiVariant('plain')}\*";         DestDir: "{app}"; Flags: ignoreversion recursesubdirs; Check: WantVariant('plain');         Components: ui
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
Name: "{group}\{#AppName}";       Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: startmenu;   Check: WantConsoleIcon
Name: "{group}\Output folder";    Filename: "{app}\output"; Tasks: startmenu
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Fluid Solver.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Check: WantConsoleIcon
#if HaveUi
Name: "{group}\{#AppName} UI";       Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; Tasks: startmenu;   Components: ui; Check: WantUiIcon
Name: "{autodesktop}\{#AppName} UI"; Filename: "{app}\Fluid Solver UI.exe"; WorkingDir: "{app}"; Tasks: desktopicon; Components: ui; Check: WantUiIcon
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
  UiAvailable: TStringList;
  IdxAvx2, IdxOmp, IdxCuda: Integer;
  // What an axis that is not shown is worth, filled in from the payload
  PinAvx2, PinOmp, PinCuda: Boolean;

const
  // PF_AVX2_INSTRUCTIONS_AVAILABLE
  PF_AVX2 = 40;
  // shell32.dll string resources: "Pin to taskbar" and "Unpin from taskbar".
  // Reading the localised name is what lets the verb be found on a Windows
  // that is not in English.
  RES_PIN_TO_TASKBAR = 5386;
  RES_UNPIN_FROM_TASKBAR = 5387;
  LOAD_LIBRARY_AS_DATAFILE = $00000002;

function IsProcessorFeaturePresent(Feature: DWORD): BOOL;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';
function LoadLibraryExW(FileName: String; F: THandle; Flags: DWORD): THandle;
  external 'LoadLibraryExW@kernel32.dll stdcall';
function FreeLibrary(Module: THandle): BOOL;
  external 'FreeLibrary@kernel32.dll stdcall';
function LoadStringW(Instance: THandle; Id: Cardinal; Buffer: String; BufferMax: Integer): Integer;
  external 'LoadStringW@user32.dll stdcall';

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

// An axis with a box answers with the box. An axis without one keeps the value
// the payload pinned it to, so the three answers can never name a build that
// is not in the file.
function AxisValue(Index: Integer; Pinned: Boolean): Boolean;
begin
  if Index < 0 then
    Result := Pinned
  else
    Result := Checked(Index);
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
  Result := ComposeFeature(AxisValue(IdxAvx2, PinAvx2),
                           AxisValue(IdxOmp, PinOmp),
                           AxisValue(IdxCuda, PinCuda));
end;

function WantVariant(Name: String): Boolean;
begin
  Result := SelectedFeature = Name;
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
function WantSolverOnly(Name: String): Boolean;
begin
  Result := WantVariant(Name) and (not UiChosen);
end;

// Spelled as a function of its own rather than "not UiChosen" written into the
// [InstallDelete] lines, so those read the same whichever Inno version compiles
// them.
function UiNotChosen: Boolean;
begin
  Result := not UiChosen;
end;

// What the shortcuts point at - the second question in [Tasks], asked in one
// place and answered for the desktop, the Start Menu and the taskbar alike. An
// install without the UI never sees those boxes, so it falls back to the only
// program it has.
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

// ---- the taskbar --------------------------------------------------------
// Windows has no supported way for an installer to pin anything. Up to
// Windows 10 the shell exposed a "Pin to taskbar" verb on the file itself, and
// driving that verb is what every installer that manages it does. Windows 11
// removed the verb, so on it this returns False and the user is told to do it
// by hand rather than left wondering why nothing appeared.

function StripAmp(const S: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
    if S[I] <> '&' then
      Result := Result + S[I];
end;

// Whatever goes wrong in here - a shell32 that will not load, a Windows that
// renumbered its resources - is not worth an error dialog: an empty string
// just sends ShellVerb to its substring fallback.
function ShellString(ResId: Integer): String;
var
  Lib: THandle;
  Buffer: String;
  Len: Integer;
begin
  Result := '';
  try
    Lib := LoadLibraryExW(ExpandConstant('{sys}\shell32.dll'), 0, LOAD_LIBRARY_AS_DATAFILE);
    if Lib = 0 then
      Exit;
    try
      SetLength(Buffer, 512);
      Len := LoadStringW(Lib, ResId, Buffer, 512);
      if Len > 0 then
        Result := Copy(Buffer, 1, Len);
    finally
      FreeLibrary(Lib);
    end;
  except
    Result := '';
  end;
end;

function ShellVerb(const FileName: String; ResId: Integer; const Fallback: String): Boolean;
var
  Shell, Folder, Item, Verbs, Verb: Variant;
  I: Integer;
  Wanted, Name: String;
begin
  Result := False;
  if not FileExists(FileName) then
    Exit;
  Wanted := Lowercase(StripAmp(ShellString(ResId)));
  try
    Shell := CreateOleObject('Shell.Application');
    Folder := Shell.NameSpace(ExtractFileDir(FileName));
    if VarIsNull(Folder) or VarIsEmpty(Folder) then
      Exit;
    Item := Folder.ParseName(ExtractFileName(FileName));
    if VarIsNull(Item) or VarIsEmpty(Item) then
      Exit;
    Verbs := Item.Verbs;
    for I := 0 to Verbs.Count - 1 do
    begin
      Verb := Verbs.Item(I);
      Name := Lowercase(StripAmp(Verb.Name));
      if ((Wanted <> '') and (Name = Wanted)) or
         ((Fallback <> '') and (Pos(Fallback, Name) > 0)) then
      begin
        Verb.DoIt;
        Result := True;
        Exit;
      end;
    end;
  except
    Result := False;
  end;
end;

function PinToTaskbar(const FileName: String): Boolean;
begin
  Result := ShellVerb(FileName, RES_PIN_TO_TASKBAR, 'pin to taskbar');
end;

function UnpinFromTaskbar(const FileName: String): Boolean;
begin
  Result := ShellVerb(FileName, RES_UNPIN_FROM_TASKBAR, 'unpin from taskbar');
end;

// One attempt, folded into two running lists so the report at the end can say
// what went up and what Windows refused in one dialog each rather than one per
// program.
procedure PinOne(const FileName, Title: String; var Pinned, Refused: String);
begin
  if not FileExists(FileName) then Exit;
  if PinToTaskbar(FileName) then
  begin
    if Pinned <> '' then Pinned := Pinned + ' and ';
    Pinned := Pinned + '"' + Title + '"';
  end
  else
  begin
    if Refused <> '' then Refused := Refused + ' and ';
    Refused := Refused + '"' + Title + '"';
  end;
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

  // The same list for the UI, which is built per variant too. It is a subset
  // of Available - possibly an empty one - and NextButtonClick uses it to catch
  // "UI ticked, but not for the build you chose" before anything is copied.
  UiAvailable := TStringList.Create;
#if HaveUiVariant('avx2-omp-cuda')
  UiAvailable.Add('avx2-omp-cuda');
#endif
#if HaveUiVariant('avx2-omp')
  UiAvailable.Add('avx2-omp');
#endif
#if HaveUiVariant('avx2-cuda')
  UiAvailable.Add('avx2-cuda');
#endif
#if HaveUiVariant('avx2')
  UiAvailable.Add('avx2');
#endif
#if HaveUiVariant('omp-cuda')
  UiAvailable.Add('omp-cuda');
#endif
#if HaveUiVariant('omp')
  UiAvailable.Add('omp');
#endif
#if HaveUiVariant('cuda')
  UiAvailable.Add('cuda');
#endif
#if HaveUiVariant('plain')
  UiAvailable.Add('plain');
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

  // What a hidden axis is worth. Set for all three, and used by AxisValue for
  // whichever of them ended up without a box.
  PinAvx2 := {#PinAvx2Value};
  PinOmp := {#PinOmpValue};
  PinCuda := {#PinCudaValue};

  // A switch is shown only when the payload has builds both with and without
  // it. That is what keeps CUDA off the 32-bit installer without a second copy
  // of this file, and what stops a box from offering a build that is not here.
#if ShowAvx2
  IdxAvx2 := FeaturePage.Add('AVX2 - vector kernels. Needs an Intel or AMD CPU from about 2013 on.');
#endif
#if ShowOmp
  IdxOmp := FeaturePage.Add('OpenMP - use every core of the CPU instead of one.');
#endif
#if ShowCuda
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

  if AxisValue(IdxAvx2, PinAvx2) and (not HasAvx2) then
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
    // The '+' leads the line on purpose: a line whose first non-blank
    // character is '#' is a preprocessor directive to ISPP, and '#13' is not
    // one of them, so wrapping before the constant instead of after it is what
    // makes this compile at all.
    MsgBox('This installer does not carry the "' + Feature + '" build.'
           + #13#10#13#10 + 'It has:' + List, mbError, MB_OK);
    Result := False;
    Exit;
  end;

#if HaveUi
  // The UI ships per variant as well, so ticking the component is not enough:
  // there has to be a UI for the build that was just chosen. Saying so here
  // beats installing a solver with nothing beside it and no explanation.
  if IsComponentSelected('ui') and (UiAvailable.IndexOf(Feature) < 0) then
  begin
    List := '';
    for I := 0 to UiAvailable.Count - 1 do
      List := List + #13#10 + '    ' + UiAvailable[I];
    if List = '' then
      List := #13#10 + '    (none)';
    MsgBox('There is no desktop UI built for the "' + Feature + '" solver.'
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
  if AxisValue(IdxCuda, PinCuda) and (not HasNvidia) then
    if MsgBox('No NVIDIA driver was found on this machine, so the CUDA build ' +
              'will fall back to the CPU every time it runs.' + #13#10#13#10 +
              'Install it anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
end;

// When every axis is pinned there is nothing on the page, so it is skipped
// rather than shown empty.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if (FeaturePage <> nil) and (PageID = FeaturePage.ID) then
    Result := FeaturePage.CheckListBox.Items.Count = 0;
end;

// The AVX2 guard again, for the run where the page above was skipped. A payload
// that varies on nothing pins all three axes, and the check that lives in
// NextButtonClick then never fires - which is how a dist of AVX2-only rows
// could install one on a CPU that cannot execute it, with nothing said.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if (FeaturePage <> nil) and (FeaturePage.CheckListBox.Items.Count > 0) then Exit;
  if AxisValue(IdxAvx2, PinAvx2) and (not HasAvx2) then
    Result := 'This installer only carries AVX2 builds, and this CPU does not ' +
              'support AVX2. The solver would stop with an "illegal instruction" ' +
              'error on the first run.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Pinned, Refused: String;
begin
  // Before the files move. [InstallDelete] is about to take the shortcuts this
  // run does not want back out, and the shell can only unpin a file that is
  // still on disk.
  if CurStep = ssInstall then
  begin
    if NoUiIcon then
      UnpinFromTaskbar(ExpandConstant('{app}\Fluid Solver UI.exe'));
    if NoConsoleIcon then
      UnpinFromTaskbar(ExpandConstant('{app}\Fluid Solver.exe'));
    Exit;
  end;
  if CurStep <> ssPostInstall then Exit;
  if not WizardIsTaskSelected('taskbar') then Exit;

  // Both, one or neither - whatever the second group of tick boxes said.
  Pinned := '';
  Refused := '';
  if WantUiIcon then
    PinOne(ExpandConstant('{app}\Fluid Solver UI.exe'), 'Fluid Solver UI', Pinned, Refused);
  if WantConsoleIcon then
    PinOne(ExpandConstant('{app}\Fluid Solver.exe'), 'Fluid Solver', Pinned, Refused);

  if Pinned <> '' then
    MsgBox(Pinned + ' has been pinned to the taskbar.', mbInformation, MB_OK);
  if Refused <> '' then
    MsgBox('Windows would not let the installer pin ' + Refused + ' to the ' +
           'taskbar. Since Windows 11 this is blocked for every program, not ' +
           'just this one.' + #13#10#13#10 +
           'Open the Start Menu, right-click it and choose "Pin to taskbar".',
           mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Before the files go, so the shell still has something to unpin.
  if CurUninstallStep = usUninstall then
  begin
    UnpinFromTaskbar(ExpandConstant('{app}\Fluid Solver UI.exe'));
    UnpinFromTaskbar(ExpandConstant('{app}\Fluid Solver.exe'));
  end;
end;
