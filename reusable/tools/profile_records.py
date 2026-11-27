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


def records(blob, start, end, context=None):
    while start < end:
        if start + RECORD.size > end:
            raise ValueError("incomplete record header")
        tag, size, flags, form = RECORD.unpack_from(blob, start)
        body = start + RECORD.size
        if tag == b"GRUP":
            after = start + size
            if after < body or after > end:
                raise ValueError("invalid group bounds")
            yield from records(blob, body, after, None if context is None else context + ((flags, form),))
        else:
            after = body + size
            if after > end:
                raise ValueError("invalid record bounds")
            row = (tag, flags, form, body, after)
            yield row if context is None else row + (context,)
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


# Quest evidence preserves the *winning record bytes*, including repeated
# CTDA and VMAD subrecords. It does not infer a turn-in rule from GetItemCount.
# BSA entries are stored in their original packed form: no archive decompressor
# or third-party dependency is needed on the MO2 machine.
QUEST_TYPES = set(b"QUST DIAL INFO SCEN FLST GLOB INGR MISC ALCH AMMO BOOK KEYM ARMO WEAP LVLI".split())
IMPLICIT = set("skyrim.esm update.esm dawnguard.esm hearthfires.esm dragonborn.esm ccbgssse001-fish.esm ccqdrsse001-survivalmode.esl ccbgssse037-curios.esl ccbgssse025-advdsgs.esm _resourcepack.esl".split())


def effective_order(loadorder, plugins):
    listed = entries(plugins)
    active = {s[1:].casefold() for s in listed if s.startswith("*")}
    inactive = {s.casefold() for s in listed if not s.startswith("*")}
    order = [s.casefold() for s in entries(loadorder) if s.casefold() not in inactive or s.casefold() in IMPLICIT]
    if len(order) != len(set(order)) or active - set(order) or set(order) - active - IMPLICIT:
        raise ValueError("inconsistent MO2 loadorder.txt / plugins.txt")
    if "skyrim.esm" not in order:
        raise ValueError("Skyrim.esm missing from effective load order")
    return order


def bsa_scripts(path):
    """Yield packed PEX entries with their exact decoding metadata (BSA 104/105)."""
    with path.open("rb") as f:
        def read(n):
            data = f.read(n)
            if len(data) != n:
                raise ValueError("truncated BSA")
            return data
        size = path.stat().st_size
        magic, version, offset, flags, folders, files, _, names_len, _ = struct.unpack("<4s8I", read(36))
        if magic != b"BSA\0" or version not in (104, 105) or flags & 3 != 3:
            raise ValueError("unsupported BSA version or missing name tables")
        if not 36 <= offset <= size or folders > files or files > 2_000_000 or names_len > size:
            raise ValueError("invalid BSA counts")
        f.seek(offset)
        fmt = "<QIIQ" if version == 105 else "<QII"
        counts = [struct.unpack(fmt, read(struct.calcsize(fmt)))[1] for _ in range(folders)]
        if sum(counts) != files:
            raise ValueError("BSA folder/file count mismatch")
        items = []
        for count in counts:
            raw = read(read(1)[0])
            if not raw.endswith(b"\0"):
                raise ValueError("invalid BSA folder name")
            folder = raw[:-1].decode("utf-8", "surrogateescape").replace("\\", "/").casefold()
            for _ in range(count):
                _, length, start = struct.unpack("<QII", read(16))
                items.append((folder, length, start))
        names = read(names_len).split(b"\0")
        if not names or names[-1] or len(names) != files + 1:
            raise ValueError("invalid BSA filename table")
        for (folder, length, start), raw in zip(items, names):
            name = raw.decode("utf-8", "surrogateescape").casefold()
            if not (folder == "scripts" or folder.startswith("scripts/")) or not name.endswith(".pex"):
                continue
            if length & 0x80000000:
                raise ValueError("unsupported BSA file size flag")
            packed = length & 0x3fffffff
            if packed > 32 * 1024 * 1024 or start + packed > size:
                raise ValueError("invalid BSA PEX extent")
            f.seek(start)
            yield {"path": folder + "/" + name, "version": version,
                   "compressed": bool(flags & 4) ^ bool(length & 0x40000000),
                   "embedded_name": bool(flags & 0x100)}, read(packed)


def export_quest_evidence(data, loadorder, plugins, output):
    import base64
    import hashlib
    import zlib
    from zipfile import ZIP_DEFLATED

    if not data.is_dir():
        raise ValueError("--quest-evidence requires the effective Data directory, visible through MO2")
    if output.exists():
        raise FileExistsError(output)
    if output.resolve().is_relative_to(data.resolve()):
        raise ValueError("write the evidence outside Data")
    order = effective_order(loadorder, plugins)
    visible = {}
    for p in data.iterdir():
        if p.is_file():
            key = p.name.casefold()
            if key in visible:
                raise ValueError(f"ambiguous effective file: {key}")
            visible[key] = p
    winners, plugin_rows = {}, []
    for plugin in order:
        blob = visible[plugin].read_bytes()
        masters = master_names(blob, next(records(blob, 0, len(blob)))) + [plugin]
        if any(m not in order[:order.index(plugin)] for m in masters[:-1]):
            raise ValueError(f"{plugin}: missing or out-of-order master")
        plugin_rows.append({"name": plugin, "masters": masters, "sha256": hashlib.sha256(blob).hexdigest()})
        for tag, flags, form, start, end, groups in records(blob, 0, len(blob), ()):
            if tag not in QUEST_TYPES or not form:
                continue
            index = form >> 24
            if index >= len(masters):
                raise ValueError(f"{plugin}: unresolved form {form:08X}")
            key = (masters[index], form & 0xffffff)
            if flags & 0x20:
                winners.pop(key, None)
                continue
            raw = blob[start:end]
            if flags & 0x40000:
                unpacked = zlib.decompress(raw[4:])
                if len(unpacked) != struct.unpack_from("<I", raw)[0]:
                    raise ValueError(f"{plugin}: invalid compressed record")
                raw = unpacked
            winners[key] = {"type": tag.decode("ascii"), "owner": key[0], "local": key[1],
                            "winner": plugin, "flags": flags, "groups": groups,
                            "payload_b64": base64.b64encode(raw).decode("ascii")}
    manifest = {"format": "stm-quest-evidence-v1", "plugins": plugin_rows,
                "records": len(winners), "scripts": [], "archive_errors": [],
                "note": "BSA entries are candidates, not claimed effective winners. Loose files are MO2-visible."}
    # Exclusive creation; partial archives are deleted on failure, never an old export.
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        try:
            with ZipFile(stream, "w", compression=ZIP_DEFLATED, compresslevel=3) as z:
                with z.open("records.jsonl", "w") as rows:
                    for key in sorted(winners):
                        rows.write((json.dumps(winners[key], separators=(",", ":")) + "\n").encode("utf-8"))
                def script(meta, raw):
                    member = f"scripts/{len(manifest['scripts']):07d}.bin"
                    z.writestr(member, raw)
                    manifest["scripts"].append(dict(meta, member=member, sha256=hashlib.sha256(raw).hexdigest()))
                loose = next((p for p in data.iterdir() if p.is_dir() and p.name.casefold() == "scripts"), None)
                if loose:
                    for p in sorted(loose.rglob("*")):
                        if p.is_file() and p.suffix.casefold() == ".pex":
                            script({"path": "scripts/" + p.relative_to(loose).as_posix().casefold(), "source": "loose"}, p.read_bytes())
                for name, archive in sorted(visible.items()):
                    if not name.endswith(".bsa"):
                        continue
                    try:
                        for meta, raw in bsa_scripts(archive):
                            script(dict(meta, source=name), raw)
                    except (ValueError, OSError, struct.error) as error:
                        manifest["archive_errors"].append({"archive": name, "error": str(error)})
                for name in ("archives.txt", "Skyrim.ini"):
                    p = loadorder.parent / name
                    if p.is_file():
                        if name == "Skyrim.ini":
                            import configparser
                            cfg = configparser.ConfigParser(interpolation=None, strict=False)
                            cfg.read(p, encoding="utf-8-sig")
                            manifest["ini_archives"] = dict(cfg["Archive"]) if "Archive" in cfg else {}
                        else:
                            manifest["mo2_archives"] = entries(p)
                z.writestr("manifest.json", json.dumps(manifest, ensure_ascii=True, indent=2))
        except BaseException:
            stream.close()
            output.unlink(missing_ok=True)
            raise
    return {"records": len(winners), "script_candidates": len(manifest["scripts"]), "archive_errors": manifest["archive_errors"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("archive", "loadorder", "plugins"):
        parser.add_argument(arg, type=Path)
    parser.add_argument("--quest-evidence", metavar="OUTPUT_ZIP", type=Path,
                        help="read MO2-visible Data instead of a ZIP; export raw winning quest records and PEX candidates")
    a = parser.parse_args()
    result = (export_quest_evidence(a.archive, a.loadorder, a.plugins, a.quest_evidence)
              if a.quest_evidence else count_profile(a.archive, a.loadorder, a.plugins))
    print(json.dumps(result, indent=2, sort_keys=True))
