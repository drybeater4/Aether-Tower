@echo off
REM One-click launcher: Pizza Tower (patched) first, then Rivals of Aether (hidden, runs the character).
REM In Pizza Tower: F6 = Rivals on/off, F7 = pick a character, F8 = input mode.
powershell -ExecutionPolicy Bypass -File "%~dp0tools\play.ps1" %*
