@echo off
rem ico_vr.cmd: starts ICO in VR on the system's OpenXR runtime (SteamVR, or
rem the headset's own). Start the runtime first, then double-click this.
rem
rem SCALE multiplies the eye image size the headset asks for, across and
rem down (0.1 to 4). Leave it at 1: the game's own row does the same job
rem while you play (Options, Display, VR resolution), and SteamVR's per-app
rem video settings set the size the headset asks for.
rem
rem The repository stores this file with LF line ends, and the package
rem converts it to CRLF (no labels or goto here for that reason).
set SCALE=1

cd /d "%~dp0"
set ICO_VR=1
set ICO_VR_SCALE=%SCALE%
start "" "%~dp0ico_pc_x64.exe" %*
