"""Count surviving Skyrim plugin records in the provided MO2 export. Stdlib only."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct
from zipfile import ZipFile

RECORD = struct.Struct("<4sIII8x")
FIELD = struct.Struct("<4sH")
TYPES = set("QUST SCEN DIAL INFO PACK FACT RELA NPC_ ACHR DOOR CONT ACTI FURN CELL WRLD COBJ FLOR TREE BOOK PERK SPEL ALCH WTHR KEYM LCTN MESG".split())


def entries(path):
    return [line.strip() for line in path.read_text(encoding="utf-8-sig").splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


def records(blob, start, end):
    while start < end:
        if start + RECORD.size > end:
            raise ValueError("incomplete record header")
        tag, size, flags, form = RECORD.unpack_from(blob, start)
        body = start + RECORD.size
        if tag == b"GRUP":
            after = start + size
            if after < body or after > end:
                raise ValueError("invalid group bounds")
            yield from records(blob, body, after)
        else:
            after = body + size
            if after > end:
                raise ValueError("invalid record bounds")
            yield tag, flags, form, body, after
        start = after


def master_names(blob, head):
    tag, flags, _, offset, end = head
    if tag != b"TES4" or flags & 0x40000:
        raise ValueError("expected an uncompressed TES4 header")
    masters = []
    while offset < end:
        field, length = FIELD.unpack_from(blob, offset)
        offset += FIELD.size
        if field == b"XXXX":
            if length != 4 or offset + 4 + FIELD.size > end:
                raise ValueError("invalid extended header field")
            length, = struct.unpack_from("<I", blob, offset)
            field, _ = FIELD.unpack_from(blob, offset + 4)
            offset += 4 + FIELD.size
        if offset + length > end:
            raise ValueError("invalid header field")
        if field == b"MAST":
            masters.append(blob[offset:offset + length].rstrip(b"\0").decode("utf-8").casefold())
        offset += length
    return masters


def count_profile(archive_path, loadorder, plugins):
    inactive = {line.casefold() for line in entries(plugins) if not line.startswith("*")}
    order = [plugin.casefold() for plugin in entries(loadorder) if plugin.casefold() not in inactive]
    if len(set(order)) != len(order):
        raise ValueError("duplicate plugin in load order")
    winners = set()
    with ZipFile(archive_path) as archive:
        paths = {}
        for member in archive.namelist():
            name = Path(member.replace(chr(92), "/")).name.casefold()
            if name.endswith((".esm", ".esp", ".esl")):
                if name in paths:
                    raise ValueError(f"ambiguous plugin in archive: {name}")
                paths[name] = member
        for plugin in order:
            blob = archive.read(paths[plugin])
            records_iter = records(blob, 0, len(blob))
            masters = master_names(blob, next(records_iter)) + [plugin]
            for tag, flags, form, _, _ in records_iter:
                kind = tag.decode("ascii")
                if kind not in TYPES or form == 0:
                    continue
                index = form >> 24
                if index >= len(masters):
                    raise ValueError(f"{plugin}: unresolved form {form:08X}")
                key = kind, masters[index], form & 0xFFFFFF
                if flags & 0x20:
                    winners.discard(key)
                else:
                    winners.add(key)
    counts = Counter(k for k, _, _ in winners)
    origins = defaultdict(Counter)
    for kind, plugin, _ in winners:
        origins[plugin][kind] += 1
    return {"plugins": len(order), "counts": counts, "origins": origins}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("archive", "loadorder", "plugins"):
        parser.add_argument(arg, type=Path)
    a = parser.parse_args()
    print(json.dumps(count_profile(a.archive, a.loadorder, a.plugins), indent=2, sort_keys=True))
