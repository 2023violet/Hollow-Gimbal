"""Verify the SDK archive, isolated Host consumers, and release approval."""

import argparse
from datetime import date
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
from zipfile import ZIP_STORED, BadZipFile, ZipFile

from build_sdk_package import (
    HEX_SHA256, INTERNAL, LOCAL_PATH, MANIFEST_NAME, MAP, METADATA_NAME, SDK_DIR,
    json_bytes, load_map, require, safe_parts, sha256,
)

PATCH_NAME = "SDK-SOURCE-CHANGES.patch"
GIT_ID = re.compile(r"(?:[a-f0-9]{40}|[a-f0-9]{64})\Z")


def read_sdk(archive, source_map, destination):
    spec = load_map(source_map)
    prefix = f"falconfoc-servo-sdk-{spec['version']}"
    require(archive.name == f"{prefix}.zip", "Archive name does not match SDK version")
    digest = sha256(archive.read_bytes())
    checksum = archive.with_name(archive.name + ".sha256").read_text(encoding="utf8")
    require(checksum == f"{digest}  {archive.name}\n", "Archive SHA-256 mismatch")

    policy = {row["path"]: row for row in spec["files"]}
    expected = {*policy, MANIFEST_NAME, METADATA_NAME}
    with ZipFile(archive) as bundle:
        members = {}
        for member in bundle.infolist():
            parts = safe_parts(member.filename)
            require(parts[0] == prefix and len(parts) >= 2, f"Invalid archive root: {member.filename}")
            path = "/".join(parts[1:])
            require(path in expected and path not in members, f"Unlisted or duplicate file: {path}")
            require(member.create_system == 3 and member.external_attr >> 16 == (stat.S_IFREG | 0o644)
                    and member.compress_type == ZIP_STORED and not member.flag_bits & 1
                    and member.date_time == (1980, 1, 1, 0, 0, 0),
                    f"Invalid archive metadata for {path}")
            members[path] = member
        require(members.keys() == expected, "Files are missing from the archive")
        require(list(members) == sorted(members), "Archive member order drift")

        manifest_bytes = bundle.read(members[MANIFEST_NAME])
        meta_bytes = bundle.read(members[METADATA_NAME])
        meta = json.loads(meta_bytes)
        require(meta["schema"] == 1 and meta["sdk_version"] == spec["version"]
                and meta["classification"] == INTERNAL and meta["c_standard"] == "C11"
                and bool(GIT_ID.fullmatch(meta["source_git_commit"])),
                "SDK metadata identity mismatch")
        date.fromisoformat(meta["source_git_date"])
        date.fromisoformat(meta["snapshot_date"])
        require(type(meta["dirty_working_tree"]) is bool
                and type(meta["package_differs_from_head"]) is bool
                and meta["source_basis"] == ("working_tree_snapshot" if meta["dirty_working_tree"]
                                             else "commit_checkout"), "Invalid Git snapshot metadata")
        require(meta["source_map_sha256"] == sha256(source_map)
                and meta["manifest_sha256"] == sha256(manifest_bytes),
                "Source map or manifest hash mismatch")
        patch = archive.with_name(PATCH_NAME).read_bytes()
        require(meta["source_changes_sha256"] == sha256(patch)
                and meta["package_differs_from_head"] == bool(patch), "Source delta mismatch")
        require(meta["dirty_working_tree"] or not patch, "Clean-source claim disagrees with delta")
        require(archive.with_name(MANIFEST_NAME).read_bytes() == manifest_bytes
                and archive.with_name(METADATA_NAME).read_bytes() == meta_bytes,
                "Manifest or metadata sidecar differs from the archive")

        manifest = json.loads(manifest_bytes)
        require(manifest["schema"] == 1 and manifest["excluded"] == spec["excluded"],
                "Manifest policy mismatch")
        require([entry["path"] for entry in manifest["files"]] == sorted(policy),
                "Manifest file list mismatch")
        verified = {}
        for entry in manifest["files"]:
            name = entry["path"]
            expected_row = policy[name]
            require(all(entry[key] == value for key, value in expected_row.items())
                    and entry["public_api"] == (entry["kind"] == "public_header")
                    and entry["included"] is True, f"Manifest row policy mismatch: {name}")
            require(entry["source_state"] in {"tracked", "modified", "added", "untracked"},
                    f"Unknown source state: {name}")
            in_head = entry["source_state"] in {"tracked", "modified"}
            require((bool(GIT_ID.fullmatch(entry["git_first_add"]))
                     and bool(HEX_SHA256.fullmatch(entry["git_blob_sha256"]))) if in_head else
                    (entry["git_first_add"] is None and entry["git_blob_sha256"] is None),
                    f"Missing source identity: {name}")
            if entry["source_state"] == "tracked":
                require(entry["sha256"] == entry["git_blob_sha256"],
                        f"Unchanged source disagrees with Git blob: {name}")
            else:
                require(meta["dirty_working_tree"] and meta["package_differs_from_head"],
                        f"Clean snapshot has a changed file: {name}")
            data = bundle.read(members[name])
            require(len(data) == entry["bytes"] and sha256(data) == entry["sha256"],
                    f"Payload hash mismatch: {name}")
            require(not LOCAL_PATH.search(data), f"Local dependency path in {name}")
            verified[name] = data

    first_line = verified["README.md"].decode("utf8").splitlines()[0]
    require(first_line == f"# FalconFOC Servo SDK {spec['version']}", "README version mismatch")
    sdk = destination / prefix
    for name, data in {**verified, MANIFEST_NAME: manifest_bytes, METADATA_NAME: meta_bytes}.items():
        path = sdk.joinpath(*safe_parts(name))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return sdk, digest, meta, manifest


def run(log, env, command):
    print(f"$ {json.dumps([str(part) for part in command])}", file=log, flush=True)
    start = time.monotonic()
    try:
        status = subprocess.run(command, env=env, cwd=Path(log.name).parent,
                                stdout=log, stderr=subprocess.STDOUT, check=False).returncode
    except OSError as error:
        raise ValueError(f"Could not start {command[0]}; see {log.name}: {error}") from error
    elapsed = round(time.monotonic() - start, 3)
    print(f"exit={status} seconds={elapsed:.3f}", file=log, flush=True)
    require(status == 0, f"Command failed ({status}); see {log.name}")
    return elapsed


def verify_lane(sdk, consumer_source, work, lane, compiler, sanitizer=False):
    env = os.environ.copy()
    for key in ("CC", "CFLAGS", "CPPFLAGS", "LDFLAGS", "CPATH", "C_INCLUDE_PATH",
                "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "CMAKE_PREFIX_PATH",
                "CMAKE_TOOLCHAIN_FILE", "CMAKE_PROJECT_TOP_LEVEL_INCLUDES"):
        env.pop(key, None)
    header_names = {header.name for header in sdk.rglob("*.h")}
    if sanitizer:
        resource = subprocess.check_output([compiler, "-print-resource-dir"], text=True).strip()
        if os.name == "nt":
            env["PATH"] = str(Path(resource) / "lib/windows") + os.pathsep + env["PATH"]
        env["ASAN_OPTIONS"] = "halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    base_flags = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_C_COMPILER={compiler}",
                  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    if sanitizer:
        base_flags.extend(["-DCMAKE_C_FLAGS=-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer",
                           "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"])

    builds = [
        ("sdk", sdk, "falconfoc-servo-demo", ["-DFALCONFOC_SDK_BUILD_TESTS=ON"]),
        ("core", consumer_source / "core", "external-falconfoc-core",
         [f"-DFALCONFOC_SOURCE_DIR={sdk}"]),
        ("servo", consumer_source / "servo", "external-servo-consumer",
         [f"-DFALCONFOC_SOURCE_DIR={sdk}"]),
    ]
    timings = {}
    suffix = ".exe" if os.name == "nt" else ""
    with (work / f"{lane}.log").open("w", encoding="utf8") as log:
        run(log, env, [compiler, "--version"])
        for project, source, target, flags in builds:
            out = work / f"{lane} {project} build"
            configure = run(log, env, ["cmake", "-S", source, "-B", out, *base_flags, *flags])
            build = run(log, env, ["cmake", "--build", out, "--parallel", "4", "--verbose"])
            units = json.loads((out / "compile_commands.json").read_text(encoding="utf8"))
            require(units and all(Path(unit["file"]).resolve().is_relative_to(sdk)
                                  or Path(unit["file"]).resolve().is_relative_to(source)
                                  for unit in units), f"{lane} {project} used source outside clean extract")
            deps = subprocess.check_output(["ninja", "-C", out, "-t", "deps"],
                                           env=env, text=True, errors="replace")
            sdk_deps = [Path(line.strip()) for line in deps.splitlines()
                        if line.startswith("    ") and Path(line.strip()).name in header_names]
            require(sdk_deps and all((out / header).resolve().is_relative_to(sdk) for header in sdk_deps),
                    f"{lane} {project} resolved FalconFOC headers outside clean extract")
            print(f"SDK compile closure: {len(units)} translation units, "
                  f"{len(set(sdk_deps))} unique FalconFOC header dependencies", file=log, flush=True)
            if project == "core":
                require(not any("/runtime/" in Path(unit["file"]).as_posix()
                                or "/simulation/" in Path(unit["file"]).as_posix()
                                for unit in units), "Core-only consumer compiled Host Runtime")
            tests = run(log, env, ["ctest", "--test-dir", out, "--no-tests=error",
                                    "--output-on-failure"])
            binary_dir = out / "examples/servo" if project == "sdk" else out
            executable = binary_dir / (target + suffix)
            direct = run(log, env, [executable])
            if project == "sdk":
                direct += run(log, env, [executable, "--faults"])
            timings[project] = {"configure_seconds": configure, "build_seconds": build,
                                "test_seconds": tests, "run_seconds": round(direct, 3),
                                "compiled_units": len(units)}
    return timings


def release_gates(meta, digest, manifest, review):
    # Evidence identifiers are explicit owner review records. This does not infer
    # permission from build logs, Git authorship, or source availability.
    bound_review = (review.get("archive_sha256") == digest
                    and review.get("source_git_commit") == meta["source_git_commit"]
                    and review.get("sdk_version") == meta["sdk_version"])
    origin_known = all(entry["origin"] in {"project_repository", "licensed_third_party"}
                       for entry in manifest["files"])
    rights_cleared = all(entry["redistribution"] == "authorized" for entry in manifest["files"])
    frozen = not meta["dirty_working_tree"] and not meta["package_differs_from_head"]

    def recorded(key):
        return bound_review and isinstance(review.get(key), str) and bool(review[key].strip())

    return {
        "PROVENANCE_GATE": "PASS" if origin_known and recorded("provenance_record") else "BLOCKED",
        "LICENSE_GATE": "PASS" if rights_cleared and recorded("license_notice_record") else "BLOCKED",
        "APPROVAL_GATE": "PASS" if frozen and recorded("owner_release_approval") else "BLOCKED",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--work", required=True, type=Path, help="New directory for clean extract, builds and logs")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--gcc", default="gcc")
    parser.add_argument("--release-review", type=Path, help="Owner review records bound to this exact archive")
    parser.add_argument("--technical-only", action="store_true",
                        help="Pass exit status when technical checks pass; report all gates")
    args = parser.parse_args()
    report = {"TECHNICAL_GATE": "FAIL", "PROVENANCE_GATE": "BLOCKED", "LICENSE_GATE": "BLOCKED",
              "APPROVAL_GATE": "BLOCKED", "release_status": "RELEASE_BLOCKED"}
    work = args.work.resolve()
    try:
        work.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(error, file=sys.stderr)
        return 1
    try:
        sdk, digest, meta, manifest = read_sdk(args.archive.resolve(), MAP.read_bytes(), work)
        review = json.loads(args.release_review.read_bytes()) if args.release_review else {}
        require(isinstance(review, dict), "Owner review must be a JSON object")
        report.update(release_gates(meta, digest, manifest, review))
        report.update({
            "archive_sha256": digest,
            "sdk_version": meta["sdk_version"],
            "source_git_commit": meta["source_git_commit"],
            "manifest_sha256": meta["manifest_sha256"],
            "payload_files": len(manifest["files"]),
            "public_headers": sum(entry["public_api"] for entry in manifest["files"]),
        })
        consumers = work / "external consumers"
        shutil.copytree(SDK_DIR / "consumers", consumers)
        report["lanes"] = {}
        for label, compiler, sanitizer in (("clang", args.clang, False), ("gcc", args.gcc, False),
                                           ("clang-asan-ubsan", args.clang, True)):
            report["lanes"][label] = verify_lane(sdk, consumers, work, label, compiler, sanitizer)
        report["TECHNICAL_GATE"] = "PASS"
        if all(report[key] == "PASS" for key in ("PROVENANCE_GATE", "LICENSE_GATE", "APPROVAL_GATE")):
            report["release_status"] = "RELEASE_READY"
    except (OSError, ValueError, KeyError, TypeError, BadZipFile, subprocess.CalledProcessError) as error:
        report["error"] = str(error)
    (work / "sdk-gate.json").write_bytes(json_bytes(report))
    print(json_bytes(report).decode(), end="")
    if report["TECHNICAL_GATE"] != "PASS":
        return 1
    return 0 if args.technical_only or report["release_status"] == "RELEASE_READY" else 2


if __name__ == "__main__":
    sys.exit(main())
