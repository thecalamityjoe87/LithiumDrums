; Lithium Drums installer for Windows (Inno Setup 6 or 7).
;
; scripts/package.ps1 stages the build into a folder and compiles this with
;   ISCC /DAppVersion=<version> /DStaging=<staged folder> /DOutputDir=<dist> LithiumDrums.iss
; The staged folder holds plugins\, standalone\, tools\, presets\, LICENSE,
; THIRD-PARTY-NOTICES.txt and README.md.
;
; The wizard: welcome (what Lithium Drums is and the two ways to use it),
; licence, components (standalone app, VST3, CLAP, the kit importer), Start
; menu and desktop shortcuts, install, and a finish page that offers to open
; the standalone app.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Staging
  #define Staging "..\..\dist\staging"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

#define AppName "Lithium Drums"
#define Company "Calamity Audio"
#define Author "Isaac Joseph"
#define Website "https://github.com/thecalamityjoe87"
#define Assets "..\..\assets\icon"

[Setup]
; Never change AppId: Windows uses it to find an earlier install to upgrade.
AppId={{502CFAB0-5F7A-4909-86CC-C1283A426179}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#Company}
AppPublisherURL={#Website}
AppSupportURL={#Website}
AppCopyright=Copyright (C) 2026 {#Author}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#Company}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#Staging}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=LithiumDrums-{#AppVersion}-windows-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The plugin folders are under Common Files, which needs admin rights.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
#if FileExists(AddBackslash(SourcePath) + Assets + "\lithium-drums.ico")
SetupIconFile={#Assets}\lithium-drums.ico
UninstallDisplayIcon={app}\Lithium Drums.exe
#endif
#if FileExists(AddBackslash(SourcePath) + Assets + "\installer-side.bmp")
WizardImageFile={#Assets}\installer-side.bmp,{#Assets}\installer-side-2x.bmp
WizardSmallImageFile={#Assets}\installer-small.bmp,{#Assets}\installer-small-2x.bmp
#endif

[Messages]
WelcomeLabel2=This installs [name/ver] by {#Company}.%n%nLithium Drums works two ways:%n%n  - as a plugin (VST3 or CLAP) inside your DAW, on an instrument track;%n  - as a standalone app, with no DAW needed: play it from an e-kit or MIDI keyboard.%n%nThe drum kits aren't in this installer. Open Lithium Drums and pick one in the kit browser; it downloads it for you.
FinishedLabel=Lithium Drums is installed.%n%nIn your DAW, rescan plugins and add Lithium Drums (by {#Company}) to an instrument track. Or open the standalone app from the Start menu.

[Types]
Name: "full"; Description: "Standalone app and all plugin formats"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "standalone"; Description: "Standalone app (runs on its own, no DAW needed)"; Types: full custom
Name: "vst3"; Description: "VST3 plugin (Cubase, Reaper, Studio One, FL Studio, Ableton Live, Bitwig...)"; Types: full custom
Name: "clap"; Description: "CLAP plugin (Bitwig, Reaper, FL Studio...)"; Types: full custom
Name: "tools"; Description: "LithiumImport (command-line kit importer, for advanced users)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for the standalone app"; Components: standalone; Flags: unchecked

[Files]
Source: "{#Staging}\standalone\Lithium Drums.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#Staging}\plugins\Lithium Drums.vst3\*"; DestDir: "{commoncf64}\VST3\Lithium Drums.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Staging}\plugins\Lithium Drums.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "{#Staging}\tools\LithiumImport.exe"; DestDir: "{app}\tools"; Components: tools; Flags: ignoreversion skipifsourcedoesntexist
; Factory presets go where the plugin looks for them (Settings::getFactoryPresetsFolder).
Source: "{#Staging}\presets\*"; DestDir: "{commonappdata}\Lithium Drums\Presets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Staging}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Staging}\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Staging}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Lithium Drums"; Filename: "{app}\Lithium Drums.exe"; Components: standalone
Name: "{group}\Uninstall Lithium Drums"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Lithium Drums"; Filename: "{app}\Lithium Drums.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Lithium Drums.exe"; Description: "Open the Lithium Drums standalone app now"; Components: standalone; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; Only what the installer put there. The user's kits, presets and settings
; (in %LOCALAPPDATA% and %APPDATA%) stay.
Type: dirifempty; Name: "{commonappdata}\Lithium Drums"
