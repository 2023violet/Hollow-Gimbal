"""Build a deterministic internal FalconFOC source candidate from an explicit map."""

import argparse
from datetime import date
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
from zipfile import ZIP_STORED, ZipFile, ZipInfo

SDK_DIR = Path(__file__).resolve().parent
MAP = SDK_DIR / "source-map.json"
MANIFEST_NAME = "SDK-SOURCE-MANIFEST.json"
METADATA_NAME = "SDK-METADATA.json"
INTERNAL = "INTERNAL EVALUATION CANDIDATE"
HEX_SHA256 = re.compile(r"[a-f0-9]{64}\Z")
LOCAL_PATH = re.compile(
    rb"(?i)(?:\b[A-Z]:[\\/]|\\\\[^\\\s]+\\|/(?:home|mnt/[a-z]|Users|private/tmp)/)"
)
ORIGIN = {"project_repository", "origin_unresolved", "licensed_third_party", "vendor_restricted"}
RIGHTS = {"owner_pending", "terms_pending", "authorized"}
EXTENSIONS = {
    "public_header": {".h"}, "private_header": {".h"}, "source": {".c"},
    "test": {".c"}, "example": {".c"}, "build": {".txt", ".cmake"},
    "documentation": {".md"},
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode()


def safe_parts(name):
    parts = name.split("/")
    require(
        all(re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9_.-]*", part)
            and not part.endswith(".")
            and part.split(".")[0].upper() not in {
                "AUX", "CON", "NUL", "PRN", *(f"COM{i}" for i in range(1, 10)),
                *(f"LPT{i}" for i in range(1, 10)),
            } for part in parts),
        f"Unsafe path: {name}",
    )
    require(PurePosixPath(name).as_posix() == name, f"Noncanonical path: {name}")
    return parts


def read_mapped_file(repo, name):
    path = repo
    for part in safe_parts(name):
        path = path / part
        status = path.lstat()
        require(
            not (stat.S_ISLNK(status.st_mode)
                 or getattr(status, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT),
            f"Link or reparse point: {name}",
        )
    require(stat.S_ISREG(status.st_mode), f"Regular source file required: {name}")
    require(path.resolve(strict=True).is_relative_to(repo), f"Outside the source root: {name}")
    return path.read_bytes()


def load_map(data):
    spec = json.loads(data)
    require(spec["schema"] == 1, "Unsupported source map schema")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-rc\.[1-9][0-9]*", spec["version"]),
            "Expected a candidate SDK version")
    sources = set()
    targets = {MANIFEST_NAME.casefold(), METADATA_NAME.casefold()}
    for entry in spec["files"]:
        safe_parts(entry["source"])
        safe_parts(entry["path"])
        require(entry["source"].casefold() not in sources, "Duplicate source path in source map")
        require(entry["path"].casefold() not in targets, "Duplicate package path in source map")
        require(entry["origin"] in ORIGIN and entry["redistribution"] in RIGHTS,
                f"Unknown rights classification: {entry['path']}")
        extensions = EXTENSIONS.get(entry["kind"], set())
        require(all(PurePosixPath(entry[key]).suffix in extensions for key in ("source", "path")),
                f"File kind or extension is outside SDK scope: {entry['path']}")
        require(all(part.casefold() not in {"build", "cmakefiles", "__pycache__"}
                    for key in ("path", "source") for part in entry[key].split("/")),
                f"Generated tree in source map: {entry['path']}")
        sources.add(entry["source"].casefold())
        targets.add(entry["path"].casefold())
    require(sources, "Source map is empty")
    return spec


def git(repo, *args, accepted=(0,)):
    result = subprocess.run(
        ["git", "-C", str(repo), "-c", "core.quotePath=false", *args],
        capture_output=True, check=False,
    )
    require(result.returncode in accepted, result.stderr.decode(errors="replace"))
    return result.stdout


def make_archive(repo, output, snapshot_date):
    spec_bytes = read_mapped_file(repo, "scripts/sdk/source-map.json")
    spec = load_map(spec_bytes)
    head = git(repo, "rev-parse", "HEAD").decode().strip()
    dirty = git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    date.fromisoformat(snapshot_date)
    head_files = set(git(repo, "ls-tree", "-r", "--name-only", "-z", head).decode().split("\0"))
    entries = []
    payload = {}
    tracked = []
    additions = []
    checkout_hashes = {}
    for policy in sorted(spec["files"], key=lambda row: row["path"]):
        source = policy["source"]
        data = read_mapped_file(repo, source)
        checkout_hashes[source] = sha256(data)
        entry = {
            **policy,
            "public_api": policy["kind"] == "public_header",
            "included": True,
        }
        if source in head_files:
            blob = git(repo, "show", f"{head}:{source}")
            entry["git_blob_sha256"] = sha256(blob)
            entry["git_first_add"] = git(
                repo, "log", "--diff-filter=A", "-1", "--format=%H", head, "--", source
            ).decode().strip()
            modified = git(repo, "diff", "--name-only", head, "--", source)
            entry["source_state"] = "modified" if modified else "tracked"
            # Use canonical Git bytes for unchanged files, including on CRLF checkouts.
            if not modified:
                data = blob
            tracked.append(source)
        else:
            entry["git_blob_sha256"] = None
            entry["git_first_add"] = None
            in_index = bool(git(repo, "ls-files", "--", source))
            require(in_index or bool(git(repo, "ls-files", "--others", "--exclude-standard", "--", source)),
                    f"Mapped source is ignored or absent from Git status: {source}")
            entry["source_state"] = "added" if in_index else "untracked"
            additions.append(source)
        require(not LOCAL_PATH.search(data), f"Local path in packaged file: {source}")
        entry.update(sha256=sha256(data), bytes=len(data))
        entries.append(entry)
        payload[entry["path"]] = data

    patch = git(repo, "diff", "--binary", "--no-ext-diff", "--no-color", head, "--", *tracked)
    for source in additions:
        patch += git(repo, "diff", "--no-index", "--binary", "--no-color", "--",
                     os.devnull, source, accepted=(0, 1))

    require(git(repo, "rev-parse", "HEAD").decode().strip() == head, "HEAD changed during packaging")
    require(git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all") == dirty,
            "Git status changed during packaging")
    for entry in entries:
        require(sha256(read_mapped_file(repo, entry["source"])) == checkout_hashes[entry["source"]],
                f"Source changed during packaging: {entry['source']}")
    require(read_mapped_file(repo, "scripts/sdk/source-map.json") == spec_bytes,
            "Source map changed during packaging")

    manifest = json_bytes({"schema": 1, "files": entries, "excluded": spec["excluded"]})
    metadata = json_bytes({
        "schema": 1,
        "sdk_version": spec["version"],
        "classification": INTERNAL,
        "source_git_commit": head,
        "source_git_date": git(repo, "show", "-s", "--format=%cs", head).decode().strip(),
        "snapshot_date": snapshot_date,
        "dirty_working_tree": bool(dirty),
        "source_basis": "working_tree_snapshot" if dirty else "commit_checkout",
        "c_standard": "C11",
        "toolchains": ["Clang", "GCC / MinGW GCC"],
        "api_compatibility": "Pre-release public C headers and CMake targets; no stable ABI promise.",
        "manifest_sha256": sha256(manifest),
        "source_map_sha256": sha256(spec_bytes),
        "source_changes_sha256": sha256(patch),
        "package_differs_from_head": bool(patch),
    })
    payload.update({MANIFEST_NAME: manifest, METADATA_NAME: metadata})
    name = f"falconfoc-servo-sdk-{spec['version']}"
    output.mkdir(parents=True, exist_ok=False)
    archive = output / f"{name}.zip"
    with ZipFile(archive, "x", compression=ZIP_STORED) as bundle:
        for path, data in sorted(payload.items()):
            member = ZipInfo(f"{name}/{path}", date_time=(1980, 1, 1, 0, 0, 0))
            member.create_system = 3
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            bundle.writestr(member, data)
    archive_hash = sha256(archive.read_bytes())
    (output / (archive.name + ".sha256")).write_bytes(f"{archive_hash}  {archive.name}\n".encode())
    (output / MANIFEST_NAME).write_bytes(manifest)
    (output / METADATA_NAME).write_bytes(metadata)
    (output / "SDK-SOURCE-CHANGES.patch").write_bytes(patch)
    print(f"{INTERNAL}\n{archive}\narchive_sha256={archive_hash}\nmanifest_sha256={sha256(manifest)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New directory for archive and sidecars")
    parser.add_argument("--snapshot-date", required=True, help="Source snapshot date, YYYY-MM-DD")
    args = parser.parse_args()
    try:
        make_archive(SDK_DIR.parents[1], args.output, args.snapshot_date)
    except (OSError, ValueError, KeyError) as error:
        print(f"SDK packaging failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
