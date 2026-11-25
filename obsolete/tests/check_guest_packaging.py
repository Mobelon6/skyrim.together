"""Synthetic plugin snapshot: test generator + validation + final MO2 paths without game data."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "required/tools"))
from build_trade_plugin import record, sub

def u(value):
    return struct.pack("<I", value)

def grp(kind, label, payload):
    if isinstance(label, int):
        label = u(label)
    return struct.pack("<4sI4sIII", b"GRUP", 24 + len(payload), label, kind, 0, 0) + payload

def ref(form, base, linked=None):
    data = sub(b"NAME", u(base)) + sub(b"DATA", bytes(24))
    if linked is not None:
        data += sub(b"XTEL", u(linked) + bytes(24))
    return record(b"REFR", 0, form, data)

def snapshot():
    data = record(b"TES4", 0, 0, sub(b"HEDR", struct.pack("<fII", 1.7, 0, 0x100000)))
    data += record(b"LCTN", 0, 0x100, sub(b"KWDA", u(0xfc1a3)))
    data += record(b"STAT", 0, 0x101, sub(b"MODL", b"fixture.nif\0"))
    data += record(b"DOOR", 0, 0x102, sub(b"MODL", b"door.nif\0"))
    data += record(b"LIGH", 0, 0x103, sub(b"DATA", bytes(16)))
    for i, cell in enumerate((91560, 93690, 92024, 92678, 93149)):
        door = 0x200 + i * 4
        contents = ref(door, 0x102, door + 1) + ref(door + 2, 0x101) + ref(door + 3, 0x103)
        data += record(b"CELL", 0, cell, sub(b"EDID", f"Fixture{i}\0".encode()) +
                       sub(b"DATA", b"\x01") + sub(b"XLCN", u(0x100)))
        data += grp(6, cell, grp(9, cell, contents))
        data += grp(6, 0x777, grp(9, 0x777, ref(door + 1, 0x102, door)))
    return data

with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    with ZipFile(p / "snapshot.zip", "w") as archive:
        archive.writestr("Data/Skyrim.esm", snapshot())
    (p / "loadorder.txt").write_text("Skyrim.esm\n")
    (p / "plugins.txt").write_text("*Skyrim.esm\n")
    base_members = {"SkyrimTogether.esp": b"base", "SkyrimCoopTrade.esp": b"trade",
                    "SkyrimTogetherReborn/SkyrimTogether.exe": b"client"}
    with ZipFile(p / "base.zip", "w") as archive:
        for name, data in base_members.items():
            archive.writestr(name, data)
    command = [sys.executable, str(ROOT / "obsolete/tools/package_guest_homes.py"), str(p / "base.zip"),
               str(p / "snapshot.zip"), str(p / "loadorder.txt"), str(p / "plugins.txt")]
    for name in ("first.zip", "second.zip"):
        subprocess.run(command + [str(p / name)], check=True)
    with ZipFile(p / "first.zip") as first, ZipFile(p / "second.zip") as second:
        assert first.testzip() is None
        assert set(first.namelist()) == set(base_members) | {
            "SkyrimCoopGuestHomes.esp", "SkyrimTogetherReborn/config/CoopHomes.txt"}
        for name, data in base_members.items():
            assert first.read(name) == data
        for name in first.namelist():
            assert first.read(name) == second.read(name)
    original = (p / "first.zip").read_bytes()
    assert subprocess.run(command + [str(p / "first.zip")], capture_output=True).returncode != 0
    assert (p / "first.zip").read_bytes() == original
print("PASS nested snapshot, five synthetic homes, both package paths, deterministic payloads and no overwrite")
