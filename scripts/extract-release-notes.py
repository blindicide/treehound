#!/usr/bin/env python3
"""Extract exactly one changelog section for a v-prefixed SemVer tag."""

import argparse
from pathlib import Path
import re


NUMBER = r"(?:0|[1-9][0-9]*)"
PRERELEASE_ID = rf"(?:{NUMBER}|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
SEMVER_TAG = re.compile(
    rf"v({NUMBER}\.{NUMBER}\.{NUMBER}"
    rf"(?:-{PRERELEASE_ID}(?:\.{PRERELEASE_ID})*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)"
)


def extract_release_notes(changelog: str, tag: str) -> str:
    """Keep the matching heading and body, ending before the next '## '."""
    match = SEMVER_TAG.fullmatch(tag)
    if match is None:
        raise ValueError(f"invalid release tag {tag!r}: expected v-prefixed SemVer")
    version = match.group(1)
    heading = re.compile(rf"^## {re.escape(version)}(?=[\s]|$)")
    lines = changelog.splitlines(keepends=True)
    starts = [index for index, line in enumerate(lines) if heading.match(line)]
    if not starts:
        raise ValueError(f"no changelog section for version {version}")
    if len(starts) != 1:
        raise ValueError(f"ambiguous changelog section for version {version}: {len(starts)} headings")
    start = starts[0]
    end = next(
        (index for index in range(start + 1, len(lines)) if lines[index].startswith("## ")),
        len(lines),
    )
    return "".join(lines[start:end])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="Release tag, e.g. v0.5.2")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        notes = extract_release_notes(args.changelog.read_text(encoding="utf-8"), args.tag)
        args.output.write_text(notes, encoding="utf-8")
    except (ValueError, OSError) as error:
        parser.exit(1, f"release notes: {error}\n")


if __name__ == "__main__":
    main()
