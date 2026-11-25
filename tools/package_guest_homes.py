"""Build and validate the optional profile-specific guest-home layer.

Run only against a frozen snapshot of the effective MO2 plugins. Distribute
the exact same result to both clients; never regenerate IDs for existing saves.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
GUEST = ("SkyrimCoopGuestHomes.esp", "SkyrimTogetherReborn/config/CoopHomes.txt")


def package(base, snapshot, loadorder, plugins, output, instances=False):
    members = ("SkyrimCoopHomeInstances.esp", "SkyrimTogetherReborn/config/CoopHomeInstances.txt") if instances else GUEST
    if output.exists():
        raise FileExistsError(output)
    if not output.parent.is_dir():
        raise FileNotFoundError(output.parent)
    with tempfile.TemporaryDirectory(dir=output.parent) as work:
        work = Path(work)
        generated = work / "generated"
        subprocess.run([sys.executable, str(ROOT / "tools/build_guest_homes.py"),
                        str(snapshot), str(loadorder), str(plugins), str(generated)] + (["--instances"] if instances else []), check=True)
        subprocess.run([sys.executable, str(ROOT / ("tests/check_home_instances_plugin.py" if instances else "tests/check_guest_homes.py")),
                        str(generated)], check=True)
        temporary = work / "package.zip"
        with ZipFile(base) as source, ZipFile(temporary, "w", compression=ZIP_DEFLATED) as result:
            names = [n.casefold() for n in source.namelist()]
            if len(names) != len(set(names)):
                raise ValueError("Duplicate paths in base package")
            if any(n.casefold() in names for n in (*GUEST, "SkyrimCoopHomeInstances.esp", "SkyrimTogetherReborn/config/CoopHomeInstances.txt")):
                raise ValueError("Base package already contains guest-home data; do not regenerate over it")
            for required in ("SkyrimTogether.esp", "SkyrimCoopTrade.esp",
                             "SkyrimTogetherReborn/SkyrimTogether.exe"):
                if required.casefold() not in names:
                    raise ValueError(f"Not a complete Interaction06 base package: {required}")
            for item in source.infolist():
                with source.open(item) as src, result.open(item, "w") as dst:
                    shutil.copyfileobj(src, dst)
            result.write(generated / members[0], members[0])
            result.write(generated / Path(members[1]).name, members[1])
        with ZipFile(temporary) as result:
            if result.testzip() is not None:
                raise ValueError("Corrupt output ZIP")
            for member in members:
                if result.getinfo(member).file_size == 0:
                    raise ValueError(f"Empty member: {member}")
        # Exclusive creation prevents accidental replacement of a previous build.
        with output.open("xb") as dst:
            try:
                with temporary.open("rb") as src:
                    shutil.copyfileobj(src, dst)
            except BaseException:
                dst.close()
                output.unlink()
                raise
    with output.open("rb") as file:
        print("SHA-256", hashlib.file_digest(file, "sha256").hexdigest(), output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "snapshot", "loadorder", "plugins", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--instances", action="store_true")
    args = parser.parse_args()
    package(args.base, args.snapshot, args.loadorder, args.plugins, args.output, args.instances)
