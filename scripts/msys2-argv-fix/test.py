import ctypes
import json
import os
import shlex
import subprocess
import sys
import tempfile
from ctypes import wintypes
from pathlib import Path

get_long_path_name = ctypes.WinDLL("kernel32", use_last_error=True).GetLongPathNameW
get_long_path_name.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
get_long_path_name.restype = wintypes.DWORD


def normalize_windows_argument(argument: str) -> str:
    prefix = "--file=" if argument.startswith("--file=") else ""
    path = argument[len(prefix) :]
    if len(path) < 3 or path[1] != ":" or path[2] not in "\\/":
        return argument
    path = path.replace("/", "\\")
    buffer = ctypes.create_unicode_buffer(32768)
    length = get_long_path_name(path, buffer, len(buffer))
    if not length:
        raise ctypes.WinError(ctypes.get_last_error())
    return prefix + buffer.value


root = Path(__file__).parents[2]
dll = (root / "dist/msys2-argv-fix.dll").resolve()
git_exec_path = Path(subprocess.check_output(["git", "--exec-path"], text=True).strip())
git_root = next(
    path for path in git_exec_path.parents if (path / "usr/bin/bash.exe").exists()
)
bash = git_root / "usr/bin/bash.exe"


def msys_path(path: Path) -> str:
    return f"/proc/cygdrive/{path.drive[0].lower()}{path.as_posix()[2:]}"


env = os.environ | {"LD_PRELOAD": msys_path(dll)}

result = subprocess.run(
    [bash, "-c", 'printf %s "$0:$1"; : ' + "x" * 20_000, "zero", "one"],
    env=env,
    capture_output=True,
    check=False,
)
assert result.returncode == 0, result.stderr.decode(errors="replace")
assert result.stdout == b"zero:one"

with tempfile.TemporaryDirectory() as directory:
    probe = Path(directory) / "probe.dll"
    probe.touch()
    pattern = msys_path(probe.with_suffix(".dl?"))
    result = subprocess.run(
        [bash, "-c", 'printf %s "$1"', "zero", pattern],
        env=env,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    assert result.stdout.decode() == msys_path(probe)

    existing = Path(directory) / "存在 file.txt"
    existing.touch()
    missing = Path(directory) / "missing.txt"
    subdirectory = Path(directory) / "subdirectory"
    subdirectory.mkdir()
    scoop = Path(directory) / "Scoop"
    junction = scoop / "apps/pnpm/current/bin"
    junction_target = scoop / "persist/pnpm/bin"
    junction_target.mkdir(parents=True)
    linked = scoop / "persist/target.txt"
    linked.touch()
    junction.parent.mkdir(parents=True)
    result = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(junction), str(junction_target)],
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    linked_path = junction / "../../../../persist/target.txt"
    linked_posix = f"/{linked_path.drive[0].lower()}{linked_path.as_posix()[2:]}"
    native_python = shlex.quote(msys_path(Path(sys.executable)))
    existing_posix = msys_path(existing)
    print_arguments = shlex.quote(
        "import json,sys; print(json.dumps(sys.argv[1:], ensure_ascii=True))"
    )
    native_command = f"{native_python} -c {print_arguments}"
    script = "; ".join(
        [
            "unset MSYS2_ARG_CONV_EXCL",
            f"{native_command} {shlex.quote(existing_posix)} --file={shlex.quote(existing_posix)} '/foo.*/' {shlex.quote(msys_path(missing))}",
            f"export MSYS2_ARG_CONV_EXCL={shlex.quote(existing_posix)}",
            f"{native_command} {shlex.quote(existing_posix)}",
            "export MSYS2_ARG_CONV_EXCL='*'",
            f"{native_command} {shlex.quote(existing_posix)}",
            f"cd {shlex.quote(msys_path(subdirectory))}",
            "unset MSYS2_ARG_CONV_EXCL",
            f"{native_command} {shlex.quote('../' + existing.name)} {shlex.quote(linked_posix)}",
        ]
    )
    result = subprocess.run(
        [bash, "-c", script], env=env, capture_output=True, check=False
    )
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    output = [
        [normalize_windows_argument(argument) for argument in json.loads(line)]
        for line in result.stdout.decode().splitlines()
    ]
    existing_windows = str(existing.resolve())
    linked_windows = normalize_windows_argument(linked_path.as_posix())
    assert output == [
        [
            existing_windows,
            f"--file={existing_windows}",
            "/foo.*/",
            msys_path(missing),
        ],
        [existing_posix],
        [existing_posix],
        [existing_windows, linked_windows],
    ], output
