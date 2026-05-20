"""TOML config loader tests."""
from __future__ import annotations

import tempfile
import textwrap
import unittest
from pathlib import Path

from recompiler.config.game_toml import load_game_config


def _write(p: Path, body: str) -> None:
    p.write_text(textwrap.dedent(body), encoding="utf-8")


class TestLoadGameConfig(unittest.TestCase):
    def test_minimal_valid(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            (tdp / "roms").mkdir()
            (tdp / "roms" / "fake.vb").write_bytes(b"\x00" * 16)
            toml = tdp / "game.toml"
            _write(toml, """
                [program]
                name       = "fake"
                rom        = "roms/fake.vb"
                load_base  = "0x07000000"
                entry_pc   = "0xFFFFFFF0"
            """)
            cfg = load_game_config(toml)
            self.assertEqual(cfg.name, "fake")
            self.assertEqual(cfg.load_base, 0x07000000)
            self.assertEqual(cfg.entry_pc, 0xFFFFFFF0)
            self.assertEqual(cfg.debug_port, 4390)   # default
            self.assertEqual(cfg.oracle_port, 4391)  # default
            self.assertAlmostEqual(cfg.frame_hz, 50.27)

    def test_runtime_overrides(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            (tdp / "fake.vb").write_bytes(b"\x00" * 16)
            toml = tdp / "g.toml"
            _write(toml, """
                [program]
                name       = "g"
                rom        = "fake.vb"
                load_base  = 0x07000000
                entry_pc   = 0xFFFFFFF0

                [runtime]
                debug_port  = 9000
                oracle_port = 9001
                window_title = "custom"
            """)
            cfg = load_game_config(toml)
            self.assertEqual(cfg.debug_port, 9000)
            self.assertEqual(cfg.oracle_port, 9001)
            self.assertEqual(cfg.window_title, "custom")

    def test_missing_required_field_raises(self):
        with tempfile.TemporaryDirectory() as td:
            toml = Path(td) / "bad.toml"
            _write(toml, """
                [program]
                name = "bad"
                # missing rom / load_base / entry_pc
            """)
            with self.assertRaises(KeyError):
                load_game_config(toml)

    def test_malformed_hex_raises(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            (tdp / "fake.vb").write_bytes(b"\x00" * 16)
            toml = tdp / "g.toml"
            _write(toml, """
                [program]
                name       = "g"
                rom        = "fake.vb"
                load_base  = "0xZZZZ"
                entry_pc   = 0xFFFFFFF0
            """)
            with self.assertRaises(ValueError):
                load_game_config(toml)

    def test_missing_toml_file(self):
        with self.assertRaises(FileNotFoundError):
            load_game_config("/this/path/definitely/does/not/exist.toml")


if __name__ == "__main__":
    unittest.main()
