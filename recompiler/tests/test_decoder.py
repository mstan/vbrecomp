"""Decoder unit tests.

Synthetic V810 instructions per format, asserting that
`decode_at` extracts the operand fields correctly and that the
mnemonic + format match the ISA table.

These tests do NOT validate semantic correctness — they verify
the decoder agrees with the V810 encoding spec for each format
(per the V810 Architecture Manual). Cross-instruction L1 oracle
parity is a Phase 2 deliverable (`test_decoder_oracle.py`).
"""
from __future__ import annotations

import unittest

from recompiler.v810.decoder import decode_at, scan
from recompiler.v810.isa import Format


def _enc_fmt_i(opcode6: int, reg2: int, reg1: int) -> bytes:
    """Encode a 2-byte Format I instruction (little-endian)."""
    assert 0 <= opcode6 < 0x40
    assert 0 <= reg2 < 32
    assert 0 <= reg1 < 32
    hw = (opcode6 << 10) | (reg2 << 5) | reg1
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])


def _enc_fmt_ii(opcode6: int, reg2: int, imm5: int) -> bytes:
    """Encode a 2-byte Format II instruction (imm5 stored in bits 4:0)."""
    return _enc_fmt_i(opcode6, reg2, imm5 & 0x1F)


def _enc_bcond(cond: int, disp9: int) -> bytes:
    """Encode a Format III Bcond: top 3 bits 100, cond, disp9."""
    hw = (0b100 << 13) | ((cond & 0xF) << 9) | (disp9 & 0x1FF)
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])


def _enc_fmt_iv(opcode6: int, disp26: int) -> bytes:
    """Encode a 4-byte Format IV (JR / JAL). disp26 is signed; the
    low 10 bits live in hw0[9:0], the rest in hw1."""
    raw26 = disp26 & ((1 << 26) - 1)
    hw0 = (opcode6 << 10) | ((raw26 >> 16) & 0x3FF)
    hw1 = raw26 & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])


def _enc_fmt_v(opcode6: int, reg2: int, reg1: int, imm16: int) -> bytes:
    hw0 = (opcode6 << 10) | (reg2 << 5) | reg1
    hw1 = imm16 & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])


def _enc_fmt_vi(opcode6: int, reg2: int, reg1: int, disp16: int) -> bytes:
    return _enc_fmt_v(opcode6, reg2, reg1, disp16 & 0xFFFF)


def _enc_fpp(reg2: int, reg1: int, subop: int) -> bytes:
    """Encode a Format VII FPP instruction (opcode 0x3E)."""
    hw0 = (0x3E << 10) | (reg2 << 5) | reg1
    hw1 = (subop & 0x3F) << 10
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])


def _enc_bsu(subop: int, reg1: int = 0) -> bytes:
    """Encode a Format VII BSU instruction (opcode 0x1F, subop in reg2)."""
    return _enc_fmt_i(0x1F, subop & 0x1F, reg1)


class TestFormatI(unittest.TestCase):
    def test_mov_r5_to_r3(self):
        # MOV r3, r5  → opcode 0x00, reg2=3, reg1=5
        buf = _enc_fmt_i(0x00, 3, 5)
        ins = decode_at(buf, 0, pc=0x07000000)
        self.assertEqual(ins.fmt, Format.I)
        self.assertEqual(ins.mnemonic, "MOV")
        self.assertEqual(ins.reg1, 5)
        self.assertEqual(ins.reg2, 3)
        self.assertEqual(ins.size, 2)
        self.assertFalse(ins.is_unknown)

    def test_add_r1_r2(self):
        buf = _enc_fmt_i(0x01, 2, 1)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "ADD")
        self.assertEqual(ins.reg2, 2)
        self.assertEqual(ins.reg1, 1)

    def test_jmp_lp_is_return(self):
        # JMP [r31] — the canonical V810 return idiom
        buf = _enc_fmt_i(0x06, 0, 31)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "JMP")
        self.assertEqual(ins.reg1, 31)
        self.assertTrue(ins.is_jump)
        self.assertTrue(ins.is_return)

    def test_not(self):
        buf = _enc_fmt_i(0x0F, 4, 7)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "NOT")


class TestFormatII(unittest.TestCase):
    def test_mov_imm5_positive(self):
        # MOV #5, r2
        buf = _enc_fmt_ii(0x10, 2, 5)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.II)
        self.assertEqual(ins.mnemonic, "MOV")
        self.assertEqual(ins.reg2, 2)
        self.assertEqual(ins.imm5, 5)
        self.assertEqual(ins.imm5_s, 5)

    def test_add_imm5_negative(self):
        # ADD #-3, r4   imm5=-3 → 5-bit two's complement = 0x1D
        buf = _enc_fmt_ii(0x11, 4, -3)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "ADD")
        self.assertEqual(ins.imm5, 0x1D)
        self.assertEqual(ins.imm5_s, -3)

    def test_halt(self):
        buf = _enc_fmt_ii(0x1A, 0, 0)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "HALT")

    def test_trap_vector(self):
        # TRAP #0x10
        buf = _enc_fmt_ii(0x18, 0, 0x10)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "TRAP")
        self.assertEqual(ins.imm5, 0x10)

    def test_cli_clears_interrupt_disable(self):
        # Sacred Tech Scroll: opcode 0x16 is CLI Format II (ID := 0).
        # Mario's Tennis triggered 2693 of these — they're NOT data.
        buf = _enc_fmt_ii(0x16, 0, 0)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "CLI")
        self.assertFalse(ins.is_unknown)

    def test_sei_sets_interrupt_disable(self):
        # Sacred Tech Scroll: opcode 0x1E is SEI Format II (ID := 1).
        buf = _enc_fmt_ii(0x1E, 0, 0)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "SEI")
        self.assertFalse(ins.is_unknown)


class TestFormatIIIBcond(unittest.TestCase):
    def test_br_positive_disp(self):
        # BR +4  (skip the next 4 bytes)
        buf = _enc_bcond(0x5, 4)
        ins = decode_at(buf, 0, pc=0x07001000)
        self.assertEqual(ins.fmt, Format.III)
        self.assertEqual(ins.mnemonic, "BR")
        self.assertEqual(ins.cond, 0x5)
        self.assertEqual(ins.disp9_s, 4)
        self.assertEqual(ins.branch_target, 0x07001004)
        self.assertTrue(ins.is_branch)

    def test_be_negative_disp(self):
        # BE -2   (tight self-loop)
        buf = _enc_bcond(0x2, -2)
        ins = decode_at(buf, 0, pc=0x100)
        self.assertEqual(ins.mnemonic, "BE")
        self.assertEqual(ins.disp9_s, -2)
        self.assertEqual(ins.branch_target, 0xFE)

    def test_nop_no_branch_target(self):
        # cond 0xD encodes NOP — never branches
        buf = _enc_bcond(0xD, 0)
        ins = decode_at(buf, 0, pc=0x100)
        self.assertEqual(ins.mnemonic, "NOP")
        self.assertIsNone(ins.branch_target)
        self.assertFalse(ins.is_branch)


class TestFormatIVJump(unittest.TestCase):
    def test_jr_forward(self):
        buf = _enc_fmt_iv(0x2A, +0x1000)
        ins = decode_at(buf, 0, pc=0x07000000)
        self.assertEqual(ins.fmt, Format.IV)
        self.assertEqual(ins.mnemonic, "JR")
        self.assertEqual(ins.size, 4)
        self.assertEqual(ins.disp26_s, 0x1000)
        self.assertEqual(ins.branch_target, 0x07001000)
        self.assertTrue(ins.is_jump)
        self.assertFalse(ins.is_call)

    def test_jal_marks_call(self):
        buf = _enc_fmt_iv(0x2B, +0x40)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "JAL")
        self.assertTrue(ins.is_call)
        self.assertEqual(ins.branch_target, 0x40)

    def test_jr_back_branch(self):
        buf = _enc_fmt_iv(0x2A, -0x100)
        ins = decode_at(buf, 0, pc=0x1000)
        self.assertEqual(ins.disp26_s, -0x100)
        self.assertEqual(ins.branch_target, 0xF00)


class TestFormatV(unittest.TestCase):
    def test_movhi_loads_wram_base(self):
        # MOVHI #0x0500, r0, r1  →  r1 = 0x05000000 (WRAM base)
        buf = _enc_fmt_v(0x2F, reg2=1, reg1=0, imm16=0x0500)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.V)
        self.assertEqual(ins.mnemonic, "MOVHI")
        self.assertEqual(ins.reg2, 1)
        self.assertEqual(ins.reg1, 0)
        self.assertEqual(ins.imm16, 0x0500)

    def test_movea(self):
        buf = _enc_fmt_v(0x28, reg2=2, reg1=1, imm16=0x1234)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "MOVEA")

    def test_addi_negative_imm(self):
        buf = _enc_fmt_v(0x29, reg2=3, reg1=2, imm16=0xFFF0)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "ADDI")
        self.assertEqual(ins.imm16, 0xFFF0)
        self.assertEqual(ins.imm16_s, -16)


class TestFormatVI(unittest.TestCase):
    def test_ld_w_negative_disp(self):
        # LD.W -4[r1], r2
        buf = _enc_fmt_vi(0x33, reg2=2, reg1=1, disp16=-4)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.VI)
        self.assertEqual(ins.mnemonic, "LD.W")
        self.assertEqual(ins.reg2, 2)
        self.assertEqual(ins.reg1, 1)
        self.assertEqual(ins.imm16_s, -4)
        self.assertTrue(ins.is_load)
        self.assertFalse(ins.is_store)

    def test_st_h(self):
        buf = _enc_fmt_vi(0x35, reg2=4, reg1=29, disp16=8)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "ST.H")
        self.assertTrue(ins.is_store)
        self.assertFalse(ins.is_load)

    def test_in_b(self):
        buf = _enc_fmt_vi(0x38, reg2=5, reg1=0, disp16=0)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.mnemonic, "IN.B")
        self.assertTrue(ins.is_load)

    def test_caxi_is_format_vi(self):
        # Sacred Tech Scroll classifies CAXI as Format VI (not VII as
        # earlier community references sometimes do). Mario's Tennis
        # used CAXI 3180 times so this matters.
        buf = _enc_fmt_vi(0x3A, reg2=3, reg1=2, disp16=0x10)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.VI)
        self.assertEqual(ins.mnemonic, "CAXI")
        self.assertEqual(ins.reg2, 3)
        self.assertEqual(ins.reg1, 2)
        self.assertEqual(ins.imm16_s, 0x10)


class TestFormatVIIFpp(unittest.TestCase):
    def test_addf_s(self):
        buf = _enc_fpp(reg2=3, reg1=4, subop=0x04)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.VII)
        self.assertEqual(ins.mnemonic, "ADDF.S")
        self.assertEqual(ins.subop, 0x04)
        self.assertFalse(ins.is_unknown)

    def test_unknown_subop_is_flagged(self):
        # 0x20 is not in FPP_SUBOP — must surface as unknown, never guessed.
        buf = _enc_fpp(reg2=0, reg1=0, subop=0x20)
        ins = decode_at(buf, 0, pc=0)
        self.assertTrue(ins.is_unknown)
        self.assertIn("FPP_UNK_", ins.mnemonic)
        self.assertEqual(ins.subop, 0x20)


class TestFormatVIIBsu(unittest.TestCase):
    def test_movbsu(self):
        # subop = 0x0B → MOVBSU
        buf = _enc_bsu(subop=0x0B)
        ins = decode_at(buf, 0, pc=0)
        self.assertEqual(ins.fmt, Format.VII)
        self.assertEqual(ins.mnemonic, "MOVBSU")
        self.assertEqual(ins.size, 2)
        self.assertFalse(ins.is_unknown)

    def test_unknown_bsu_subop(self):
        buf = _enc_bsu(subop=0x1F)   # not in BSU_SUBOP map
        ins = decode_at(buf, 0, pc=0)
        self.assertTrue(ins.is_unknown)
        self.assertIn("BSU_UNK_", ins.mnemonic)


class TestReservedAndTruncation(unittest.TestCase):
    def test_reserved_primary_opcode(self):
        # 0x1B is the one genuinely-reserved slot per the Sacred Tech Scroll.
        hw = 0x1B << 10
        buf = bytes([hw & 0xFF, (hw >> 8) & 0xFF])
        ins = decode_at(buf, 0, pc=0)
        self.assertTrue(ins.is_unknown)
        self.assertIn("reserved", ins.mnemonic.lower())

    def test_truncated_second_halfword(self):
        # MOVHI needs 4 bytes; provide only 2.
        buf = _enc_fmt_v(0x2F, 1, 0, 0x0500)[:2]
        ins = decode_at(buf, 0, pc=0)
        self.assertTrue(ins.is_unknown)
        self.assertIn("truncated", ins.notes.lower())

    def test_eof(self):
        ins = decode_at(b"", 0, pc=0x123)
        self.assertTrue(ins.is_unknown)
        self.assertEqual(ins.pc, 0x123)


class TestScan(unittest.TestCase):
    def test_scan_advances_by_instruction_size(self):
        # MOV r1,r2 (2 bytes) ; MOVHI #0x0500, r0, r1 (4 bytes) ; HALT (2 bytes)
        buf = (
            _enc_fmt_i(0x00, 2, 1) +
            _enc_fmt_v(0x2F, 1, 0, 0x0500) +
            _enc_fmt_ii(0x1A, 0, 0)
        )
        insns = list(scan(buf, base_pc=0x07000000))
        self.assertEqual(len(insns), 3)
        self.assertEqual(insns[0].pc, 0x07000000)
        self.assertEqual(insns[0].mnemonic, "MOV")
        self.assertEqual(insns[1].pc, 0x07000002)
        self.assertEqual(insns[1].mnemonic, "MOVHI")
        self.assertEqual(insns[2].pc, 0x07000006)
        self.assertEqual(insns[2].mnemonic, "HALT")


if __name__ == "__main__":
    unittest.main()
