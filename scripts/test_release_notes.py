"""Dependency-free release-note tests; never invoke a publishing command."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("extract-release-notes.py")
SPEC = importlib.util.spec_from_file_location("release_notes", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
extract_release_notes = MODULE.extract_release_notes


class ReleaseNotesTests(unittest.TestCase):
    def test_matching_section_among_multiple_versions(self):
        section = "## 0.5.2 — 2026-10-10\n\n- Current changes.\n### Fixes\n- More fixes.\n\n"
        changelog = "# Changelog\n\n## 0.6.0\n- Future.\n\n" + section + "## 0.5.1\n- Old.\n"
        self.assertEqual(extract_release_notes(changelog, "v0.5.2"), section)

    def test_last_section_through_eof_without_final_newline(self):
        section = "## 0.1.0\n\n- First release."
        self.assertEqual(extract_release_notes("## 0.2.0\n- New.\n\n" + section, "v0.1.0"), section)

    def test_last_section_preserves_final_newline(self):
        section = "## 0.1.0\n- First release.\n"
        self.assertEqual(extract_release_notes(section, "v0.1.0"), section)

    def test_missing_version(self):
        with self.assertRaisesRegex(ValueError, "no changelog section for version 0.5.2"):
            extract_release_notes("## 0.5.1\n- Old.\n", "v0.5.2")

    def test_duplicate_matching_headers(self):
        changelog = "## 0.5.2 — today\n- One.\n## 0.5.1\n- Old.\n## 0.5.2 — yesterday\n- Two.\n"
        with self.assertRaisesRegex(ValueError, "ambiguous.*0.5.2: 2 headings"):
            extract_release_notes(changelog, "v0.5.2")

    def test_stops_at_next_heading_even_if_not_a_version(self):
        changelog = "## 0.5.2\n- Current.\n## Unreleased\n- Future.\n## 0.5.1\n- Old.\n"
        self.assertEqual(extract_release_notes(changelog, "v0.5.2"), "## 0.5.2\n- Current.\n")

    def test_version_prefixes_do_not_match(self):
        changelog = "## 0.5.20\n- Other.\n## 0.5.2-rc.1\n- Preview.\n## 0.5.2+build.1\n- Build.\n"
        with self.assertRaisesRegex(ValueError, "no changelog section"):
            extract_release_notes(changelog, "v0.5.2")

    def test_semver_prerelease_and_build_metadata(self):
        section = "## 1.2.3-rc.1+build.42 — today\n- Preview.\n"
        self.assertEqual(extract_release_notes(section, "v1.2.3-rc.1+build.42"), section)

    def test_invalid_tags(self):
        for tag in ("0.5.2", "main", "v0.5", "v01.5.2", "v0.5.2-01", "v0.5.2+", "v0.5.2\n"):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "invalid release tag"):
                extract_release_notes("## 0.5.2\n", tag)

    def test_cli_writes_only_matching_section(self):
        with tempfile.TemporaryDirectory() as directory:
            changelog = Path(directory) / "CHANGELOG.md"
            output = Path(directory) / "notes.md"
            section = "## 0.5.2 — today\n- Current.\n"
            changelog.write_text(section + "## 0.5.1\n- Old.\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--tag", "v0.5.2", "--changelog", str(changelog), "--output", str(output)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), section)

    def test_cli_errors_do_not_create_notes_file(self):
        for changelog_text, tag, message in (
            ("## 0.5.1\n", "v0.5.2", "no changelog section"),
            ("## 0.5.2\n## 0.5.2\n", "v0.5.2", "ambiguous changelog section"),
            ("## 0.5.2\n", "main", "invalid release tag"),
        ):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                changelog = Path(directory) / "CHANGELOG.md"
                output = Path(directory) / "notes.md"
                changelog.write_text(changelog_text, encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, str(SCRIPT), "--tag", tag, "--changelog", str(changelog), "--output", str(output)],
                    capture_output=True, text=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
