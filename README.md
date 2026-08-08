# scoop-zsh

[![Tests](https://github.com/zetaloop/scoop-zsh/actions/workflows/ci.yml/badge.svg)](https://github.com/zetaloop/scoop-zsh/actions/workflows/ci.yml) [![Excavator](https://github.com/zetaloop/scoop-zsh/actions/workflows/excavator.yml/badge.svg)](https://github.com/zetaloop/scoop-zsh/actions/workflows/excavator.yml)

Zsh for Git for Windows, without a separate MSYS2 installation.

```bash
scoop bucket add zsh https://github.com/zetaloop/scoop-zsh
scoop install zsh/zsh
```

`msys2-argv-fix` is a tiny DLL that keeps long command payloads from native Windows programs intact in MSYS2, including large `apply_patch` payloads.

```bash
scoop install zsh/msys2-argv-fix
```

MSYS2 sends every native argument through [Cygwin's globbing code](https://cygwin.com/pipermail/cygwin/2014-May/215372.html) before `main()`, even when there is nothing to expand. The globber works through fixed 8192-character buffers, so a long quoted command is cut short for no useful reason. The DLL loads later through `LD_PRELOAD`, replaces the application's `main()` entry, and rebuilds `argv` from the untouched `GetCommandLineW()` result. Plain arguments bypass globbing; only unquoted expansion patterns go back to the runtime and retain its buffer limit.
