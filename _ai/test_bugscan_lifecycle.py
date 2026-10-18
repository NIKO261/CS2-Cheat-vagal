"""Static lifecycle regression; no DLL loading, game calls, or compilation."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def mask_cpp(source: str) -> str:
    """Keep offsets/braces while ignoring comments and quoted text."""
    tokens = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(tokens, lambda match: re.sub(r"[^\n]", " ", match[0]), source, flags=re.S)


def block_after(source: str, pattern: str, start: int = 0) -> str:
    match = re.search(pattern, source[start:], re.S)
    if not match:
        return ""
    opening = source.find("{", start + match.end())
    if opening < 0:
        return ""
    depth = 1
    for index in range(opening + 1, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[opening + 1:index]
    return ""


def project_dev(source: str, development: bool) -> str:
    """Select entry.cpp's DEV branches without invoking a C++ preprocessor."""
    active = True
    stack: list[tuple[bool, bool]] = []
    output: list[str] = []
    for line in source.splitlines():
        directive = line.strip()
        if directive.startswith("#if"):
            if not re.fullmatch(r"#if\s+defined\s*\(\s*DEV\s*\)", directive):
                raise ValueError(f"Unhandled detach directive: {directive}")
            stack.append((active, development))
            active = active and development
        elif directive == "#else":
            parent, branch = stack[-1]
            active = parent and not branch
        elif directive == "#endif":
            active, _ = stack.pop()
        elif active:
            output.append(line)
    if stack:
        raise ValueError("Unbalanced detach preprocessor branches")
    return "\n".join(output)


def run(root: Path) -> int:
    cpp = mask_cpp((root / "firgune/project/entry.cpp").read_text(encoding="utf-8-sig"))
    init = block_after(cpp, r"DWORD\s+WINAPI\s+init_thread_impl\s*\([^)]*\)")
    detach = block_after(cpp, r"else\s+if\s*\(\s*reason\s*==\s*DLL_PROCESS_DETACH\s*\)")
    checks: list[tuple[str, bool]] = []

    def check(name: str, passed: bool) -> None:
        checks.append((name, passed))
        print(f"{'PASS' if passed else 'FAIL'}: {name}")

    check("initialization registers VEH and UEF", bool(init) and
          "AddVectoredExceptionHandler" in init and "SetUnhandledExceptionFilter" in init)
    check("detach entry exists", bool(detach))

    for development in (False, True):
        mode = "Development" if development else "Release"
        selected = project_dev(detach, development)
        check(f"{mode}: manual detach removes VEH", "RemoveVectoredExceptionHandler" in selected)
        check(f"{mode}: manual detach restores UEF", "SetUnhandledExceptionFilter" in selected)
        check(f"{mode}: manual detach unregisters events", "systems::events::shutdown" in selected)
        check(f"{mode}: detach never uninitializes another thread's COM", "CoUninitialize" not in selected)

        termination = block_after(selected, r"if\s*\(\s*reserved\s*!=\s*nullptr\s*\)")
        gate = selected.find("reserved != nullptr")
        first_cleanup = min((selected.find(token) for token in (
            "RemoveVectoredExceptionHandler", "systems::events::shutdown",
            "hooks::cheat::shutdown", "systems::g_account.shutdown")
            if token in selected), default=-1)
        check(f"{mode}: process termination skips manual cleanup", bool(termination) and
              bool(re.search(r"return\s+1\s*;", termination)) and 0 <= gate < first_cleanup)

        external_filter = block_after(selected,
            r"if\s*\(\s*current_filter\s*!=\s*diag_unhandled_exception_filter\s*\)")
        check(f"{mode}: a later external UEF is preserved", bool(re.search(
            r"SetUnhandledExceptionFilter\s*\(\s*current_filter\s*\)", external_filter)))

        cleanup_positions = [selected.find(token) for token in (
            "RemoveVectoredExceptionHandler", "systems::events::shutdown",
            "hooks::cheat::shutdown", "diag::shutdown")]
        check(f"{mode}: callbacks leave before hooks and diagnostics", all(
            position >= 0 for position in cleanup_positions) and
            cleanup_positions[0] < cleanup_positions[2] < cleanup_positions[3] and
            cleanup_positions[1] < cleanup_positions[2])

    com_result = re.search(r"(?:const\s+)?auto\s+(\w+)\s*=\s*CoInitializeEx\s*\(", init)
    check("COM initializes once in the initialization thread", len(re.findall(r"\bCoInitializeEx\s*\(", init)) == 1)
    destructors = [block_after(init, r"~\s*" + re.escape(match[1]) + r"\s*\(\s*\)", match.start())
                   for match in re.finditer(r"~\s*(\w+)\s*\(\s*\)", init)]
    com_destructors = [body for body in destructors if "CoUninitialize" in body]
    check("COM cleanup is local RAII on the initialization thread", len(com_destructors) == 1)
    success_guarded = False
    for body in com_destructors:
        succeeded = re.search(r"if\s*\(\s*SUCCEEDED\s*\(\s*(\w+)\s*\)\s*\)", body)
        if succeeded and "CoUninitialize" in block_after(body, re.escape(succeeded[0])):
            member = succeeded[1]
            if com_result and (member == com_result[1] or re.search(
                    r"\b\w+\s*\{\s*" + re.escape(com_result[1]) + r"\s*\}\s*;", init)):
                success_guarded = True
    check("COM RAII only uninitializes SUCCEEDED initialization", success_guarded)
    check("COM has one uninitialization site", len(re.findall(r"\bCoUninitialize\s*\(", cpp)) == 1)

    passed = sum(result for _, result in checks)
    print(f"LIFECYCLE: {passed}/{len(checks)} passed")
    print("Evidence: source contracts only; no runtime unload or COM execution.")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    sys.exit(run(args.root.resolve()))
