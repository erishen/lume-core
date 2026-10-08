@echo off
REM build_core.bat - Windows local build script.
REM Reads .env when present (LUME_RT_SRC / MINGW_BIN ...), falls back to defaults.
cd /d %~dp0
setlocal

REM --- Load .env if present: every KEY=VALUE line becomes a variable ---
if exist .env (
    for /f "eol=# tokens=1,* delims==" %%a in (.env) do set "%%a=%%b"
)

REM --- LUME_RT_SRC default: <script dir>/src/rt.c (backslashes -> forward) ---
if not defined LUME_RT_SRC (
    set "LUME_RT_SRC=%~dp0src\rt.c"
    set "LUME_RT_SRC=%LUME_RT_SRC:\=/%"
)

REM --- Compiler: prefer MINGW_BIN from .env, else gcc from PATH ---
set "CC=gcc"
if defined MINGW_BIN if exist "%MINGW_BIN%\gcc.exe" set "CC=%MINGW_BIN%\gcc.exe"

echo [build_core] CC=%CC%
echo [build_core] LUME_RT_SRC=%LUME_RT_SRC%

%CC% -std=c11 -Wall -Wextra -O2 -g -Isrc -DLUME_HAS_HTTP=1 -DTARGET_TRIPLE="\"x86_64-w64-mingw32\"" -DLUME_RT_SRC="\"%LUME_RT_SRC%\"" src\main.c src\lexer.c src\parser.c src\parser_stmt.c src\parser_expr.c src\value.c src\typecheck.c src\typecheck_expr.c src\typecheck_stmt.c src\interp.c src\builtins.c src\builtins_fs.c src\os_win32.c src\builtins_catalog.c src\builtins_hof.c src\builtins_str.c src\builtins_math.c src\builtins_crypt.c src\loader.c src\vdom.c src\token.c src\bridge_stub.c src\bridge_serve.c src\net_compat.c src\builtins_http.c src\codegen.c src\codegen_types.c src\codegen_expr.c src\codegen_scan.c src\codegen_sig.c src\codegen_stmt.c src\irbuf.c src\backend.c -o bin\lume-core.exe -lws2_32 2> build_err.txt
echo EXITCODE=%errorlevel%
endlocal
