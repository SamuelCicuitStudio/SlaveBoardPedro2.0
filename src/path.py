#!/usr/bin/env python3
"""
PlatformIO build_flags auto include generator.

- Prompts the user to select a platformio.ini
- Scans ./src/** from the folder the script is launched in (cwd)
- Builds a list of -I include paths for directories that contain C/C++ source/header files
- Updates build_flags blocks in the ini, preserving existing non -I flags
- Creates a .bak backup next to the ini

Works best on Windows/macOS/Linux with tkinter available.
"""

from __future__ import annotations

import os
import re
import shutil
from pathlib import Path
from typing import List, Tuple

C_EXTS = {".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".ino", ".S", ".s"}

def pick_ini_file() -> Path:
    """Open a file picker to select the platformio.ini (fallback to CLI)."""
    try:
        import tkinter as tk
        from tkinter import filedialog

        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)

        file_path = filedialog.askopenfilename(
            title="Select platformio.ini",
            filetypes=[("PlatformIO INI", "*.ini"), ("All files", "*.*")],
        )
        if not file_path:
            raise SystemExit("No file selected.")
        return Path(file_path).resolve()
    except Exception:
        # Fallback: ask user in terminal
        p = input("Enter path to platformio.ini: ").strip().strip('"')
        if not p:
            raise SystemExit("No path provided.")
        return Path(p).resolve()

def has_c_like_files(dir_path: Path) -> bool:
    """Return True if dir contains any C/C++/Arduino source or header (non-recursive)."""
    try:
        for item in dir_path.iterdir():
            if item.is_file() and item.suffix in C_EXTS:
                return True
    except Exception:
        return False
    return False

def scan_include_dirs(project_root: Path) -> List[str]:
    """
    Scan project_root/src recursively and return include dirs like 'src/services/utils/'.
    Only include dirs that contain at least one C-like file.
    """
    src = project_root / "src"
    if not src.exists() or not src.is_dir():
        raise SystemExit(f"Could not find 'src' directory in {project_root}")

    include_dirs: List[str] = []

    # Walk directories under src (including deeper levels)
    for dirpath, dirnames, filenames in os.walk(src):
        # Skip hidden and common junk folders
        dirnames[:] = [
            d for d in dirnames
            if not d.startswith(".")
            and d not in {"__pycache__", ".pio", "build", "libdeps", ".git"}
        ]

        p = Path(dirpath)

        # Only include directories that have C/C++ files
        if any(Path(f).suffix in C_EXTS for f in filenames):
            rel = p.relative_to(project_root).as_posix()
            if not rel.endswith("/"):
                rel += "/"
            # match your style: start at src/... (not adding "-I src/")
            include_dirs.append(rel)

    # Make stable + nice ordering:
    # 1) shorter paths first (src/app/ before src/app/device/)
    # 2) then lexicographic
    include_dirs = sorted(set(include_dirs), key=lambda s: (s.count("/"), s))
    return include_dirs

def parse_build_flags_block(lines: List[str], start_idx: int) -> Tuple[int, int, List[str]]:
    """
    Given index at the 'build_flags =' line, returns:
    (block_start, block_end_exclusive, captured_continuation_lines)
    Continuation lines are those that are indented (start with whitespace) and not a new key/section.
    """
    i = start_idx + 1
    captured: List[str] = []
    while i < len(lines):
        line = lines[i]
        if line.startswith("["):  # next section
            break
        # continuation lines in ini are typically indented; stop when a new key begins (non-indented "key =")
        if line.strip() == "":
            captured.append(line)
            i += 1
            continue
        if re.match(r"^\S", line):  # non-whitespace at start => new key
            break
        captured.append(line)
        i += 1
    return start_idx, i, captured

def rebuild_build_flags(first_line: str, continuation: List[str], include_dirs: List[str]) -> List[str]:
    """
    Keep any existing non-'-I' flags from the continuation block,
    replace all existing '-I' lines with newly generated ones.
    """
    kept: List[str] = []

    # Keep comments/blank lines and any non -I flags
    for raw in continuation:
        stripped = raw.strip()
        if stripped.startswith("-I " ) or stripped.startswith("-I\t") or stripped.startswith("-I"):
            continue
        kept.append(raw)

    # Ensure first_line is exactly 'build_flags = ...' (keep anything after '=' if user had it)
    out: List[str] = [first_line]

    # If user had flags on the same line: build_flags = -D...
    # we keep it as-is. If they had empty, it's fine too.

    # Append kept continuation lines (but remove trailing blank-only tail to avoid huge gaps)
    while kept and kept[-1].strip() == "":
        kept.pop()

    if kept:
        # Ensure at least one newline if first_line didn't end with newline
        out.extend(kept)
        if out[-1].endswith("\n") is False:
            out[-1] += "\n"

    # Always add a newline before includes if last line isn't blank
    if out and out[-1].strip() != "":
        out.append("\n")

    # Add includes, matching your reference formatting (tab-indented)
    for d in include_dirs:
        out.append(f"\t-I {d}\n")

    return out

def update_ini(ini_path: Path, include_dirs: List[str]) -> None:
    text = ini_path.read_text(encoding="utf-8", errors="replace")
    # Keep original line endings as much as possible: splitlines(keepends=True)
    lines = text.splitlines(keepends=True)

    build_flags_pattern = re.compile(r"^(\s*)build_flags\s*=")

    # Find all build_flags lines
    indices = [i for i, line in enumerate(lines) if build_flags_pattern.match(line)]

    if indices:
        # Update each existing build_flags block
        offset = 0
        for idx in indices:
            idx += offset
            block_start, block_end, cont = parse_build_flags_block(lines, idx)
            new_block = rebuild_build_flags(lines[block_start], cont, include_dirs)
            # Replace old block (start line + continuation)
            old_len = block_end - block_start
            lines[block_start:block_end] = new_block
            offset += len(new_block) - old_len
    else:
        # No build_flags found: insert under first [env:...] section if present
        env_idx = next((i for i, line in enumerate(lines) if line.strip().startswith("[env:")), None)
        insert_at = env_idx + 1 if env_idx is not None else len(lines)

        new_block = ["build_flags =\n"]
        for d in include_dirs:
            new_block.append(f"\t-I {d}\n")
        new_block.append("\n")

        lines[insert_at:insert_at] = new_block

    new_text = "".join(lines)

    # Backup
    bak = ini_path.with_suffix(ini_path.suffix + ".bak")
    shutil.copy2(ini_path, bak)

    ini_path.write_text(new_text, encoding="utf-8")

def main():
    project_root = Path.cwd().resolve()
    ini_path = pick_ini_file()

    include_dirs = scan_include_dirs(project_root)

    if not include_dirs:
        raise SystemExit("No include directories found (no C/C++ files under ./src/**).")

    update_ini(ini_path, include_dirs)

    print("Done.")
    print(f"Project root (scanned): {project_root}")
    print(f"Updated: {ini_path}")
    print(f"Backup:  {ini_path.with_suffix(ini_path.suffix + '.bak')}")
    print("Generated include dirs:")
    for d in include_dirs:
        print("  -I", d)

if __name__ == "__main__":
    main()
