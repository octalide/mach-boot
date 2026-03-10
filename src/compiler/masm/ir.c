#include "compiler/masm/ir.h"
#include <stdlib.h>
#include <string.h>

// instruction builders

MasmInstruction masm_inst_create(MasmOpcodeKind kind, uint32_t opcode, MasmOperand *operands, uint8_t count)
{
    MasmInstruction inst;
    inst.kind = kind;
    inst.opcode = opcode;
    inst.operand_count = count;
    inst.meta = 0;

    if (count > 0)
    {
        inst.operands = malloc(sizeof(MasmOperand) * count);
        memcpy(inst.operands, operands, sizeof(MasmOperand) * count);

        // deep copy strings to avoid ownership issues
        for (int i = 0; i < count; i++)
        {
            if (inst.operands[i].kind == MASM_OPERAND_LABEL && inst.operands[i].label)
            {
                inst.operands[i].label = strdup(inst.operands[i].label);
            }
            else if (inst.operands[i].kind == MASM_OPERAND_SYMBOL && inst.operands[i].symbol)
            {
                inst.operands[i].symbol = strdup(inst.operands[i].symbol);
            }
        }
    }
    else
    {
        inst.operands = NULL;
    }

    return inst;
}

// ir instruction builders (default to MASM_OPCODE_IR)

MasmInstruction masm_inst_0(uint32_t opcode)
{
    return masm_inst_create(MASM_OPCODE_IR, opcode, NULL, 0);
}

MasmInstruction masm_inst_1(uint32_t opcode, MasmOperand op1)
{
    return masm_inst_create(MASM_OPCODE_IR, opcode, &op1, 1);
}

MasmInstruction masm_inst_2(uint32_t opcode, MasmOperand op1, MasmOperand op2)
{
    MasmOperand ops[] = {op1, op2};
    return masm_inst_create(MASM_OPCODE_IR, opcode, ops, 2);
}

MasmInstruction masm_inst_3(uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3)
{
    MasmOperand ops[] = {op1, op2, op3};
    return masm_inst_create(MASM_OPCODE_IR, opcode, ops, 3);
}

MasmInstruction masm_inst_4(uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3, MasmOperand op4)
{
    MasmOperand ops[] = {op1, op2, op3, op4};
    return masm_inst_create(MASM_OPCODE_IR, opcode, ops, 4);
}

// builders with meta byte

MasmInstruction masm_inst_0m(uint32_t opcode, uint8_t meta)
{
    MasmInstruction inst = masm_inst_0(opcode);
    inst.meta = meta;
    return inst;
}

MasmInstruction masm_inst_1m(uint32_t opcode, uint8_t meta, MasmOperand op1)
{
    MasmInstruction inst = masm_inst_1(opcode, op1);
    inst.meta = meta;
    return inst;
}

MasmInstruction masm_inst_2m(uint32_t opcode, uint8_t meta, MasmOperand op1, MasmOperand op2)
{
    MasmInstruction inst = masm_inst_2(opcode, op1, op2);
    inst.meta = meta;
    return inst;
}

MasmInstruction masm_inst_3m(uint32_t opcode, uint8_t meta, MasmOperand op1, MasmOperand op2, MasmOperand op3)
{
    MasmInstruction inst = masm_inst_3(opcode, op1, op2, op3);
    inst.meta = meta;
    return inst;
}

// target-specific instruction builders

MasmInstruction masm_inst_target_create(MasmOpcodeKind kind, uint32_t opcode, MasmOperand *operands, uint8_t count)
{
    return masm_inst_create(kind, opcode, operands, count);
}

MasmInstruction masm_inst_target_0(MasmOpcodeKind kind, uint32_t opcode)
{
    return masm_inst_create(kind, opcode, NULL, 0);
}

MasmInstruction masm_inst_target_1(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1)
{
    return masm_inst_create(kind, opcode, &op1, 1);
}

MasmInstruction masm_inst_target_2(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2)
{
    MasmOperand ops[] = {op1, op2};
    return masm_inst_create(kind, opcode, ops, 2);
}

MasmInstruction masm_inst_target_3(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3)
{
    MasmOperand ops[] = {op1, op2, op3};
    return masm_inst_create(kind, opcode, ops, 3);
}

MasmInstruction masm_inst_target_4(MasmOpcodeKind kind, uint32_t opcode, MasmOperand op1, MasmOperand op2, MasmOperand op3, MasmOperand op4)
{
    MasmOperand ops[] = {op1, op2, op3, op4};
    return masm_inst_create(kind, opcode, ops, 4);
}

void masm_inst_destroy(MasmInstruction inst)
{
    if (inst.operands)
    {
        for (int i = 0; i < inst.operand_count; i++)
        {
            if (inst.operands[i].kind == MASM_OPERAND_LABEL && inst.operands[i].label)
            {
                free((void *)inst.operands[i].label);
            }
            else if (inst.operands[i].kind == MASM_OPERAND_SYMBOL && inst.operands[i].symbol)
            {
                free((void *)inst.operands[i].symbol);
            }
        }
        free(inst.operands);
    }
}

// ir opcode names

const char *masm_ir_name(MasmIrOpcode op)
{
    switch (op)
    {
    // data movement
    case MASM_IR_MOV:
        return "mov";
    case MASM_IR_LOAD:
        return "load";
    case MASM_IR_STORE:
        return "store";
    case MASM_IR_ADDR:
        return "addr";

    // arithmetic
    case MASM_IR_ADD:
        return "add";
    case MASM_IR_SUB:
        return "sub";
    case MASM_IR_MUL:
        return "mul";
    case MASM_IR_DIV:
        return "div";

    // bitwise
    case MASM_IR_AND:
        return "and";
    case MASM_IR_OR:
        return "or";
    case MASM_IR_XOR:
        return "xor";
    case MASM_IR_SHL:
        return "shl";
    case MASM_IR_SHR:
        return "shr";

    // comparison
    case MASM_IR_CMP:
        return "cmp";

    // control flow
    case MASM_IR_JMP:
        return "jmp";
    case MASM_IR_BRANCH:
        return "branch";
    case MASM_IR_CALL:
        return "call";
    case MASM_IR_RET:
        return "ret";

    // type conversion
    case MASM_IR_CONV:
        return "conv";

    // pseudo/system
    case MASM_IR_LABEL:
        return "label";
    case MASM_IR_SYSCALL:
        return "syscall";

    // ABI
    case MASM_IR_ARG:
        return "arg";
    case MASM_IR_PARAM:
        return "param";

    // atomics
    case MASM_IR_ATOMIC_LOAD:
        return "atomic.load";
    case MASM_IR_ATOMIC_STORE:
        return "atomic.store";
    case MASM_IR_ATOMIC_CAS:
        return "atomic.cas";
    case MASM_IR_ATOMIC_RMW:
        return "atomic.rmw";
    case MASM_IR_FENCE:
        return "fence";

    // trap and hints
    case MASM_IR_TRAP:
        return "trap";
    case MASM_IR_HINT:
        return "hint";

    // internal pseudo-ops (mach-boot only)
    case MASM_IR_NEG:
        return "neg";
    case MASM_IR_NOT:
        return "not";
    case MASM_IR_STACK_FRAME:
        return "stack_frame";

    default:
        return "unknown";
    }
}
