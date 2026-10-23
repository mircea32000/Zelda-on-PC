; Zelda on PC - Windows installer (Inno Setup 6.3 or newer).
;
; Built by tools\package.ps1, which stages the files (executable, FSR 3 runtime, fonts, community graphic packs) and
; then compiles this script. The installer asks for the folder with the player's own game dump (title folders or a
; .wua archive; archives are checked by the program itself, ZeldaOnPC.exe --check-game) and writes it to
; settings.ini; it contains no game files. The shortcuts get the game's own icon, which the installed program makes
; on the spot from the icon inside that dump (data\game.ico); without it they show the icon of the executable.
;
; Per-user installation (no administrator rights): the game keeps its saves and settings next to the executable.
; Silent installation: ZeldaOnPC-Setup-x.y.z.exe /VERYSILENT /GAMEDIR="D:\Games\BotW" [/DIR="..."] [/TASKS="desktopicon"]

#define AppName "Zelda on PC"
#define AppExe "ZeldaOnPC.exe"
#define ShortcutComment "The Legend of Zelda: Breath of the Wild on PC"
#ifndef AppVersion
  #define AppVersion "1.3.6"
#endif
#ifndef StageDir
  #define StageDir "..\build\stage"
#endif

[Setup]
AppId={{6C0B6C8E-3A0E-4B7B-9D0B-5E1F6B6E3A51}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Zelda on PC contributors
AppComments=Runs your own copy of The Legend of Zelda: Breath of the Wild on the Cemu emulator. Contains no game data.
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\src\resource\zelda_on_pc.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
InfoBeforeFile=info.txt
OutputDir=output
OutputBaseFilename=ZeldaOnPC-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\{#AppExe}"; Flags: recursesubdirs createallsubdirs ignoreversion
; the executable last: it then makes the shortcut icon from the player's dump, before the shortcuts are created
Source: "{#StageDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion; AfterInstall: ExtractGameIcon

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\data\game.ico"; Comment: "{#ShortcutComment}"; Check: HasGameIcon
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "{#ShortcutComment}"; Check: not HasGameIcon
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; IconFilename: "{app}\data\game.ico"; Comment: "{#ShortcutComment}"; Tasks: desktopicon; Check: HasGameIcon
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "{#ShortcutComment}"; Tasks: desktopicon; Check: not HasGameIcon

[UninstallDelete]
; made from the player's dump at install time / first start
Type: files; Name: "{app}\data\game.ico"

[Run]
Filename: "{app}\{#AppExe}"; Description: "Play {#AppName} now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent unchecked

[Code]
const
  CP_UTF8 = 65001;
  HighBase = '00050000';
  HighUpdate = '0005000E';
  HighDlc = '0005000C';
  RequiredUpdateVersion = 208; // update 1.5.0

var
  GamePage: TInputDirWizardPage;
  FoundBase, FoundUpdate, FoundDlc: String;
  FoundUpdateVersion: Integer;
  DiscKeyMissing: Boolean; { the program found a disc image whose key it does not have }
  GameIcon: Boolean;

function MultiByteToWideChar(CodePage: UINT; dwFlags: DWORD; const lpMultiByteStr: AnsiString; cbMultiByte: Integer;
  lpWideCharStr: String; cchWideChar: Integer): Integer; external 'MultiByteToWideChar@kernel32.dll stdcall';
function WideCharToMultiByte(CodePage: UINT; dwFlags: DWORD; const lpWideCharStr: String; cchWideChar: Integer;
  lpMultiByteStr: AnsiString; cchMultiByte: Integer; lpDefaultChar: Integer; lpUsedDefaultChar: Integer): Integer;
  external 'WideCharToMultiByte@kernel32.dll stdcall';

{ settings.ini and Cemu's settings.xml are UTF-8 }
function Utf8ToStr(const S: AnsiString): String;
var
  Len: Integer;
begin
  Result := '';
  if S = '' then
    Exit;
  Len := MultiByteToWideChar(CP_UTF8, 0, S, Length(S), Result, 0);
  SetLength(Result, Len);
  MultiByteToWideChar(CP_UTF8, 0, S, Length(S), Result, Len);
end;

function StrToUtf8(const S: String): AnsiString;
var
  Len: Integer;
begin
  Result := '';
  if S = '' then
    Exit;
  Len := WideCharToMultiByte(CP_UTF8, 0, S, Length(S), Result, 0, 0, 0);
  SetLength(Result, Len);
  WideCharToMultiByte(CP_UTF8, 0, S, Length(S), Result, Len, 0, 0);
end;

{ Title id (16 hex digits) and title version of a loose title folder, read from its code\app.xml. '' if it is none. }
function ReadTitle(const Dir: String; var Version: Integer): String;
var
  Xml: AnsiString;
  S: String;
  P: Integer;
begin
  Result := '';
  Version := 0;
  if not LoadStringFromFile(AddBackslash(Dir) + 'code\app.xml', Xml) then
    Exit;
  S := String(Xml);
  P := Pos('<title_id', S);
  if P = 0 then
    Exit;
  Delete(S, 1, P);
  Result := Uppercase(Copy(S, Pos('>', S) + 1, 16));
  P := Pos('<title_version', S);
  if P > 0 then
  begin
    Delete(S, 1, P);
    P := Pos('>', S);
    Version := StrToIntDef('$' + Copy(S, P + 1, Pos('<', S) - P - 1), 0);
  end;
end;

{ Breath of the Wild: Japan, Americas, Europe }
function IsGameRegion(const Low: String): Boolean;
begin
  Result := (Low = '101C9300') or (Low = '101C9400') or (Low = '101C9500');
end;

{ Collects the title folders below a folder as "id|version|path" (title folders are not searched further). }
procedure ScanTitles(const Dir: String; Depth: Integer; Titles: TStringList);
var
  FindRec: TFindRec;
  Id: String;
  Version: Integer;
begin
  if not DirExists(Dir) then
    Exit;
  Id := ReadTitle(Dir, Version);
  if Id <> '' then
  begin
    if (Length(Id) = 16) and IsGameRegion(Copy(Id, 9, 8)) then
      Titles.Add(Id + '|' + IntToStr(Version) + '|' + Dir);
    Exit;
  end;
  if Depth <= 0 then
    Exit;
  if FindFirst(AddBackslash(Dir) + '*', FindRec) then
  try
    repeat
      if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        ScanTitles(AddBackslash(Dir) + FindRec.Name, Depth - 1, Titles);
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

{ Looks for the base game, the update and the DLC as title folders the way the game does at startup: anywhere below
  the folder, or next to it when the base game folder itself was selected. False if the base game is not there. }
function LocateFolders(const Dir: String): Boolean;
var
  Titles: TStringList;
  I, P: Integer;
  Region, High, Rest, Path: String;
begin
  FoundBase := '';
  FoundUpdate := '';
  FoundDlc := '';
  FoundUpdateVersion := 0;
  Region := '';
  Titles := TStringList.Create;
  try
    ScanTitles(Dir, 3, Titles);
    if FileExists(AddBackslash(Dir) + 'code\app.xml') then
      ScanTitles(ExtractFileDir(RemoveBackslashUnlessRoot(Dir)), 1, Titles);
    for I := 0 to Titles.Count - 1 do
      if (Region = '') and (Copy(Titles[I], 1, 8) = HighBase) then
        Region := Copy(Titles[I], 9, 8);
    for I := 0 to Titles.Count - 1 do
    begin
      if (Region = '') or (Copy(Titles[I], 9, 8) <> Region) then
        Continue;
      High := Copy(Titles[I], 1, 8);
      Rest := Copy(Titles[I], 18, Length(Titles[I]));
      P := Pos('|', Rest);
      Path := Copy(Rest, P + 1, Length(Rest));
      if (High = HighBase) and (FoundBase = '') then
        FoundBase := Path
      else if (High = HighUpdate) and (FoundUpdate = '') then
      begin
        FoundUpdate := Path;
        FoundUpdateVersion := StrToIntDef(Copy(Rest, 1, P - 1), 0);
      end
      else if (High = HighDlc) and (FoundDlc = '') then
        FoundDlc := Path;
    end;
  finally
    Titles.Free;
  end;
  Result := FoundBase <> '';
end;

{ A file with this extension: the path itself, or a file directly in the folder. }
function HasFileOfType(const Dir, Ext: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := (CompareText(ExtractFileExt(Dir), Ext) = 0) and FileExists(Dir);
  if not Result and DirExists(Dir) and FindFirst(AddBackslash(Dir) + '*' + Ext, FindRec) then
  begin
    Result := True;
    FindClose(FindRec);
  end;
end;

{ A Wii U archive (.wua) or a disc image (.wud, .wux). }
function HasArchive(const Dir: String): Boolean;
begin
  Result := HasFileOfType(Dir, '.wua') or HasFileOfType(Dir, '.wud') or HasFileOfType(Dir, '.wux');
end;

{ Asks the program itself what it finds (it reads archives and disc images too): ZeldaOnPC.exe --check-game answers
  with a set of bits, 1 base game, 2 update, 4 the update is 1.5.0, 8 DLC; or with 16: a disc image is there, but not
  the key of that disc. The executable is unpacked to the temporary folder. }
function LocateWithProgram(const Dir: String): Boolean;
var
  Arg: String;
  ResultCode: Integer;
begin
  Result := False;
  FoundBase := '';
  FoundUpdate := '';
  FoundDlc := '';
  FoundUpdateVersion := 0;
  DiscKeyMissing := False;
  Arg := Dir;
  if (Arg <> '') and (Arg[Length(Arg)] = '\') then
    Arg := Arg + '\';
  try
    ExtractTemporaryFile('{#AppExe}');
  except
    Exit;
  end;
  if not Exec(ExpandConstant('{tmp}\{#AppExe}'), '--check-game "' + Arg + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;
  DiscKeyMissing := ResultCode = 16;
  if (ResultCode <= 0) or (ResultCode > 15) or (ResultCode and 1 = 0) then
    Exit;
  FoundBase := Dir;
  if ResultCode and 2 <> 0 then
    FoundUpdate := Dir;
  if ResultCode and 4 <> 0 then
    FoundUpdateVersion := RequiredUpdateVersion
  else if ResultCode and 2 <> 0 then
    FoundUpdateVersion := -1;
  if ResultCode and 8 <> 0 then
    FoundDlc := Dir;
  Result := True;
end;

{ Title folders are checked here; for anything else (an archive, a layout only the program knows) the program is
  asked. Thorough: ask it even when no archive is in sight. }
function LocateGame(const Dir: String; Thorough: Boolean): Boolean;
begin
  Result := LocateFolders(Dir);
  if not Result and (Thorough or HasArchive(Dir)) then
    Result := LocateWithProgram(Dir);
end;

{ First game folder of an existing Cemu installation that holds the game ('' if there is none). }
function CemuGamePath: String;
var
  Raw: AnsiString;
  Xml, Entry: String;
  P, E: Integer;
begin
  Result := '';
  if not LoadStringFromFile(ExpandConstant('{userappdata}\Cemu\settings.xml'), Raw) then
    Exit;
  Xml := Utf8ToStr(Raw);
  P := Pos('<GamePaths>', Xml);
  E := Pos('</GamePaths>', Xml);
  if (P = 0) or (E < P) then
    Exit;
  Xml := Copy(Xml, P, E - P);
  while True do
  begin
    P := Pos('<Entry>', Xml);
    E := Pos('</Entry>', Xml);
    if (P = 0) or (E < P) then
      Break;
    Entry := Copy(Xml, P + 7, E - P - 7);
    StringChangeEx(Entry, '&amp;', '&', True);
    Delete(Xml, 1, E + 7);
    if LocateGame(Entry, False) then
    begin
      Result := Entry;
      Break;
    end;
  end;
end;

procedure InitializeWizard;
var
  Dir: String;
begin
  GamePage := CreateInputDirPage(wpSelectDir, 'Game files',
    'Where is your copy of The Legend of Zelda: Breath of the Wild?',
    'Select the folder that holds your own dump of the game (Wii U version): the base game, the update 1.5.0 and, ' +
    'if you have it, the DLC. They can be folders with "code", "content" and "meta" inside, or a .wua archive; the ' +
    'base game can also be a .wud / .wux disc image with its key file.' + #13#10#13#10 +
    'You can select the folder that contains them, or the base game folder itself.',
    False, '');
  GamePage.Add('');
  Dir := ExpandConstant('{param:GAMEDIR|}');
  if Dir = '' then
    Dir := GetPreviousData('GamePath', '');
  if (Dir = '') and not WizardSilent then
    Dir := CemuGamePath;
  GamePage.Values[0] := Dir;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'GamePath', GamePage.Values[0]);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
  Located: Boolean;
begin
  Result := True;
  if CurPageID <> GamePage.ID then
    Exit;
  Dir := Trim(GamePage.Values[0]);
  GamePage.Values[0] := Dir;
  if Dir = '' then
  begin
    { a silent installation may leave it out: the game then looks in the folders of an existing Cemu installation }
    if not WizardSilent then
    begin
      MsgBox('Select the folder that holds your dump of the game.', mbError, MB_OK);
      Result := False;
    end;
    Exit;
  end;
  Located := LocateGame(Dir, True);
  if (not Located) and DiscKeyMissing then
    Result := SuppressibleMsgBox('A disc image of the game is in this folder, but not the key of that disc.' + #13#10#13#10 +
      'Disc images (.wud, .wux) are encrypted. Put the key file your dumping tool wrote next to the image (same name, ' +
      'extension .key). A keys.txt of a Cemu installation that has the key is read as well.' + #13#10#13#10 +
      'Use this folder anyway?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDYES) = IDYES
  else if not Located then
    Result := SuppressibleMsgBox('The game was not found in this folder.' + #13#10#13#10 +
      'It has to contain the base game as a folder with "code", "content" and "meta" inside, as a .wua archive or ' +
      'as a .wud / .wux disc image (directly, or up to three levels below).' + #13#10#13#10 +
      'Use this folder anyway?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDYES) = IDYES
  else if FoundUpdateVersion <> RequiredUpdateVersion then
    Result := SuppressibleMsgBox('The base game was found, but not the update 1.5.0, which is required.' + #13#10#13#10 +
      'Put your dumped update next to the base game. An update installed in Cemu is found too.' + #13#10#13#10 +
      'Continue?', mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo,
  MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + 'Game files:' + NewLine + Space + GamePage.Values[0] + NewLine;
  if FoundBase <> '' then
  begin
    Result := Result + Space + 'Base game: found' + NewLine;
    if FoundUpdate = '' then
      Result := Result + Space + 'Update: not found here' + NewLine
    else if FoundUpdateVersion = RequiredUpdateVersion then
      Result := Result + Space + 'Update: 1.5.0' + NewLine
    else if FoundUpdateVersion < 0 then
      Result := Result + Space + 'Update: an older version (1.5.0 is required)' + NewLine
    else
      Result := Result + Space + 'Update: version ' + IntToStr(FoundUpdateVersion) + ' (1.5.0 is required)' + NewLine;
    if FoundDlc = '' then
      Result := Result + Space + 'DLC: not found (optional)' + NewLine
    else
      Result := Result + Space + 'DLC: found' + NewLine;
  end;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + MemoTasksInfo;
end;

{ Runs right after the executable was copied: it writes data\game.ico from the icon of the player's dump. }
procedure ExtractGameIcon;
var
  Dir, Ico: String;
  ResultCode: Integer;
begin
  GameIcon := False;
  Dir := GamePage.Values[0];
  if Dir = '' then
    Exit;
  { a trailing backslash would escape the closing quote }
  if Dir[Length(Dir)] = '\' then
    Dir := Dir + '\';
  Ico := ExpandConstant('{app}\data\game.ico');
  ForceDirectories(ExtractFileDir(Ico));
  if Exec(ExpandConstant('{app}\{#AppExe}'), '--extract-icon "' + Dir + '" "' + Ico + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) then
    GameIcon := (ResultCode = 0) and FileExists(Ico);
  Log('Game icon: ' + IntToStr(ResultCode) + ' ' + Ico);
end;

function HasGameIcon: Boolean;
begin
  Result := GameIcon;
end;

{ Writes the game folder to settings.ini ([Game] Path=...). An existing file keeps everything else. }
procedure WriteGamePath(const Dir: String);
var
  FileName, Line, Section: String;
  Raw: AnsiString;
  Lines: TStringList;
  I, P: Integer;
  Done: Boolean;
begin
  FileName := ExpandConstant('{app}\settings.ini');
  Lines := TStringList.Create;
  try
    if LoadStringFromFile(FileName, Raw) then
      Lines.Text := Utf8ToStr(Raw)
    else
    begin
      Lines.Add('; Zelda on PC settings. This file is written by the game; everything can also be changed in-game (Esc).');
      Lines.Add('');
    end;
    Section := '';
    Done := False;
    for I := 0 to Lines.Count - 1 do
    begin
      Line := Trim(Lines[I]);
      if (Length(Line) > 1) and (Line[1] = '[') then
        Section := Lowercase(Line)
      else if Section = '[game]' then
      begin
        P := Pos('=', Line);
        if (P > 0) and (Lowercase(Trim(Copy(Line, 1, P - 1))) = 'path') then
        begin
          Lines[I] := 'Path=' + Dir;
          Done := True;
        end;
      end;
    end;
    if not Done then
    begin
      Lines.Add('[Game]');
      Lines.Add('Path=' + Dir);
    end;
    SaveStringToFile(FileName, StrToUtf8(Lines.Text), False);
  finally
    Lines.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and (GamePage.Values[0] <> '') then
    WriteGamePath(GamePage.Values[0]);
end;

{ Saves and settings are written next to the executable, so the uninstaller does not know them: ask. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  App := ExpandConstant('{app}');
  if not (DirExists(App + '\data') or FileExists(App + '\settings.ini')) then
    Exit;
  if SuppressibleMsgBox('Do you also want to delete your saved games and settings?' + #13#10#13#10 + App,
    mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
  begin
    DelTree(App + '\data', True, True, True);
    DeleteFile(App + '\settings.ini');
    RemoveDir(App);
  end;
end;
