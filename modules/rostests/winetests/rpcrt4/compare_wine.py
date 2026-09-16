#!/usr/bin/env python3

"""Compare the ReactOS RPCRT4 Wine tests with a Wine source checkout.

This is an inventory tool, not a compatibility oracle. A source difference must
still be classified using Windows documentation and native Windows behaviour
before changing ReactOS semantics.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


REACTOS_LOCAL_FILES = {
    "BASELINE.md",
    "CMakeLists.txt",
    "compare_wine.py",
    "testlist.c",
    "warningfix.diff",
}

WINE_LOCAL_FILES = {
    "Makefile.in",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_output(path: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(path), *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip()


def git_revision(path: Path) -> tuple[str, bool | None]:
    revision = git_output(path, "rev-parse", "HEAD")
    if revision is None:
        return "unknown", None

    status = git_output(path, "status", "--porcelain", "--untracked-files=no")
    return revision, bool(status) if status is not None else None


def resolve_reactos_tests(path: Path) -> Path:
    candidate = path / "modules" / "rostests" / "winetests" / "rpcrt4"
    if candidate.is_dir():
        return candidate
    if (path / "CMakeLists.txt").is_file() and (path / "ndr_marshall.c").is_file():
        return path
    raise ValueError(f"{path} is neither a ReactOS checkout nor the RPCRT4 test directory")


def resolve_wine_tests(path: Path) -> Path:
    candidate = path / "dlls" / "rpcrt4" / "tests"
    if candidate.is_dir():
        return candidate
    if (path / "Makefile.in").is_file() and (path / "ndr_marshall.c").is_file():
        return path
    raise ValueError(f"{path} is neither a Wine checkout nor the RPCRT4 test directory")


def source_files(root: Path, excluded: set[str]) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative in excluded:
            continue
        files[relative] = path
    return files


def short_digest(path: Path | None) -> str:
    return "-" if path is None else sha256(path)[:12]


def compare(reactos_dir: Path, wine_dir: Path) -> int:
    reactos_files = source_files(reactos_dir, REACTOS_LOCAL_FILES)
    wine_files = source_files(wine_dir, WINE_LOCAL_FILES)

    reactos_revision, reactos_dirty = git_revision(reactos_dir)
    wine_revision, wine_dirty = git_revision(wine_dir)

    print(f"ReactOS tests: {reactos_dir}")
    print(f"ReactOS revision: {reactos_revision}" + (" (dirty)" if reactos_dirty else ""))
    print(f"Wine tests: {wine_dir}")
    print(f"Wine revision: {wine_revision}" + (" (dirty)" if wine_dirty else ""))
    print()
    print("classification\tpath\treactos_sha256\twine_sha256")

    differences = 0
    for relative in sorted(set(reactos_files) | set(wine_files)):
        reactos_path = reactos_files.get(relative)
        wine_path = wine_files.get(relative)

        if reactos_path is None:
            classification = "MISSING_FROM_REACTOS"
            differences += 1
        elif wine_path is None:
            classification = "REACTOS_ONLY"
            differences += 1
        elif sha256(reactos_path) == sha256(wine_path):
            classification = "IDENTICAL"
        else:
            classification = "DIFFERS"
            differences += 1

        print(
            f"{classification}\t{relative}\t"
            f"{short_digest(reactos_path)}\t{short_digest(wine_path)}"
        )

    print()
    print(
        "Source differences are inventory results only. Do not infer Windows "
        "compatibility from Wine agreement or disagreement."
    )
    return differences


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inventory differences between ReactOS and Wine RPCRT4 tests."
    )
    parser.add_argument(
        "wine",
        type=Path,
        help="Wine checkout root or dlls/rpcrt4/tests directory",
    )
    parser.add_argument(
        "--reactos",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="ReactOS checkout root or RPCRT4 test directory (default: this directory)",
    )
    parser.add_argument(
        "--fail-on-difference",
        action="store_true",
        help="return exit status 1 when source differences are found",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        reactos_dir = resolve_reactos_tests(args.reactos.resolve())
        wine_dir = resolve_wine_tests(args.wine.resolve())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    differences = compare(reactos_dir, wine_dir)
    return 1 if args.fail_on_difference and differences else 0


if __name__ == "__main__":
    raise SystemExit(main())
