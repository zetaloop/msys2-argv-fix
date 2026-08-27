# msys2-argv-fix

Fixes command-line argument handling between Windows and MSYS2.

The x64 DLL loads through `LD_PRELOAD` and patches the MSYS2 runtime in memory.

### Windows to MSYS2

MSYS2 passes command lines from native Windows processes through [Cygwin's globbing code](https://cygwin.com/pipermail/cygwin/2014-May/215372.html). That code uses fixed 8192-character buffers, so long arguments such as large `apply_patch` payloads can be cut short.

The DLL recovers the original Win32 command line and rebuilds `argv` from it. Arguments without unquoted expansion syntax skip the globber; unquoted glob, brace, and tilde patterns still use MSYS2's normal expansion logic.

### MSYS2 to Windows

MSYS2 uses a broad heuristic to convert arguments for native child processes. That heuristic can mistake URLs, regular expressions, path lists, and option values for paths. `msys2-argv-fix` narrows the conversion to unambiguous paths that exist:

- absolute POSIX paths beginning with `/`
- relative paths beginning with `./` or `../`
- path values in options such as `--file=/path`

Single-letter switches such as `/c`, URLs, regular expressions, path lists, joined short options, and paths that do not exist remain unchanged. `MSYS2_ARG_CONV_EXCL` can still exclude arguments with semicolon-separated prefixes; `*` disables conversion entirely.
