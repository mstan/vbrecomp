"""DecodedInstruction → IR lifter for V810.

Phase 3 of the project. Skeleton exposes the API shape only.
"""
from __future__ import annotations

from typing import List

from .decoder import DecodedInstruction
from .ir import IROp


PHASE = "P3 — IR lifter"


def lift(insn: DecodedInstruction) -> List[IROp]:
    raise NotImplementedError(
        f"lifter is part of {PHASE}; "
        f"see docs/INSTRUCTION_STATUS.md for current coverage"
    )
