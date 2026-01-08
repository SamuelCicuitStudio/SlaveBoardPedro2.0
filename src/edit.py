#!/usr/bin/env python3
"""
Project-wide rename of DEBUG_* macros to DBG_* macros.

Replacements (as whole tokens):
  DEBUG_PRINT   -> DBG_PRINT
  DEBUG_PRINTLN -> DBG_PRINTLN
  DEBUG_PRINTF  -> DBG_PRINTF
  DEBUGSTART    -> DBGSTR
  DEBUGGSTOP    -> DBGGSTP

- Walks the entire project tree starting from where this script lives.
- Edits common source/header file types only.
- Creates a .bak backup for each modified file (unless --no-backup).
- Deletes all .bak files afterward by default (use --keep-bak to keep them).
- Dry-run mode available.

Usage:
  python3 rename_debug_macros.py
  python3 rename_debug_macros.py --dry-run
  python3 rename_debug_macros.py --keep-bak
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

# File types to edit (add more if needed)
TEXT_CODE_EXTS = {
    ".h", ".hpp", ".hh", ".hxx", ".ipp", ".tpp",
    ".c", ".cc", ".cpp", ".cxx",
    ".ino",
    ".S", ".s",
}

SKIP_DIRS = {".git", ".pio", "build", "dist", "out", "bin", "libdeps", "__pycache__"}

REPLACEMENTS = {
    "LOG->": "LOGG->",

}

# Build regex to replace whole identifiers only
# e.g. won't change MY_DEBUG_PRINT or DEBUG_PRINTER, etc.
_PATTERN = re.compile(r"\b(" + "|".join(map(re.escape, sorted(REPLACEMENTS, key=len, reverse=True))) + r")\b")


def should_process_file(p: Path) -> bool:
    return p.is_file() and p.suffix in TEXT_CODE_EXTS


def iter_project_files(root: Path):
    for p in root.rglob("*"):
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        if should_process_file(p):
            yield p


def rewrite_text(text: str) -> tuple[str, int]:
    count = 0

    def _sub(m: re.Match) -> str:
        nonlocal count
        count += 1
        return REPLACEMENTS[m.group(1)]

    new_text = _PATTERN.sub(_sub, text)
    return new_text, count


def delete_bak_files(root: Path) -> int:
    deleted = 0
    for bak in root.rglob("*.bak"):
        if any(part in SKIP_DIRS for part in bak.parts):
            continue
        try:
            bak.unlink()
            deleted += 1
        except Exception as e:
            print(f"[WARN] Could not delete {bak}: {e}")
    return deleted


def main():
    ap = argparse.ArgumentParser(description="Rename DEBUG_* macros to DBG_* across the project.")
    ap.add_argument("--dry-run", action="store_true", help="Show what would change, but do not write files")
    ap.add_argument("--no-backup", action="store_true", help="Do not create .bak backups")
    ap.add_argument("--keep-bak", action="store_true", help="Do not delete .bak files at the end")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent

    files_changed = 0
    total_rewrites = 0

    for file_path in iter_project_files(root):
        try:
            original = file_path.read_text(encoding="utf-8", errors="replace")
        except Exception as e:
            print(f"[SKIP] {file_path} (read error: {e})")
            continue

        updated, n = rewrite_text(original)
        if n == 0:
            continue

        files_changed += 1
        total_rewrites += n

        if args.dry_run:
            print(f"[DRY] {file_path}  ({n} replacement(s))")
            continue

        try:
            if not args.no_backup:
                bak = file_path.with_suffix(file_path.suffix + ".bak")
                bak.write_text(original, encoding="utf-8")
            file_path.write_text(updated, encoding="utf-8")
            print(f"[OK]  {file_path}  ({n} replacement(s))")
        except Exception as e:
            print(f"[FAIL] {file_path} (write error: {e})")

    deleted_baks = 0
    if not args.dry_run and not args.no_backup and not args.keep_bak:
        deleted_baks = delete_bak_files(root)

    print("\nDone.")
    print(f"root scanned:       {root}")
    print(f"files changed:      {files_changed}")
    print(f"total replacements: {total_rewrites}")
    if not args.dry_run and not args.no_backup and not args.keep_bak:
        print(f".bak files deleted: {deleted_baks}")


if __name__ == "__main__":
    main()
