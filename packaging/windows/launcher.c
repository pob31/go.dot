/*
    This file is part of Go.dot - https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
    Go.dot.exe: what a Windows user double-clicks, and what a .wfg opens with.
    The Windows twin of the Mac's launcher.c and launch.sh, and it replaces
    Go.dot.cmd.

    WHY A PROGRAM AND NOT THE SCRIPT. wfg.exe is a console program - it is a
    command-line tool first - so Windows gives it a black console window
    whenever it is started from Explorer, and so did the .cmd. This is a
    window program with no window of its own: it starts wfg.exe with no
    console, and its output goes to a log file instead, as the Mac's does.
    It also carries the app icon and the .wfg page icon, and the name
    "Go.dot" that "Open with" shows.

    WHAT IT OPENS:
      - a show folder or a .wfg given to it (a double-click on a .wfg, a
        drop on this file, the installer's association): that show;
      - nothing: the empty show, copied once to %APPDATA%\Go.dot\Untitled
        and opened from there with its show settings (--show-settings).
        Copied because beside this file is Program Files once installed,
        where a save would be refused - the Mac copies it out of its bundle
        for the same reason.

    THE LOG is %APPDATA%\Go.dot\logs\go.dot-<date>-<time>.log, one per start,
    beside the session logs wfg already writes there.

    IT DOES NOT WAIT. wfg says for itself when it cannot open a show, in the
    system's alert (src/wfg/engine/app/StartupReport.h), and a double-clicked
    show that is already open brings its window forward (app/OpenShows.h) -
    which Windows allows it only because this program, started from Explorer,
    passes the right on (AllowSetForegroundWindow).

    NO --device, for the reason the Mac's launch.sh gives: the show names its
    interface.
*/

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

#define PATH_CHARS 32768

static void fail (const wchar_t* what)
{
    wchar_t message[1024];
    const DWORD error = GetLastError();
    wchar_t reason[512] = L"";

    FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0,
                    reason, (DWORD) (sizeof (reason) / sizeof (reason[0])), NULL);

    _snwprintf_s (message, sizeof (message) / sizeof (message[0]), _TRUNCATE, L"%s\n\n%s", what, reason);
    MessageBoxW (NULL, message, L"Go.dot", MB_OK | MB_ICONERROR);
}

/*  A path passed inside quotes must not end in a backslash, which would
    escape the closing quote; a folder never needs one. */
static void trimTrailingSeparators (wchar_t* path)
{
    size_t length = wcslen (path);

    while (length > 3 && (path[length - 1] == L'\\' || path[length - 1] == L'/'))
        path[--length] = L'\0';
}

/*  <from> copied, whole, to become <to>: its files, and its folders the same
    way. By hand rather than with the shell's copy, which made the folder and
    left it empty when it was asked to name it. */
static BOOL copyFolder (const wchar_t* from, const wchar_t* to)
{
    wchar_t pattern[MAX_PATH * 2];
    WIN32_FIND_DATAW found;
    HANDLE search;
    BOOL ok = TRUE;

    if (! CreateDirectoryW (to, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;

    _snwprintf_s (pattern, sizeof (pattern) / sizeof (pattern[0]), _TRUNCATE, L"%s\\*", from);
    search = FindFirstFileW (pattern, &found);

    if (search == INVALID_HANDLE_VALUE)
        return FALSE;

    do
    {
        wchar_t source[MAX_PATH * 2];
        wchar_t destination[MAX_PATH * 2];

        if (wcscmp (found.cFileName, L".") == 0 || wcscmp (found.cFileName, L"..") == 0)
            continue;

        _snwprintf_s (source, sizeof (source) / sizeof (source[0]), _TRUNCATE, L"%s\\%s", from, found.cFileName);
        _snwprintf_s (destination, sizeof (destination) / sizeof (destination[0]), _TRUNCATE, L"%s\\%s", to, found.cFileName);

        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            ok = copyFolder (source, destination) && ok;
        else
            ok = CopyFileW (source, destination, TRUE) && ok;
    }
    while (FindNextFileW (search, &found));

    FindClose (search);
    return ok;
}

int WINAPI WinMain (HINSTANCE instance, HINSTANCE previous, LPSTR ignored, int show)
{
    static wchar_t here[PATH_CHARS];
    static wchar_t home[PATH_CHARS];
    static wchar_t folder[PATH_CHARS];
    static wchar_t chosen[PATH_CHARS];
    static wchar_t logPath[PATH_CHARS];
    static wchar_t wfg[PATH_CHARS];
    static wchar_t command[PATH_CHARS * 2];
    const wchar_t* extra = L"";
    int argumentCount = 0;
    wchar_t** arguments;
    wchar_t* slash;
    SYSTEMTIME now;
    SECURITY_ATTRIBUTES inherited;
    HANDLE log;
    HANDLE nothing;
    STARTUPINFOW startup;
    PROCESS_INFORMATION started;

    (void) instance; (void) previous; (void) ignored; (void) show;

    //  --- where this is, and so where wfg.exe and console/ are ---------------
    if (GetModuleFileNameW (NULL, here, PATH_CHARS) == 0)
    {
        fail (L"Go.dot could not find where it is.");
        return 1;
    }

    slash = wcsrchr (here, L'\\');
    if (slash != NULL)
        *slash = L'\0';

    _snwprintf_s (wfg, PATH_CHARS, _TRUNCATE, L"%s\\wfg.exe", here);

    //  --- Go.dot's own folder, and its logs -----------------------------------
    if (GetEnvironmentVariableW (L"APPDATA", home, PATH_CHARS) == 0)
    {
        fail (L"Go.dot could not find your application data folder (%APPDATA%).");
        return 1;
    }

    _snwprintf_s (folder, PATH_CHARS, _TRUNCATE, L"%s\\Go.dot", home);
    CreateDirectoryW (folder, NULL);
    _snwprintf_s (logPath, PATH_CHARS, _TRUNCATE, L"%s\\logs", folder);
    CreateDirectoryW (logPath, NULL);

    //  --- which show ----------------------------------------------------------
    arguments = CommandLineToArgvW (GetCommandLineW(), &argumentCount);

    if (arguments != NULL && argumentCount > 1 && arguments[1][0] != L'\0')
    {
        if (GetFullPathNameW (arguments[1], PATH_CHARS, chosen, NULL) == 0)
            wcsncpy_s (chosen, PATH_CHARS, arguments[1], _TRUNCATE);
    }
    else
    {
        wchar_t shipped[PATH_CHARS];
        wchar_t manifest[PATH_CHARS];

        _snwprintf_s (chosen, PATH_CHARS, _TRUNCATE, L"%s\\Untitled", folder);
        _snwprintf_s (shipped, PATH_CHARS, _TRUNCATE, L"%s\\Untitled", here);
        _snwprintf_s (manifest, PATH_CHARS, _TRUNCATE, L"%s\\Untitled.wfg", chosen);

        //  By its manifest, not the folder: a copy cut short is copied again.
        if (GetFileAttributesW (manifest) == INVALID_FILE_ATTRIBUTES && ! copyFolder (shipped, chosen))
        {
            fail (L"Go.dot could not copy the empty show to %APPDATA%\\Go.dot\\Untitled.");
            return 1;
        }

        extra = L" --show-settings";
    }

    if (arguments != NULL)
        LocalFree (arguments);

    trimTrailingSeparators (chosen);
    trimTrailingSeparators (here);

    //  --- the log ---------------------------------------------------------------
    GetLocalTime (&now);
    _snwprintf_s (logPath, PATH_CHARS, _TRUNCATE, L"%s\\logs\\go.dot-%04u%02u%02u-%02u%02u%02u.log", folder,
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    ZeroMemory (&inherited, sizeof (inherited));
    inherited.nLength = sizeof (inherited);
    inherited.bInheritHandle = TRUE;

    log = CreateFileW (logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    nothing = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    //  --- wfg.exe, with no console ------------------------------------------------
    //  --ui is resolved against the working directory, which is set to here as well.
    _snwprintf_s (command, PATH_CHARS * 2, _TRUNCATE, L"\"%s\" serve \"%s\" --window \"--ui=%s\\console\"%s",
                  wfg, chosen, here, extra);

    ZeroMemory (&startup, sizeof (startup));
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nothing;
    startup.hStdOutput = log;
    startup.hStdError = log;
    ZeroMemory (&started, sizeof (started));

    if (! CreateProcessW (wfg, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                          NULL, here, &startup, &started))
    {
        fail (L"Go.dot could not start wfg.exe, which should be beside it.");
        return 1;
    }

    AllowSetForegroundWindow (started.dwProcessId);

    CloseHandle (started.hThread);
    CloseHandle (started.hProcess);

    if (log != INVALID_HANDLE_VALUE)     CloseHandle (log);
    if (nothing != INVALID_HANDLE_VALUE) CloseHandle (nothing);

    return 0;
}
