@echo off
REM Launches Trace with the D3D11 GPU renderer instead of the default CPU one.
REM The HUD's "renderer" field will read d3d11 if it engaged, or cpu if it fell back.
set TRACE_RENDERER=d3d11
start "" "%~dp0build\app\Release\Trace.exe"
