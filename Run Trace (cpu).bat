@echo off
REM Launches Trace with the default CPU renderer -- the A/B control.
set TRACE_RENDERER=cpu
start "" "%~dp0build\app\Release\Trace.exe"
