#!/usr/bin/env bash
# Build for private local testing only. No artifact upload or installation into MO2.
set -euo pipefail
die() { printf '%s\n' "$*" >&2; exit 1; }
[[ $# == 1 ]] || die "Usage: bash required/tools/build-local.sh C:/stm-build/new-directory"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] || die "Run in Git Bash on Windows."
for tool in git python xmake node pnpm 7z; do
  command -v "$tool" >/dev/null || die "Missing in PATH: $tool"
done
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || die "Commit or stash local edits before building."
revision="$(git rev-parse HEAD)"
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || die "Cannot resolve repository revision."
xmake --version | grep -Eq 'v3\.1\.0([ ,]|$)' || die "xmake 3.1.0 required."
# Never overwrite an existing workspace or user's installed game.
[[ ! -e "$1" ]] || die "Workspace must not exist: $1"
mkdir -p "$1"
work="$(cd "$1" && pwd)"
case "$work/" in "$root/"*) die "Workspace must be outside the STM checkout.";; esac
# Derive the upstream pin and ordered patch list from the authoritative workflow.
# Parse only expected literal commands, never execute YAML or arbitrary shell code.
python - "$root/.github/workflows/build.yml" "$root" "$work" <<'PY'
import pathlib, re, sys
workflow, root, work = map(pathlib.Path, sys.argv[1:])
source = workflow.read_text(encoding='utf-8')
build = source.split('\n  build:\n', 1)[1].split('\n  packaged-ui:', 1)[0]
pins = re.findall(r'^          repository: tiltedphoques/TiltedEvolution\s*\n          ref: ([0-9a-f]{40})$', build, re.M)
if len(pins) != 1:
    raise SystemExit('Cannot establish pinned upstream.')
block = build.split('      - name: Apply co-op and overlay changes\n        run: |\n', 1)[1].split('\n      - name:', 1)[0]
rows = []
for line in block.splitlines():
    if not line.strip():
        continue
    match = re.fullmatch(r'          git (?:-C (Libraries/TiltedConnect|Libraries/TiltedUI) )?apply (\.\./(?:\.\./\.\./)?)([a-z]+/patches/[A-Za-z0-9_]+\.patch)', line)
    if not match:
        raise SystemExit('Unexpected apply command: '+line)
    sub, relative, patch = match.groups()
    if relative != ('../../../' if sub else '../') or not (root/patch).is_file():
        raise SystemExit('Invalid patch path: '+line)
    rows.append((sub or '.', patch))
check = re.findall(r'^      - run: git -C upstream apply \.\./([a-z]+/patches/[A-Za-z0-9_]+\.patch)$', source, re.M)
if check != [p for sub,p in rows if sub == '.'] or len(rows) == 0:
    raise SystemExit('Check/build patch order mismatch.')
(work/'upstream-pin.txt').write_text(pins[0]+'\n', encoding='ascii', newline='\n')
(work/'patch-order.tsv').write_text(''.join(sub+'\t'+p+'\n' for sub,p in rows), encoding='ascii', newline='\n')
print(f'Validated {len(check)} main patches and {len(rows)-len(check)} dependency patches.')
PY
pin="$(cat "$work/upstream-pin.txt")"
git clone --no-checkout https://github.com/tiltedphoques/TiltedEvolution.git "$work/upstream"
git -C "$work/upstream" checkout --detach "$pin"
git -C "$work/upstream" submodule update --init --recursive
[[ "$(git -C "$work/upstream" rev-parse HEAD)" == "$pin" ]] || die "Upstream revision mismatch."
while IFS=$'\t' read -r sub path; do
  printf 'Applying %s\n' "$path"
  git -C "$work/upstream/$sub" apply --check "$root/$path"
  git -C "$work/upstream/$sub" apply "$root/$path"
done < "$work/patch-order.tsv"
cd "$work/upstream"
required_pnpm="$(python -c 'import json; print(json.load(open("Code/skyrim_ui/package.json"))["packageManager"].split("@",1)[1].split("+",1)[0])')"
[[ "$(pnpm --version)" == "$required_pnpm" ]] || die "Install pnpm $required_pnpm (from pinned UI package.json)."
xmake config --plat=windows --arch=x64 --mode=release --skyrim_version=1.6.1170 --yes
xmake -y
xmake run TPTests -d yes -s
xmake install -o distrib
cp -R distrib/bin/. build/windows/x64/release/
build/windows/x64/release/SkyrimTogetherServer.exe --generate-config "$work/ci-STServer.ini"
test -s "$work/ci-STServer.ini"
(cd Code/skyrim_ui && pnpm install --frozen-lockfile && pnpm deploy:production)
mo2="$work/mo2"
mkdir -p "$mo2/SkyrimTogetherReborn"
cp -R GameFiles/Skyrim/. "$mo2/"
python "$root/required/tools/build_trade_plugin.py" "$mo2/SkyrimCoopTrade.esp"
cp "$root/GPL-3.0.txt" "$root/TiltedEvolution-LICENSE.txt" "$mo2/"
cp -R build/windows/x64/release/. "$mo2/SkyrimTogetherReborn/"
cp -R Code/skyrim_ui/dist/UI "$mo2/SkyrimTogetherReborn/"
cd "$mo2/SkyrimTogetherReborn"
rm -f -- *.pdb *.lib *.exp *Tests.exe
for file in SkyrimTogether.exe STServer.dll SkyrimTogetherServer.exe TPProcess.exe UI/index.html ../SkyrimTogether.esp ../SkyrimCoopTrade.esp; do
  test -s "$file" || die "Missing runtime file: $file"
done
cp SkyrimTogether.exe SkyrimSE.exe
# Reject a changed source checkout before packaging.
[[ "$(git -C "$root" rev-parse HEAD)" == "$revision" && -z "$(git -C "$root" status --porcelain --untracked-files=normal)" ]] || die "STM source changed during build."
archive="$work/Skyrim-Coop-SE-1170-${revision:0:12}-MO2.zip"
cd "$mo2"
7z a -tzip "$archive" .
7z t "$archive"
python - "$archive" "$revision" "$pin" <<'PY'
import hashlib, pathlib, sys
archive=pathlib.Path(sys.argv[1])
with archive.open('rb') as f:
    sha=hashlib.file_digest(f,'sha256').hexdigest()
manifest=f'STM commit: {sys.argv[2]}\nUpstream: {sys.argv[3]}\nSHA256: {sha}\nArchive: {archive.name}\n'
archive.with_suffix('.manifest.txt').write_text(manifest, encoding='utf-8')
print(manifest)
print(archive)
PY
