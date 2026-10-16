@echo off
REM build_core.bat - build lume-core (bin/lume-core.exe) for Windows with mingw-w64.
REM Optional config is read from .env (LUME_RT_SRC, MINGW_BIN); both fall back to
REM defaults (script dir src/rt.c, D:/Software/mingw64/bin). Keep this file ASCII.
setlocal

set ROOT=%~dp0
set ROOT=%ROOT:\=/%
set OUT=bin/lume-core.exe
set ERR=build_err.txt

REM --- load .env if present (KEY=VALUE per line; comments/empty lines ignored) ---
if exist "%ROOT%.env" (
  for /f "usebackq tokens=1,* delims==" %%a in ("%ROOT%.env") do (
    if not "%%a"=="" set "%%a=%%b"
  )
)

REM --- toolchain defaults ---
if not defined MINGW_BIN set MINGW_BIN=D:/Software/mingw64/bin
set CC=%MINGW_BIN%/gcc.exe
if not defined LUME_RT_SRC set LUME_RT_SRC=%ROOT%src/rt.c
set LUME_NATIVE_SRC=%ROOT%src/bridge_native.c
set TRIPLE=x86_64-w64-mingw32

REM --- sources: Makefile SRCS minus builtins_http.c (HTTP outbound is off on Windows) ---
set SRCS=src\main.c src\lexer.c src\parser.c src\parser_stmt.c src\parser_expr.c src\value.c src\typecheck.c src\typecheck_expr.c src\typecheck_stmt.c src\interp.c src\builtins.c src\builtins_fs.c src\os_win32.c src\builtins_catalog.c src\builtins_hof.c src\builtins_str.c src\builtins_math.c src\builtins_crypt.c src\loader.c src\vdom.c src\token.c src\bridge_stub.c src\bridge_serve.c src\bridge_mcp.c src\bridge_lsp.c src\bridge_native.c src\codegen.c src\codegen_types.c src\codegen_expr.c src\codegen_scan.c src\codegen_sig.c src\codegen_stmt.c src\codegen_lit.c src\codegen_call.c src\codegen_match.c src\codegen_print.c src\irbuf.c src\backend.c

REM --- vendored POSIX regex (TRE-derived, BSD license): compiled separately so
REM --- its upstream warnings (-Wimplicit-fallthrough / -Wsign-compare) stay out
REM --- of the project's own zero-warning build; objects land in build/. ---
set REGEX_SRC=src\compat\regex\regcomp.c src\compat\regex\regexec.c src\compat\regex\regerror.c src\compat\regex\tre-mem.c

cd /d "%ROOT%"
if not exist bin mkdir bin
if not exist build mkdir build

"%CC%" -std=c11 -O2 -g -I src -Isrc/compat/regex -Wno-implicit-fallthrough -Wno-sign-compare -c src\compat\regex\regcomp.c -o build\regcomp.o
if errorlevel 1 goto :fail
"%CC%" -std=c11 -O2 -g -I src -Isrc/compat/regex -Wno-implicit-fallthrough -Wno-sign-compare -c src\compat\regex\regexec.c -o build\regexec.o
if errorlevel 1 goto :fail
"%CC%" -std=c11 -O2 -g -I src -Isrc/compat/regex -Wno-implicit-fallthrough -Wno-sign-compare -c src\compat\regex\regerror.c -o build\regerror.o
if errorlevel 1 goto :fail
"%CC%" -std=c11 -O2 -g -I src -Isrc/compat/regex -Wno-implicit-fallthrough -Wno-sign-compare -c src\compat\regex\tre-mem.c -o build\tre-mem.o
if errorlevel 1 goto :fail

REM --- vendored SHA-256 (public-domain style, no upstream file copied): same
REM --- standalone-object pattern as regex; no warning overrides needed ---
"%CC%" -std=c11 -O2 -g -I src -Isrc/compat/sha256 -c src\compat\sha256\sha256.c -o build\sha256.o
if errorlevel 1 goto :fail

"%CC%" -std=c11 -Wall -Wextra -O2 -g -I src -Isrc/compat/regex -DTARGET_TRIPLE="\"%TRIPLE%\"" -DLUME_RT_SRC="\"%LUME_RT_SRC%\"" -DLUME_NATIVE_SRC="\"%LUME_NATIVE_SRC%\"" -DLUME_HAS_HTTP=0 -DLUME_CORE_VERSION=\"0.7.0\" %SRCS% build\regcomp.o build\regexec.o build\regerror.o build\tre-mem.o build\sha256.o -lws2_32 -o "%OUT%" 2> "%ERR%"

if errorlevel 1 (
  echo Build FAILED - see %ERR%
  exit /b 1
)
echo OK: %OUT%
exit /b 0

:fail
echo Build FAILED - vendor object compile error
exit /b 1
