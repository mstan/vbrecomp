"""Helpers to compute V810 opcode-coverage state from the live ISA table.

The canonical record of "what is implemented" is `docs/INSTRUCTION_STATUS.md`.
This module derives a coverage histogram from a decoded ROM so that
table can be regenerated mechanically (Phase 2+) and never drifts
silently.
"""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from typing import Iterable, List, Tuple

from .decoder import DecodedInstruction


@dataclass
class CoverageReport:
    """Opcode coverage over a decoded stream."""
    total_insns: int
    unknown_insns: int
    by_mnemonic: Counter            # mnemonic -> count
    unknown_examples: List[Tuple[int, str]]   # (pc, raw_hex) — first N unknowns

    @property
    def unknown_rate(self) -> float:
        if self.total_insns == 0:
            return 0.0
        return self.unknown_insns / self.total_insns

    def top(self, n: int = 20) -> List[Tuple[str, int]]:
        return self.by_mnemonic.most_common(n)


def coverage_of(insns: Iterable[DecodedInstruction],
                *, unknown_sample_limit: int = 32) -> CoverageReport:
    by = Counter()
    unknown_count = 0
    total = 0
    samples: List[Tuple[int, str]] = []

    for ins in insns:
        total += 1
        by[ins.mnemonic] += 1
        if ins.is_unknown:
            unknown_count += 1
            if len(samples) < unknown_sample_limit:
                samples.append((ins.pc, ins.raw.hex()))

    return CoverageReport(
        total_insns=total,
        unknown_insns=unknown_count,
        by_mnemonic=by,
        unknown_examples=samples,
    )
