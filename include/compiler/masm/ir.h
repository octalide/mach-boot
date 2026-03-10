#ifndef MASM_IR_H
#define MASM_IR_H

#include "compiler/masm/operand.h"
#include <stdint.h>
#include <stddef.h>

// masm two-layer opcode architecture
//
// layer 1: portable IR (this file)
//   - MasmIrOpcode enum defines platform-independent three-operand instructions
//   - condition codes, float/unsigned variants encoded in meta byte
//   - emitted by lower.c from AST
//   - consumed by isel which lowers to target opcodes
//
// layer 2: target-specific opcodes (e.g., isa/x86_64/x86_64.h)
//   - MasmX86Opcode (and future ARM64, RISCV, etc.)
//   - machine-level instructions with target semantics (flags, register constraints)
//   - emitted by isel and ISA-specific inline asm parsers
//   - consumed by encode/elf for binary emission
//
// the MasmOpcodeKind discriminator on MasmInstruction indicates which layer
// an instruction belongs to. this avoids magic numeric offsets and ensures
// type-safe dispatch in the emit pipeline.

// portable masm ir opcodes (30-op spec)
typedef enum MasmIrOpcode
{
    // data movement
    MASM_IR_MOV       = 0,
    MASM_IR_LOAD      = 1,
    MASM_IR_STORE     = 2,
    MASM_IR_ADDR      = 3,   // address-of (was LEA)

    // arithmetic (meta cc=ALU_FLOAT for .f variants)
    MASM_IR_ADD       = 4,
    MASM_IR_SUB       = 5,
    MASM_IR_MUL       = 6,

    // division (meta cc=DIV_* selects variant)
    MASM_IR_DIV       = 7,

    // bitwise
    MASM_IR_AND       = 8,
    MASM_IR_OR        = 9,
    MASM_IR_XOR       = 10,
    MASM_IR_SHL       = 11,

    // shift right (meta cc=SHR_ARITHMETIC for .a variant)
    MASM_IR_SHR       = 12,

    // comparison (meta cc=condition, flag=1 for float)
    MASM_IR_CMP       = 13,

    // control flow
    MASM_IR_JMP       = 14,
    MASM_IR_BRANCH    = 15,  // was BEQ/BNE/BLT/etc
    MASM_IR_CALL      = 16,
    MASM_IR_RET       = 17,

    // type conversion (meta cc=CONV_* selects kind)
    MASM_IR_CONV      = 18,

    // pseudo/system
    MASM_IR_LABEL     = 19,
    MASM_IR_SYSCALL   = 20,

    // ABI
    MASM_IR_ARG       = 21,
    MASM_IR_PARAM     = 22,

    // atomics
    MASM_IR_ATOMIC_LOAD  = 23,
    MASM_IR_ATOMIC_STORE = 24,
    MASM_IR_ATOMIC_CAS   = 25,
    MASM_IR_ATOMIC_RMW   = 26,
    MASM_IR_FENCE        = 27,

    // trap and hints
    MASM_IR_TRAP      = 28,
    MASM_IR_HINT      = 29,

    MASM_IR_COUNT     = 30,

    // internal pseudo-ops (mach-boot only, not part of MASM spec)
    MASM_IR_NEG         = 100,
    MASM_IR_NOT         = 101,
    MASM_IR_STACK_FRAME = 102,
} MasmIrOpcode;

// condition codes (stored in meta cc field)
#define MASM_CC_NONE  0
#define MASM_CC_EQ    1
#define MASM_CC_NE    2
#define MASM_CC_LT    3
#define MASM_CC_LE    4
#define MASM_CC_GT    5
#define MASM_CC_GE    6
#define MASM_CC_ULT   7
#define MASM_CC_ULE   8
#define MASM_CC_UGT   9
#define MASM_CC_UGE   10

// ALU float flag (stored in cc field for add, sub, mul)
#define MASM_ALU_FLOAT 1

// division modes (stored in cc field for MASM_IR_DIV)
#define MASM_DIV_QUOT   0
#define MASM_DIV_REM    1
#define MASM_DIV_UQUOT  2
#define MASM_DIV_UREM   3
#define MASM_DIV_FLOAT  4

// shift-right modes (stored in cc field for MASM_IR_SHR)
#define MASM_SHR_LOGICAL    0
#define MASM_SHR_ARITHMETIC 1

// conversion kinds (stored in cc field for MASM_IR_CONV)
#define MASM_CONV_ZEXT   0
#define MASM_CONV_SEXT   1
#define MASM_CONV_TRUNC  2
#define MASM_CONV_ITOF   3
#define MASM_CONV_FTOI   4
#define MASM_CONV_FEXT   5
#define MASM_CONV_FTRUNC 6

// hint kinds (stored in cc field for MASM_IR_HINT)
#define MASM_HINT_PAUSE 0

// call flags (stored in cc field for MASM_IR_CALL)
#define MASM_CALL_TAIL 1

// memory orderings (stored in cc field for atomic ops and fence)
#define MASM_MO_RELAXED 0
#define MASM_MO_ACQUIRE 1
#define MASM_MO_RELEASE 2
#define MASM_MO_ACQ_REL 3
#define MASM_MO_SEQ_CST 4

// meta byte encoding: cc:4 | size_log2:3 | flag:1
// size_log2 is unused in mach-boot (sizes come from operands)
#define MASM_META(cc, flag) ((uint8_t)(((cc) << 4) | ((flag) & 1)))
#define MASM_META_CC(meta)  ((uint8_t)(((meta) >> 4) & 0xF))
#define MASM_META_FLAG(meta) ((uint8_t)((meta) & 1))

// opcode namespace discriminator
typedef enum MasmOpcodeKind
{
    MASM_OPCODE_IR = 0,   // portable IR opcodes (MasmIrOpcode)
    MASM_OPCODE_X86,      // x86_64 target opcodes (MasmX86Opcode)
    // future: MASM_OPCODE_ARM64, etc.
} MasmOpcodeKind;

// instruction structure
// used for both IR instructions and target-specific instructions
typedef struct MasmInstruction
{
    MasmOpcodeKind kind;          // which opcode namespace
    uint32_t       opcode;        // interpretation depends on kind
    MasmOperand   *operands;
    uint8_t        operand_count;
    uint8_t        meta;          // cc:4|size_log2:3|flag:1
} MasmInstruction;

// instruction builders (default to IR opcode kind)
MasmInstruction masm_inst_create(MasmOpcodeKind kind, uint32_t opcode, MasmOperand *operands, uint8_t count);
MasmInstruction masm_inst_0(uint32_t opcode);
MasmInstruction masm_inst_1(uint32_t opcode, MasmOperand op1);
MasmInstruction masm_inst_2(uint32_t opcode, MasmOperand op1, MasmOperand op2);
MasmInstruction masm_inst_3(uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3);
MasmInstruction masm_inst_4(uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3, MasmOperand op4);
void            masm_inst_destroy(MasmInstruction inst);

// builders with meta byte
MasmInstruction masm_inst_0m(uint32_t opcode, uint8_t meta);
MasmInstruction masm_inst_1m(uint32_t opcode, uint8_t meta, MasmOperand op1);
MasmInstruction masm_inst_2m(uint32_t opcode, uint8_t meta, MasmOperand op1, MasmOperand op2);
MasmInstruction masm_inst_3m(uint32_t opcode, uint8_t meta, MasmOperand op1, MasmOperand op2, MasmOperand op3);

// target-specific instruction builders (for use by isel)
MasmInstruction masm_inst_target_create(MasmOpcodeKind kind, uint32_t opcode, MasmOperand *operands, uint8_t count);
MasmInstruction masm_inst_target_0(MasmOpcodeKind kind, uint32_t opcode);
MasmInstruction masm_inst_target_1(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1);
MasmInstruction masm_inst_target_2(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2);
MasmInstruction masm_inst_target_3(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3);
MasmInstruction masm_inst_target_4(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3, MasmOperand op4);

// helper to get string representation of ir opcode
const char *masm_ir_name(MasmIrOpcode op);

#endif // MASM_IR_H
