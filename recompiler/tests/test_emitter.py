"""Tests for v810/emitter.py — verify the C source it emits has the
expected shape for representative instruction encodings.

These tests are mnemonic-pattern tests, not full compile-and-run
tests; the latter happens implicitly when `vbrecomp-codegen` ships
into the CMake build (test_decoder_oracle covers per-insn correctness
on the decoder side, and the C compiler covers syntactic validity).
"""
from __future__ import annotations

import unittest

from recompiler.v810.analysis import (
    FunctionRange,
    RomImage,
)
from recompiler.v810.emitter import (
    _bb_label,
    _fn_symbol,
    emit_dispatch_c,
    emit_function,
    emit_header,
    recompile_rom,
)


# Re-use the encoding helpers from test_analysis — duplicated narrowly
# rather than imported across test modules.
def _enc_fmt_i(opcode6, reg2, reg1):
    hw = (opcode6 << 10) | (reg2 << 5) | reg1
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])

def _enc_fmt_ii(opcode6, reg2, imm5):
    return _enc_fmt_i(opcode6, reg2, imm5 & 0x1F)

def _enc_fmt_v(opcode6, reg2, reg1, imm16):
    hw0 = (opcode6 << 10) | (reg2 << 5) | reg1
    hw1 = imm16 & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])

def _enc_fmt_iv(opcode6, disp26):
    raw = disp26 & ((1 << 26) - 1)
    hw0 = (opcode6 << 10) | ((raw >> 16) & 0x3FF)
    hw1 = raw & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])

def _enc_bcond(cond, disp9):
    hw = (0b100 << 13) | ((cond & 0xF) << 9) | (disp9 & 0x1FF)
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])


def _make_rom(size=0x4000):
    return bytearray(size)


def _install_trampoline(rom, entry_va):
    high = (entry_va >> 16) & 0xFFFF
    low = entry_va & 0xFFFF
    if low & 0x8000:
        high = (high + 1) & 0xFFFF
        signed_low = low - 0x10000
    else:
        signed_low = low
    off = len(rom) - 16
    rom[off:off + 4] = _enc_fmt_v(0x2F, 11, 0, high)            # MOVHI
    rom[off + 4:off + 8] = _enc_fmt_v(0x28, 11, 11,
                                     signed_low & 0xFFFF)        # MOVEA
    rom[off + 8:off + 10] = _enc_fmt_i(0x06, 0, 11)              # JMP r11


class TestEmitFunctionShape(unittest.TestCase):
    def test_basic_function_recompile_shape(self):
        rom = _make_rom()
        # Entry at 0x07000020. JAL +0x40 → callee at 0x60.
        rom[0x20:0x24] = _enc_fmt_iv(0x2B, 0x40)   # JAL +0x40
        rom[0x24:0x26] = _enc_fmt_ii(0x1A, 0, 0)   # HALT
        rom[0x60:0x62] = _enc_fmt_i(0x00, 2, 1)    # MOV r1, r2
        rom[0x62:0x64] = _enc_fmt_i(0x06, 0, 31)   # JMP r31 (return)
        _install_trampoline(rom, 0x07000020)

        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")

        # All three artifacts must exist.
        self.assertIn("generated/testcart_full.c", result.files)
        self.assertIn("generated/testcart_dispatch.c", result.files)
        self.assertIn("generated/testcart.h", result.files)

        full = result.files["generated/testcart_full.c"]
        # Trampoline tracer resolves through the trampoline (it never
        # appears as a discovered function); the cart entry and the
        # JAL callee both do.
        self.assertIn(_fn_symbol(0x07000020), full)
        self.assertIn(_fn_symbol(0x07000060), full)
        # The entry function's JAL must turn into vb_dispatch_call.
        self.assertIn("vb_dispatch_call(cpu, 0x07000060u, "
                      "0x07000024u);", full)
        # The entry function's HALT must compile to halted=1 + return.
        self.assertIn("cpu->halted = 1;", full)
        # JMP r31 compiles to `cpu->pc = cpu->gpr[31]; return;` so the
        # outer dispatch_call loop's `cpu->pc == saved_lp` check can
        # detect a clean return.
        callee_body_start = full.index(_fn_symbol(0x07000060))
        callee_chunk = full[callee_body_start:full.index("\n}\n", callee_body_start)]
        self.assertIn("cpu->pc = cpu->gpr[31] & 0xFFFFFFFEu;", callee_chunk)
        self.assertIn("return;", callee_chunk)

    def test_dispatch_table_covers_every_function(self):
        rom = _make_rom()
        rom[0x20:0x24] = _enc_fmt_iv(0x2B, 0x40)
        rom[0x24:0x26] = _enc_fmt_ii(0x1A, 0, 0)
        rom[0x60:0x62] = _enc_fmt_i(0x00, 2, 1)
        rom[0x62:0x64] = _enc_fmt_i(0x06, 0, 31)
        _install_trampoline(rom, 0x07000020)
        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")
        dispatch = result.files["generated/testcart_dispatch.c"]
        # Only real instruction boundaries are dispatchable, including
        # the return address and the callee's second instruction.
        for pc, entry in [(0x20,0x20),(0x24,0x20),(0x60,0x60),(0x62,0x60)]:
            self.assertIn(f"{{0x070000{pc:02X}u, vb_fn_070000{entry:02X}}}", dispatch)
        self.assertNotIn("{0x07000022u,", dispatch)
        # vb_dispatch_call must set up r31 and loop until the callee
        # returns to the caller's expected lp.
        self.assertIn("cpu->gpr[31] = lp;", dispatch)
        self.assertIn("cpu->pc != saved_lp", dispatch)
        # vb_dispatch is the top-level wrapper using a sentinel lp.
        self.assertIn("VB_TOP_LEVEL_LP", dispatch)

    def test_header_declares_each_function(self):
        rom = _make_rom()
        rom[0x20:0x22] = _enc_fmt_i(0x06, 0, 31)   # tiny: just `JMP r31`
        _install_trampoline(rom, 0x07000020)
        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")
        header = result.files["generated/testcart.h"]
        self.assertIn(f"void {_fn_symbol(0x07000020)}(CPUState* cpu);", header)


class TestEmitAluPatterns(unittest.TestCase):
    """Verify a few representative ALU encodings produce sensible C."""

    def _emit_single_insn_function(self, encoded: bytes,
                                   entry_va: int = 0x07000020) -> str:
        """Helper: drop a single insn + `JMP r31` at entry_va and run
        the emitter; return just the resulting function body."""
        rom = _make_rom()
        off = entry_va - 0x07000000
        rom[off:off + len(encoded)] = encoded
        ret_off = off + len(encoded)
        rom[ret_off:ret_off + 2] = _enc_fmt_i(0x06, 0, 31)  # JMP r31
        _install_trampoline(rom, entry_va)
        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")
        return result.files["generated/testcart_full.c"]

    def test_movhi_emits_shift_and_add(self):
        full = self._emit_single_insn_function(
            _enc_fmt_v(0x2F, 1, 0, 0x0500))  # MOVHI 0x0500, r0, r1
        self.assertIn("cpu->gpr[1] = cpu->gpr[0] + "
                      "(uint32_t)(0x0500u << 16);", full)

    def test_add_reg_emits_psw_update(self):
        full = self._emit_single_insn_function(
            _enc_fmt_i(0x01, 2, 1))   # ADD r1, r2
        # Z, S, CY, OV flags must all be touched.
        self.assertIn("cpu->psw_z=", full)
        self.assertIn("cpu->psw_s=", full)
        self.assertIn("cpu->psw_cy=", full)
        self.assertIn("cpu->psw_ov=", full)

    def test_writes_to_r0_are_suppressed(self):
        # MOV r1, r0 — destination is r0, hardwired to zero. The emit
        # must NOT generate cpu->gpr[0] = ... or the constant breaks.
        full = self._emit_single_insn_function(_enc_fmt_i(0x00, 0, 1))
        self.assertNotIn("cpu->gpr[0] =", full)


class TestEmitBcond(unittest.TestCase):
    def test_be_within_function_becomes_goto(self):
        rom = _make_rom()
        # Function at 0x07000020:
        #   0x20: BE +4   (target 0x24)
        #   0x22: ADD r1,r2 (fall-through filler)
        #   0x24: JMP r31  (return)
        rom[0x20:0x22] = _enc_bcond(0x2, 4)
        rom[0x22:0x24] = _enc_fmt_i(0x01, 2, 1)
        rom[0x24:0x26] = _enc_fmt_i(0x06, 0, 31)
        _install_trampoline(rom, 0x07000020)
        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")
        full = result.files["generated/testcart_full.c"]
        # Branch target 0x07000024 is in-function — must become a goto.
        self.assertIn(f"goto {_bb_label(0x07000024)};", full)
        self.assertIn(f"{_bb_label(0x07000024)}:;", full)

    def test_cond_nop_emits_nothing(self):
        # Bcond cond=0xD is the NOP encoding — emits nothing.
        rom = _make_rom()
        rom[0x20:0x22] = _enc_bcond(0xD, 0)
        rom[0x22:0x24] = _enc_fmt_i(0x06, 0, 31)
        _install_trampoline(rom, 0x07000020)
        img = RomImage.from_bytes(bytes(rom))
        result = recompile_rom(img, module_name="testcart")
        full = result.files["generated/testcart_full.c"]
        # No goto, no `if`, no vb_dispatch generated by the NOP itself.
        # (PC update will still be there.)
        nop_pc_line = "cpu->pc = 0x07000020u;"
        self.assertIn(nop_pc_line, full)
        # Verify there's no spurious branch hanging off the NOP.
        idx = full.index(nop_pc_line)
        tail = full[idx + len(nop_pc_line):idx + len(nop_pc_line) + 80]
        self.assertNotIn("vb_dispatch", tail.split("cpu->pc")[0])


if __name__ == "__main__":
    unittest.main()
