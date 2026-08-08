import os
import subprocess
import tempfile
from pathlib import Path

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

cases = [
    ([bash, "-c", "printf %s " + "x" * 30_000], None, b"x" * 30_000, 0),
    (
        [bash, "-c", 'printf %s "$0:$1"; : ' + "x" * 20_000, "zero", "one"],
        None,
        b"zero:one",
        0,
    ),
    ([bash, "-c", "cat; : " + "x" * 20_000], b"stdin", b"stdin", 0),
    ([bash, "-c", "exit 7; : " + "x" * 20_000], None, b"", 7),
]

for arguments, stdin, stdout, returncode in cases:
    result = subprocess.run(
        arguments, input=stdin, env=env, capture_output=True, check=False
    )
    assert result.returncode == returncode, result.stderr.decode(errors="replace")
    assert result.stdout == stdout

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
