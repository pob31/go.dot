@echo off
rem This file is part of Go.dot - https://github.com/pob31/go.dot
rem
rem Copyright (C) 2026 Pierre-Olivier Boulant
rem
rem Go.dot is free software: you can redistribute it and/or modify it under the
rem terms of the GNU General Public License as published by the Free Software
rem Foundation, either version 3 of the License, or (at your option) any later
rem version. Go.dot is distributed in the hope that it will be useful, but
rem WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
rem or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
rem (LICENSE, at the repository root) for more details.
rem
rem SPDX-License-Identifier: GPL-3.0-or-later
rem
rem The Windows launcher. A double-click opens the empty show beside it; drop a
rem show folder on it to open that one instead. The console window stays open
rem with the engine's log in it, which is what a tester's report wants.

setlocal
set "HERE=%~dp0"
set "SHOW=%HERE%Untitled"
if not "%~1"=="" set "SHOW=%~f1"

rem --ui is resolved against the working directory, so run from beside the binary.
cd /d "%HERE%"
"%HERE%wfg.exe" serve "%SHOW%" --device --window --ui=console
if errorlevel 1 pause
endlocal
