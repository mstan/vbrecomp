"""Per-game TOML config loader.

Schema (see `games/*.toml.example`):

    [program]
    name        = "...""
    rom         = "roms/foo.vb"
    rom_sha256  = "..."          (optional; verified if present)
    load_base   = "0x07000000"
    entry_pc    = "0xFFFFFFF0"

    [runtime]
    window_title = "..."
    debug_port   = 4390
    oracle_port  = 4391
    frame_hz     = 50.27

    [functions]
    0xADDR = "name"     (hand-seeded entry points; rare)

    [mmio]              (MMIO range overrides — typically empty)
    [breakpoints]       (block-PC breakpoints for the debug build)

Hex parsing is strict: a string that looks like a number but isn't a
valid hex literal raises ValueError immediately. The recompiler never
silently coerces a typo into 0.
"""
from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Optional

if sys.version_info >= (3, 11):
    import tomllib  # stdlib, Python 3.11+
else:
    try:
        import tomli as tomllib  # pip-installable backport
    except ImportError as e:  # pragma: no cover
        raise RuntimeError(
            "vbrecomp requires TOML support. Either upgrade to Python 3.11+ "
            "(stdlib tomllib) or install the backport: `pip install tomli`."
        ) from e


@dataclass
class GameConfig:
    config_path: Path                  # absolute path of the TOML file
    name: str
    rom_path: Path                     # absolute path of the ROM
    rom_sha256: Optional[str]
    load_base: int
    entry_pc: int

    window_title: str
    debug_port: int
    oracle_port: int
    frame_hz: float

    forced_functions: Dict[int, str] = field(default_factory=dict)
    mmio_overrides: Dict[str, object] = field(default_factory=dict)
    breakpoints: Dict[int, str] = field(default_factory=dict)


def _parse_hex(value: object, *, field_name: str) -> int:
    """Parse `value` as an integer.

    Accepts:
      - actual ints (already parsed by tomllib)
      - strings starting with "0x"/"0X"
      - strings of decimal digits

    Anything else raises ValueError naming `field_name`.
    """
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        s = value.strip()
        try:
            if s.lower().startswith("0x"):
                return int(s, 16)
            return int(s, 10)
        except ValueError as e:
            raise ValueError(
                f"config field '{field_name}': cannot parse {value!r} as integer ({e})"
            )
    raise ValueError(
        f"config field '{field_name}': expected int or hex string, got {type(value).__name__}"
    )


def _resolve_path(p: str, *, config_dir: Path) -> Path:
    """Resolve a path relative to the TOML file's directory if not absolute."""
    pp = Path(p)
    if pp.is_absolute():
        return pp
    return (config_dir / pp).resolve()


def load_game_config(path: str | Path) -> GameConfig:
    """Load and validate a per-game TOML config.

    Raises:
      FileNotFoundError if the TOML or referenced ROM is missing.
      KeyError for missing required fields.
      ValueError for malformed hex / unexpected types.
    """
    config_path = Path(path).resolve()
    if not config_path.is_file():
        raise FileNotFoundError(f"game config not found: {config_path}")

    with config_path.open("rb") as f:
        doc = tomllib.load(f)

    config_dir = config_path.parent

    prog = doc.get("program")
    if not isinstance(prog, dict):
        raise KeyError(f"{config_path}: missing required [program] table")

    for key in ("name", "rom", "load_base", "entry_pc"):
        if key not in prog:
            raise KeyError(f"{config_path}: missing required field [program].{key}")

    rom_path = _resolve_path(prog["rom"], config_dir=config_dir)
    rom_sha = prog.get("rom_sha256") or None
    if rom_sha is not None and not isinstance(rom_sha, str):
        raise ValueError(f"{config_path}: [program].rom_sha256 must be a string")
    if isinstance(rom_sha, str) and rom_sha.strip() == "":
        rom_sha = None  # treat empty string as unset

    rt = doc.get("runtime", {}) or {}
    window_title = rt.get("window_title", f"vbrecomp - {prog['name']}")
    debug_port = int(rt.get("debug_port", 4390))
    oracle_port = int(rt.get("oracle_port", 4391))
    frame_hz = float(rt.get("frame_hz", 50.27))

    funcs_raw = doc.get("functions", {}) or {}
    forced: Dict[int, str] = {}
    for k, v in funcs_raw.items():
        addr = _parse_hex(k, field_name=f"[functions].{k}")
        if not isinstance(v, str):
            raise ValueError(f"{config_path}: [functions].{k} must be a string name")
        forced[addr] = v

    bp_raw = doc.get("breakpoints", {}) or {}
    bps: Dict[int, str] = {}
    for k, v in bp_raw.items():
        addr = _parse_hex(k, field_name=f"[breakpoints].{k}")
        bps[addr] = str(v) if not isinstance(v, str) else v

    return GameConfig(
        config_path=config_path,
        name=str(prog["name"]),
        rom_path=rom_path,
        rom_sha256=rom_sha,
        load_base=_parse_hex(prog["load_base"], field_name="[program].load_base"),
        entry_pc=_parse_hex(prog["entry_pc"], field_name="[program].entry_pc"),
        window_title=window_title,
        debug_port=debug_port,
        oracle_port=oracle_port,
        frame_hz=frame_hz,
        forced_functions=forced,
        mmio_overrides=dict(doc.get("mmio", {}) or {}),
        breakpoints=bps,
    )
