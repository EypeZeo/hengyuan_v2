"""Deployment bundle identity and verification.

A release is a flat directory: the ``hy_recorder`` package next to the vendored wheels' contents, plus a
``MANIFEST.json``. Its identity is the SHA-256 over the sorted ``(relative path, file sha256)`` pairs of
every file except the manifest itself, so it depends on content only (not on tar metadata or on when it
was built) and the directory name ``releases/<first 16 hex>`` states exactly what is inside.

``python3 -m hy_recorder.bundle verify <dir>`` re-hashes the tree against its manifest; the install
script runs it after unpacking. It needs nothing but the standard library.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

MANIFEST = "MANIFEST.json"
FORMAT = 1
DIGEST_CHARS = 16


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_path(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while chunk := fh.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def hash_tree(root: Path) -> dict[str, str]:
    """``{relative posix path: sha256}`` of every regular file except the manifest and bytecode caches."""
    files: dict[str, str] = {}
    for path in sorted(Path(root).rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        rel = path.relative_to(root).as_posix()
        if rel == MANIFEST or "__pycache__" in path.parts or rel.endswith(".pyc"):
            continue
        files[rel] = sha256_path(path)
    return files


def tree_digest(files: dict[str, str]) -> str:
    h = hashlib.sha256()
    for rel in sorted(files):
        h.update(rel.encode("utf-8") + b"\0" + files[rel].encode("ascii") + b"\n")
    return h.hexdigest()


def verify_bundle(root: Path, *, check_name: bool = False) -> list[str]:
    """Return a list of problems (empty means the tree is exactly what its manifest says)."""
    root = Path(root)
    problems: list[str] = []
    try:
        manifest = json.loads((root / MANIFEST).read_bytes())
    except (OSError, ValueError) as exc:
        return ["cannot read %s: %s" % (MANIFEST, exc)]
    if manifest.get("format") != FORMAT:
        problems.append("unsupported manifest format %r" % (manifest.get("format"),))
    declared = manifest.get("files")
    if not isinstance(declared, dict) or not declared:
        return problems + ["manifest lists no files"]
    actual = hash_tree(root)
    for rel in sorted(set(declared) - set(actual)):
        problems.append("missing: " + rel)
    for rel in sorted(set(actual) - set(declared)):
        problems.append("unexpected file: " + rel)
    for rel in sorted(set(declared) & set(actual)):
        if declared[rel] != actual[rel]:
            problems.append("modified: " + rel)
    if tree_digest(declared) != manifest.get("digest"):
        problems.append("manifest digest does not match its own file list")
    if check_name and root.resolve().name != str(manifest.get("digest", ""))[:DIGEST_CHARS]:
        problems.append("directory name %r is not the release digest" % root.resolve().name)
    return problems


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) < 2 or args[0] != "verify":
        print("usage: python3 -m hy_recorder.bundle verify <dir> [--check-name]", file=sys.stderr)
        return 2
    problems = verify_bundle(Path(args[1]), check_name="--check-name" in args[2:])
    for line in problems:
        print("BUNDLE PROBLEM: " + line, file=sys.stderr)
    if not problems:
        print("bundle ok: %s" % Path(args[1]).resolve().name)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
