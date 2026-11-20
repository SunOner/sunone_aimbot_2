"""Exercise the updater with real Git history and stand-in build wrappers."""
from pathlib import Path
from tempfile import TemporaryDirectory
import os
import shutil
import subprocess

UPSTREAM = "https://github.com/SunOner/sunone_aimbot_2.git"
SCRIPT = Path(__file__).resolve().parents[1] / "tools/update_build.ps1"
SHELL = shutil.which("powershell") or shutil.which("pwsh")


def run_tests(folder):
    source = folder / "upstream"
    clone = folder / "user clone"
    env = dict(os.environ, LOCALAPPDATA=str(folder / "profile"))
    # Rewrite only the fixture's Git traffic; the real upstream is never contacted.
    env.update(GIT_CONFIG_COUNT="1",
               GIT_CONFIG_KEY_0=f"url.{source.as_uri()}.insteadOf",
               GIT_CONFIG_VALUE_0=UPSTREAM)

    def git(cwd, *args):
        subprocess.run(["git", *args], cwd=cwd, env=env, check=True, capture_output=True)

    def commit(path, contents):
        file = source / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(contents)
        git(source, "add", ".")
        git(source, "commit", "-qm", path)

    def invoke(*args, success=True):
        result = subprocess.run([SHELL, "-NoProfile", "-ExecutionPolicy", "Bypass",
                                 "-File", str(clone / "tools/update_build.ps1"), *args],
                                env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return result.stdout + result.stderr

    def update(expected, success=True):
        output = invoke("-Action", "Run", success=success)
        assert expected in output, output

    source.mkdir()
    git(source, "init", "-b", "main")
    git(source, "config", "user.name", "Fixture")
    git(source, "config", "user.email", "fixture@example.invalid")
    commit("tools/update_build.ps1", SCRIPT.read_text())
    commit(".gitignore", "/build/\n")
    for backend in ("dml", "cuda"):
        commit(f"tools/build_{backend}.ps1", """
param([switch]$NonInteractive, [string]$DownloadOrUpdateNeeded, [string[]]$ExtraCMakeArgs)
$ErrorActionPreference = 'Stop'
if ($ExtraCMakeArgs -ne '--fresh' -or $DownloadOrUpdateNeeded -ne 'true') { exit 2 }
$build = Join-Path (Split-Path $PSScriptRoot) 'build/BACKEND'
if (Test-Path (Join-Path $build 'fail-full')) { exit 1 }
[void][IO.Directory]::CreateDirectory("$build/Release")
foreach ($f in 'CMakeCache.txt','build-Release.ninja','Release/ai.exe','Release/runtime.dll') {
    [IO.File]::WriteAllText("$build/$f", 'fixture')
}
""".replace("BACKEND", backend))
    commit("build_no-options.ps1", """
param([string]$Backend, [switch]$NonInteractive)
if (Test-Path "$PSScriptRoot/build/dml/fail-incremental") { exit 1 }
""")
    git(folder, "clone", str(source), str(clone))
    update(" - Full")
    update(" - Current")
    commit("docs/build.md", "docs")
    update(" - Skip")
    commit("sunone_aimbot_2/app.cpp", "code")
    update(" - Incremental")
    commit("CMakeLists.txt", "changed dependencies")
    update(" - Full")

    build = clone / "build/dml"
    (build / "Release/runtime.dll").unlink()
    update(" - Full")
    commit("sunone_aimbot_2/app.cpp", "next revision")
    (build / "fail-incremental").touch()
    (build / "fail-full").touch()
    update("Retrying with dependency setup", success=False)
    assert not (build / "last-successful-update.txt").exists()
    (build / "fail-full").unlink()
    update(" - Full")

    invoke("-Action", "SetBackend", "-Backend", "CUDA")
    update(" - Full")
    assert (clone / "build/cuda/Release/ai.exe").is_file()
    invoke("-Action", "SetBackend", "-Backend", "DML")
    update(" - Current")
    (clone / "docs/build.md").write_text("local edit")
    update("tracked changes", success=False)
    assert (clone / "docs/build.md").read_text() == "local edit"
    git(clone, "restore", "docs/build.md")
    git(clone, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
        "commit", "--allow-empty", "-m", "local work")
    update("cannot fast-forward", success=False)


if __name__ == "__main__":
    if not SHELL:
        raise SystemExit("PowerShell is required")
    with TemporaryDirectory() as tmp:
        run_tests(Path(tmp))
    print("Updater integration checks passed.")
