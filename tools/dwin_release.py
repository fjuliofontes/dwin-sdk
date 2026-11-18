#!/usr/bin/env python3
"""Build the .dwpk bundles of a display release: a full one and a delta one.

    full    every file of --files, minus --full-exclude (typically the font)
    delta   only the files that changed since the previous release tag, plus the file that
            carries the version. It is only valid on a display that runs exactly that
            previous release, which is why its name says so: hmi-2.4.5-from-2.4.4.dwpk

The version is the label of the bundle. With --version-vp it is also the text of a DGUS text
box, held in the file --version-file (22, the initial content of the variable memory, two
bytes per VP):

    --version given     the text box is stamped in the copy that goes into the bundles, the
                        project files are left untouched
    --version empty     the version is read from the text box, as set in DGUS Tool

"Changed" is decided with git: the files are compared with the ones of the highest release
tag below the version being built (or --base). This is the backend of the GitHub action in
action.yml, and it runs the same way on a desk:

    dwin_release.py --source DWIN_SET --files "0 13 14 22 23 24" --full-exclude 0 \\
        --version 2.4.5 --version-vp 0x1600 --version-prefix v. --output-dir dist
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PACKER = Path(__file__).resolve().parent / "dwin_pack.py"
LABEL_MAX = 15  # the bundle header keeps 16 bytes for the label, terminator included
# DGUS Tool leaves the page images next to the flashable files, under the same numbers.
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp"}


def fail(message):
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::error::{message}")
    sys.exit(f"error: {message}")


def warn(message):
    print(f"::warning::{message}" if os.environ.get("GITHUB_ACTIONS") else f"warning: {message}", file=sys.stderr)


def git(*args, cwd, check=True):
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        if check:
            fail(f"git {' '.join(args)}: {result.stderr.strip()}")
        return None
    return result.stdout.strip()


def strip_prefix(text, prefix):
    return text[len(prefix) :] if prefix and text.startswith(prefix) else text


def as_version(text, what):
    if re.fullmatch(r"\d+(\.\d+)*", text) is None:
        fail(f'{what} "{text}" is not dotted numeric, e.g. 1.4.0')
    if len(text) > LABEL_MAX:
        fail(f'{what} "{text}" is longer than {LABEL_MAX} characters')
    return text


def key(version):
    return tuple(int(part) for part in version.split("."))


def file_id(path):
    return int(re.match(r"\d+", path.name).group())


def resolve(source, token):
    """A DGUS file of the source directory, from its number ("22") or its name."""
    if not token.isdigit():
        path = source / token
        if not path.is_file():
            fail(f"{path} does not exist")
        if re.match(r"\d", path.name) is None:
            fail(f"{path.name} does not start with a DGUS file number")
        return path
    matches = [
        p
        for p in sorted(source.iterdir())
        if p.is_file() and p.suffix.lower() not in IMAGE_SUFFIXES and re.fullmatch(rf"0*{int(token)}(\D.*)?", p.name)
    ]
    if not matches:
        fail(f"no DGUS file number {token} in {source}")
    if len(matches) > 1:
        fail(f"file number {token} is ambiguous in {source} ({', '.join(p.name for p in matches)}), give the file name")
    return matches[0]


def read_version(path, vp, size, prefix):
    data = path.read_bytes()
    if len(data) < vp * 2 + size:
        fail(f"{path.name} is too short to hold VP 0x{vp:04X}")
    raw = data[vp * 2 : vp * 2 + size]
    text = re.split(rb"[\x00\xff]", raw, maxsplit=1)[0]  # a DGUS text ends at 0x00 or 0xFF
    try:
        text = text.decode("ascii").strip()
    except UnicodeDecodeError:
        fail(f"VP 0x{vp:04X} of {path.name} does not hold ASCII text: {raw.hex()}")
    return as_version(strip_prefix(text, prefix), f"the text at VP 0x{vp:04X} of {path.name}")


def stamp_version(path, vp, size, text):
    data = bytearray(path.read_bytes())
    if len(data) < vp * 2 + size:
        fail(f"{path.name} is too short to hold VP 0x{vp:04X}")
    if len(text) > size:
        fail(f'"{text}" does not fit in the {size} bytes of VP 0x{vp:04X}')
    data[vp * 2 : vp * 2 + size] = text.encode("ascii").ljust(size, b"\0")
    path.write_bytes(data)


def previous_release(repo, tag_prefix, version):
    """Highest release tag below the version, taken from the remote so a shallow clone works."""
    listing = git("ls-remote", "--tags", "--refs", "origin", cwd=repo, check=False)
    if listing is not None:
        tags = [line.split("refs/tags/", 1)[1] for line in listing.splitlines() if "refs/tags/" in line]
    else:
        tags = git("tag", "--list", cwd=repo).splitlines()
    older = []
    for tag in tags:
        label = strip_prefix(tag, tag_prefix)
        if tag.startswith(tag_prefix) and re.fullmatch(r"\d+(\.\d+)*", label) and key(label) < key(version):
            older.append((key(label), tag))
    return max(older)[1] if older else None


def changed_since(repo, tag, paths):
    """Files whose content differs from the one of the tag, new files included."""
    ref = f"refs/tags/{tag}"
    if git("rev-parse", "--quiet", "--verify", f"{ref}^{{commit}}", cwd=repo, check=False) is None:
        git("fetch", "--quiet", "--no-tags", "--depth=1", "origin", f"+{ref}:{ref}", cwd=repo)
    changed = []
    for path in paths:
        relative = path.resolve().relative_to(repo).as_posix()
        before = git("rev-parse", "--quiet", "--verify", f"{ref}:{relative}", cwd=repo, check=False)
        if before != git("hash-object", "--", relative, cwd=repo):
            changed.append(path)
    return changed


def pack(paths, version, last, output, args):
    command = [sys.executable, str(PACKER), "pack", *map(str, paths), "--version", version, "-o", str(output)]
    command += ["--flash-size", str(args.flash_size), "--max-files", str(args.max_files)]
    if last is not None:
        command += ["--last", str(last)]
    sys.stdout.flush()
    if subprocess.run(command).returncode != 0:
        sys.exit(1)
    print()


def main():
    parser = argparse.ArgumentParser(description="Build the full and the delta .dwpk bundle of a display release.")
    parser.add_argument("--source", default="DWIN_SET", help="directory with the DGUS files (default DWIN_SET)")
    parser.add_argument("--files", required=True, help='DGUS files of the display, by number or by name: "0 13 14 22 23 24"')
    parser.add_argument("--full-exclude", default="", help='files left out of the full bundle, typically the font: "0"')
    parser.add_argument("--version", default="", help="version of the release, a tag name is fine; empty reads it from --version-vp")
    parser.add_argument("--tag-prefix", default="v", help='prefix of the release tags (default "v")')
    parser.add_argument("--version-file", default="22", help="file that carries the version, written last (default 22)")
    parser.add_argument("--version-vp", default="", help="VP of the version text box, e.g. 0x1600; empty for none")
    parser.add_argument("--version-size", default="32", help="size of the version text box in bytes (default 32)")
    parser.add_argument("--version-prefix", default="", help='text shown before the version on the display, e.g. "v."')
    parser.add_argument("--delta", default="true", choices=("true", "false"), help="also build the delta bundle (default true)")
    parser.add_argument("--base", default="", help="release tag the delta is made against (default: the highest tag below the version)")
    parser.add_argument("--name", default="hmi", help='first part of the bundle file names (default "hmi")')
    parser.add_argument("--output-dir", default=".", help="where the bundles are written")
    parser.add_argument("--flash-size", default="16", help="display NOR flash size in MB, 8 or 16 (default 16)")
    parser.add_argument("--max-files", default="16", help="CONFIG_DWIN_OTA_BUNDLE_MAX_FILES of the firmware (default 16)")
    args = parser.parse_args()

    source = Path(args.source)
    if not source.is_dir():
        fail(f"{source} is not a directory")
    originals = [resolve(source, token) for token in args.files.replace(",", " ").split()]
    if not originals:
        fail("--files is empty")
    excluded = {resolve(source, token) for token in args.full_exclude.replace(",", " ").split()}
    if not excluded <= set(originals):
        fail("--full-exclude names a file that is not in --files")

    carrier = resolve(source, args.version_file) if args.version_file else None
    if carrier is not None and carrier not in originals:
        fail(f"the version file {carrier.name} is not in --files")
    if carrier in excluded:
        fail(f"the version file {carrier.name} cannot be left out of the full bundle")

    vp = int(args.version_vp, 0) if args.version_vp else None
    size = int(args.version_size, 0)
    if vp is not None and carrier is None:
        fail("--version-vp needs --version-file")

    requested = strip_prefix(args.version.strip(), args.tag_prefix)
    if requested:
        version = as_version(requested, "version")
    elif vp is not None:
        version = read_version(carrier, vp, size, args.version_prefix)
    else:
        fail("give --version, or --version-vp to read it from the display files")

    with tempfile.TemporaryDirectory() as staging:
        staged = {}
        for path in originals:
            staged[path] = Path(shutil.copy2(path, staging))
        if requested and vp is not None:
            stamp_version(staged[carrier], vp, size, args.version_prefix + version)
            print(f'{carrier.name}: VP 0x{vp:04X} = "{args.version_prefix + version}"')

        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        last = file_id(carrier) if carrier is not None else None

        full = output_dir / f"{args.name}-{version}.dwpk"
        pack([staged[p] for p in originals if p not in excluded], version, last, full, args)

        delta = base = None
        changed = []
        if args.delta == "true":
            top = git("rev-parse", "--show-toplevel", cwd=source, check=False)
            if top is None:
                warn(f"{source} is not in a git repository, no delta bundle")
            else:
                repo = Path(top).resolve()
                base = args.base.strip() or previous_release(repo, args.tag_prefix, version)
                if base is None:
                    print(f'no release tag "{args.tag_prefix}*" below {version}, no delta bundle')
                else:
                    changed = changed_since(repo, base, originals)
                    left_out = sorted(p.name for p in changed if p in excluded)
                    if left_out:
                        warn(
                            f"{', '.join(left_out)} changed since {base} but is left out of the full bundle: "
                            f"only the delta bundle carries it"
                        )
                    content = [p for p in originals if p in changed or p == carrier]
                    base_version = strip_prefix(base, args.tag_prefix)
                    delta = output_dir / f"{args.name}-{version}-from-{base_version}.dwpk"
                    pack([staged[p] for p in content], version, last, delta, args)

    outputs = {
        "version": version,
        "full": full.as_posix(),
        "delta": delta.as_posix() if delta else "",
        "base": base or "",
        "changed": " ".join(str(file_id(p)) for p in changed),
    }
    for name, value in outputs.items():
        print(f"{name}={value}")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as handle:
            handle.writelines(f"{name}={value}\n" for name, value in outputs.items())


if __name__ == "__main__":
    main()
