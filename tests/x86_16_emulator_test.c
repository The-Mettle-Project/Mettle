#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_BYTES (1u << 20)
#define LOAD_ADDRESS 0x7C00u
#define OUTPUT_MAX 4096
#define STEP_LIMIT 20000000

enum { REG_AX, REG_CX, REG_DX, REG_BX, REG_SP, REG_BP, REG_SI, REG_DI };
enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };

typedef struct {
  uint16_t reg[8];
  uint16_t seg[6];
  uint16_t ip;
  int carry;
  int zero;
  int sign;
  int overflow;
  int direction;
  int interrupts;
  int halted;
  int fault;
  const char *fault_reason;
} Cpu;

static unsigned char memory[MEMORY_BYTES];
static char output[OUTPUT_MAX];
static size_t output_length;

static void emit_output(char c) {
  if (output_length + 1 < sizeof(output)) {
    output[output_length++] = c;
    output[output_length] = '\0';
  }
}

static void fault(Cpu *cpu, const char *reason) {
  if (!cpu->fault) {
    cpu->fault = 1;
    cpu->fault_reason = reason;
  }
}

static uint32_t linear(uint16_t segment, uint16_t offset) {
  return (((uint32_t)segment << 4) + offset) & (MEMORY_BYTES - 1);
}

static uint8_t read8(uint32_t address) { return memory[address & (MEMORY_BYTES - 1)]; }

static uint16_t read16(uint32_t address) {
  return (uint16_t)(read8(address) | ((uint16_t)read8(address + 1) << 8));
}

static void write8(uint32_t address, uint8_t value) {
  memory[address & (MEMORY_BYTES - 1)] = value;
}

static void write16(uint32_t address, uint16_t value) {
  write8(address, (uint8_t)(value & 0xFF));
  write8(address + 1, (uint8_t)(value >> 8));
}

static uint8_t fetch8(Cpu *cpu) {
  uint8_t value = read8(linear(cpu->seg[SEG_CS], cpu->ip));
  cpu->ip++;
  return value;
}

static uint16_t fetch16(Cpu *cpu) {
  uint16_t low = fetch8(cpu);
  uint16_t high = fetch8(cpu);
  return (uint16_t)(low | (high << 8));
}

static uint16_t get_reg8(const Cpu *cpu, int index) {
  return index < 4 ? (uint8_t)(cpu->reg[index] & 0xFF)
                   : (uint8_t)(cpu->reg[index - 4] >> 8);
}

static void set_reg8(Cpu *cpu, int index, uint8_t value) {
  if (index < 4) {
    cpu->reg[index] = (uint16_t)((cpu->reg[index] & 0xFF00) | value);
  } else {
    cpu->reg[index - 4] =
        (uint16_t)((cpu->reg[index - 4] & 0x00FF) | ((uint16_t)value << 8));
  }
}

typedef struct {
  int is_register;
  int reg;
  uint32_t address;
} Operand;

static Operand decode_modrm(Cpu *cpu, int *reg_field, uint16_t segment) {
  uint8_t modrm = fetch8(cpu);
  int mod = modrm >> 6;
  int rm = modrm & 7;
  Operand operand;
  uint16_t base = 0;
  uint16_t displacement = 0;

  *reg_field = (modrm >> 3) & 7;
  memset(&operand, 0, sizeof(operand));

  if (mod == 3) {
    operand.is_register = 1;
    operand.reg = rm;
    return operand;
  }

  switch (rm) {
  case 0: base = (uint16_t)(cpu->reg[REG_BX] + cpu->reg[REG_SI]); break;
  case 1: base = (uint16_t)(cpu->reg[REG_BX] + cpu->reg[REG_DI]); break;
  case 2: base = (uint16_t)(cpu->reg[REG_BP] + cpu->reg[REG_SI]); break;
  case 3: base = (uint16_t)(cpu->reg[REG_BP] + cpu->reg[REG_DI]); break;
  case 4: base = cpu->reg[REG_SI]; break;
  case 5: base = cpu->reg[REG_DI]; break;
  case 6: base = mod == 0 ? 0 : cpu->reg[REG_BP]; break;
  default: base = cpu->reg[REG_BX]; break;
  }

  if (mod == 0 && rm == 6) {
    displacement = fetch16(cpu);
  } else if (mod == 1) {
    displacement = (uint16_t)(int16_t)(int8_t)fetch8(cpu);
  } else if (mod == 2) {
    displacement = fetch16(cpu);
  }

  if (segment == 0xFFFF) {
    segment = (rm == 2 || rm == 3 || (rm == 6 && mod != 0)) ? cpu->seg[SEG_SS]
                                                            : cpu->seg[SEG_DS];
  }
  operand.address = linear(segment, (uint16_t)(base + displacement));
  return operand;
}

static uint16_t read_operand16(const Cpu *cpu, const Operand *operand) {
  return operand->is_register ? cpu->reg[operand->reg] : read16(operand->address);
}

static void write_operand16(Cpu *cpu, const Operand *operand, uint16_t value) {
  if (operand->is_register) {
    cpu->reg[operand->reg] = value;
  } else {
    write16(operand->address, value);
  }
}

static uint8_t read_operand8(const Cpu *cpu, const Operand *operand) {
  return operand->is_register ? (uint8_t)get_reg8(cpu, operand->reg)
                              : read8(operand->address);
}

static void write_operand8(Cpu *cpu, const Operand *operand, uint8_t value) {
  if (operand->is_register) {
    set_reg8(cpu, operand->reg, value);
  } else {
    write8(operand->address, value);
  }
}

static void set_logic_flags(Cpu *cpu, uint32_t result, int width) {
  uint32_t mask = width == 1 ? 0xFFu : 0xFFFFu;
  cpu->zero = (result & mask) == 0;
  cpu->sign = (result & (width == 1 ? 0x80u : 0x8000u)) != 0;
  cpu->carry = 0;
  cpu->overflow = 0;
}

static uint32_t alu(Cpu *cpu, int op, uint32_t a, uint32_t b, int width) {
  uint32_t mask = width == 1 ? 0xFFu : 0xFFFFu;
  uint32_t sign_bit = width == 1 ? 0x80u : 0x8000u;
  uint32_t result = 0;

  switch (op) {
  case 0: result = a + b; break;
  case 1: result = a | b; break;
  case 2: result = a + b + (uint32_t)(cpu->carry ? 1 : 0); break;
  case 3: result = a - b - (uint32_t)(cpu->carry ? 1 : 0); break;
  case 4: result = a & b; break;
  case 5: result = a - b; break;
  case 6: result = a ^ b; break;
  default: result = a - b; break;
  }

  if (op == 1 || op == 4 || op == 6) {
    set_logic_flags(cpu, result, width);
    return result & mask;
  }

  cpu->zero = (result & mask) == 0;
  cpu->sign = (result & sign_bit) != 0;
  if (op == 0 || op == 2) {
    cpu->carry = (result & ~mask) != 0;
    cpu->overflow = (((a ^ result) & (b ^ result)) & sign_bit) != 0;
  } else {
    cpu->carry = (a & mask) < (b & mask);
    cpu->overflow = (((a ^ b) & (a ^ result)) & sign_bit) != 0;
  }
  return result & mask;
}

static int condition(const Cpu *cpu, int code) {
  switch (code) {
  case 0x0: return cpu->overflow;
  case 0x1: return !cpu->overflow;
  case 0x2: return cpu->carry;
  case 0x3: return !cpu->carry;
  case 0x4: return cpu->zero;
  case 0x5: return !cpu->zero;
  case 0x6: return cpu->carry || cpu->zero;
  case 0x7: return !cpu->carry && !cpu->zero;
  case 0x8: return cpu->sign;
  case 0x9: return !cpu->sign;
  case 0xA: return 0;
  case 0xB: return 1;
  case 0xC: return cpu->sign != cpu->overflow;
  case 0xD: return cpu->sign == cpu->overflow;
  case 0xE: return cpu->zero || cpu->sign != cpu->overflow;
  default: return !cpu->zero && cpu->sign == cpu->overflow;
  }
}

static void push16(Cpu *cpu, uint16_t value) {
  cpu->reg[REG_SP] = (uint16_t)(cpu->reg[REG_SP] - 2);
  write16(linear(cpu->seg[SEG_SS], cpu->reg[REG_SP]), value);
}

static uint16_t pop16(Cpu *cpu) {
  uint16_t value = read16(linear(cpu->seg[SEG_SS], cpu->reg[REG_SP]));
  cpu->reg[REG_SP] = (uint16_t)(cpu->reg[REG_SP] + 2);
  return value;
}

static uint16_t shift(Cpu *cpu, int op, uint16_t value, int count, int width) {
  uint32_t mask = width == 1 ? 0xFFu : 0xFFFFu;
  uint32_t sign_bit = width == 1 ? 0x80u : 0x8000u;
  uint32_t result = value & mask;
  int i;

  count &= 0x1F;
  for (i = 0; i < count; i++) {
    switch (op) {
    case 4:
      cpu->carry = (result & sign_bit) != 0;
      result = (result << 1) & mask;
      break;
    case 5:
      cpu->carry = (result & 1u) != 0;
      result >>= 1;
      break;
    case 7:
      cpu->carry = (result & 1u) != 0;
      result = ((result >> 1) | (result & sign_bit)) & mask;
      break;
    case 0:
      cpu->carry = (result & sign_bit) != 0;
      result = ((result << 1) | (cpu->carry ? 1u : 0u)) & mask;
      break;
    case 1:
      cpu->carry = (result & 1u) != 0;
      result = ((result >> 1) | (cpu->carry ? sign_bit : 0u)) & mask;
      break;
    default:
      return (uint16_t)result;
    }
  }
  if (count) {
    cpu->zero = (result & mask) == 0;
    cpu->sign = (result & sign_bit) != 0;
  }
  return (uint16_t)result;
}

static uint16_t pack_flags(const Cpu *cpu) {
  return (uint16_t)((cpu->carry ? 0x0001u : 0u) |
                    (cpu->zero ? 0x0040u : 0u) |
                    (cpu->sign ? 0x0080u : 0u) |
                    (cpu->direction ? 0x0400u : 0u) |
                    (cpu->interrupts ? 0x0200u : 0u) |
                    (cpu->overflow ? 0x0800u : 0u) | 0x0002u);
}

static void unpack_flags(Cpu *cpu, uint16_t value) {
  cpu->carry = (value & 0x0001u) != 0;
  cpu->zero = (value & 0x0040u) != 0;
  cpu->sign = (value & 0x0080u) != 0;
  cpu->direction = (value & 0x0400u) != 0;
  cpu->interrupts = (value & 0x0200u) != 0;
  cpu->overflow = (value & 0x0800u) != 0;
}

static void software_interrupt(Cpu *cpu, uint8_t vector) {
  uint16_t offset;
  uint16_t segment;
  if (vector == 0x10) {
    if (get_reg8(cpu, 4) == 0x0E) {
      emit_output((char)get_reg8(cpu, 0));
      return;
    }
    fault(cpu, "unsupported int 0x10 function");
    return;
  }
  offset = read16((uint32_t)vector * 4u);
  segment = read16((uint32_t)vector * 4u + 2u);
  if (offset == 0 && segment == 0) {
    fault(cpu, "interrupt vector is empty");
    return;
  }
  push16(cpu, pack_flags(cpu));
  push16(cpu, cpu->seg[SEG_CS]);
  push16(cpu, cpu->ip);
  cpu->interrupts = 0;
  cpu->seg[SEG_CS] = segment;
  cpu->ip = offset;
}

static void step(Cpu *cpu) {
  uint8_t opcode;
  uint16_t segment_override = 0xFFFF;

  for (;;) {
    opcode = fetch8(cpu);
    if (opcode == 0x26) { segment_override = cpu->seg[SEG_ES]; continue; }
    if (opcode == 0x2E) { segment_override = cpu->seg[SEG_CS]; continue; }
    if (opcode == 0x36) { segment_override = cpu->seg[SEG_SS]; continue; }
    if (opcode == 0x3E) { segment_override = cpu->seg[SEG_DS]; continue; }
    break;
  }

  if (opcode < 0x40 && (opcode & 7) < 6 && (opcode & 0x38) >> 3 <= 7 &&
      (opcode & 7) <= 5) {
    int op = (opcode >> 3) & 7;
    int form = opcode & 7;
    int width = (form & 1) ? 2 : 1;
    int reg_field = 0;
    Operand operand;

    if (form <= 1) {
      operand = decode_modrm(cpu, &reg_field, segment_override);
      if (width == 1) {
        uint8_t result = (uint8_t)alu(cpu, op, read_operand8(cpu, &operand),
                                      get_reg8(cpu, reg_field), 1);
        if (op != 7) {
          write_operand8(cpu, &operand, result);
        }
      } else {
        uint16_t result = (uint16_t)alu(cpu, op, read_operand16(cpu, &operand),
                                        cpu->reg[reg_field], 2);
        if (op != 7) {
          write_operand16(cpu, &operand, result);
        }
      }
      return;
    }
    if (form <= 3) {
      operand = decode_modrm(cpu, &reg_field, segment_override);
      if (width == 1) {
        uint8_t result = (uint8_t)alu(cpu, op, get_reg8(cpu, reg_field),
                                      read_operand8(cpu, &operand), 1);
        if (op != 7) {
          set_reg8(cpu, reg_field, result);
        }
      } else {
        uint16_t result = (uint16_t)alu(cpu, op, cpu->reg[reg_field],
                                        read_operand16(cpu, &operand), 2);
        if (op != 7) {
          cpu->reg[reg_field] = result;
        }
      }
      return;
    }
    if (width == 1) {
      uint8_t result = (uint8_t)alu(cpu, op, get_reg8(cpu, 0), fetch8(cpu), 1);
      if (op != 7) {
        set_reg8(cpu, 0, result);
      }
    } else {
      uint16_t result = (uint16_t)alu(cpu, op, cpu->reg[REG_AX], fetch16(cpu), 2);
      if (op != 7) {
        cpu->reg[REG_AX] = result;
      }
    }
    return;
  }

  if (opcode >= 0x40 && opcode <= 0x4F) {
    int index = opcode & 7;
    int saved_carry = cpu->carry;
    uint16_t result = (uint16_t)alu(cpu, opcode < 0x48 ? 0 : 5,
                                    cpu->reg[index], 1, 2);
    cpu->reg[index] = result;
    cpu->carry = saved_carry;
    return;
  }
  if (opcode >= 0x50 && opcode <= 0x57) {
    push16(cpu, cpu->reg[opcode & 7]);
    return;
  }
  if (opcode >= 0x58 && opcode <= 0x5F) {
    cpu->reg[opcode & 7] = pop16(cpu);
    return;
  }
  if (opcode >= 0x70 && opcode <= 0x7F) {
    int8_t displacement = (int8_t)fetch8(cpu);
    if (condition(cpu, opcode & 0xF)) {
      cpu->ip = (uint16_t)(cpu->ip + displacement);
    }
    return;
  }
  if (opcode >= 0xB0 && opcode <= 0xB7) {
    set_reg8(cpu, opcode & 7, fetch8(cpu));
    return;
  }
  if (opcode >= 0xB8 && opcode <= 0xBF) {
    cpu->reg[opcode & 7] = fetch16(cpu);
    return;
  }

  switch (opcode) {
  case 0x80:
  case 0x81:
  case 0x83: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0x80) {
      uint8_t immediate = fetch8(cpu);
      uint8_t result =
          (uint8_t)alu(cpu, reg_field, read_operand8(cpu, &operand), immediate, 1);
      if (reg_field != 7) {
        write_operand8(cpu, &operand, result);
      }
      return;
    }
    {
      uint16_t immediate = opcode == 0x81
                               ? fetch16(cpu)
                               : (uint16_t)(int16_t)(int8_t)fetch8(cpu);
      uint16_t result = (uint16_t)alu(cpu, reg_field,
                                      read_operand16(cpu, &operand), immediate, 2);
      if (reg_field != 7) {
        write_operand16(cpu, &operand, result);
      }
    }
    return;
  }
  case 0x84:
  case 0x85: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0x84) {
      set_logic_flags(cpu, (uint32_t)(read_operand8(cpu, &operand) &
                                      get_reg8(cpu, reg_field)), 1);
    } else {
      set_logic_flags(cpu, (uint32_t)(read_operand16(cpu, &operand) &
                                      cpu->reg[reg_field]), 2);
    }
    return;
  }
  case 0x86:
  case 0x87: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0x86) {
      uint8_t a = read_operand8(cpu, &operand);
      uint8_t b = (uint8_t)get_reg8(cpu, reg_field);
      write_operand8(cpu, &operand, b);
      set_reg8(cpu, reg_field, a);
    } else {
      uint16_t a = read_operand16(cpu, &operand);
      uint16_t b = cpu->reg[reg_field];
      write_operand16(cpu, &operand, b);
      cpu->reg[reg_field] = a;
    }
    return;
  }
  case 0x88:
  case 0x89:
  case 0x8A:
  case 0x8B: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    switch (opcode) {
    case 0x88: write_operand8(cpu, &operand, (uint8_t)get_reg8(cpu, reg_field)); break;
    case 0x89: write_operand16(cpu, &operand, cpu->reg[reg_field]); break;
    case 0x8A: set_reg8(cpu, reg_field, read_operand8(cpu, &operand)); break;
    default: cpu->reg[reg_field] = read_operand16(cpu, &operand); break;
    }
    return;
  }
  case 0x8C:
  case 0x8E: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0x8C) {
      write_operand16(cpu, &operand, cpu->seg[reg_field]);
    } else {
      cpu->seg[reg_field] = read_operand16(cpu, &operand);
    }
    return;
  }
  case 0x8D: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, 0);
    if (operand.is_register) {
      fault(cpu, "lea with a register source");
      return;
    }
    cpu->reg[reg_field] = (uint16_t)operand.address;
    return;
  }
  case 0x8F: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    write_operand16(cpu, &operand, pop16(cpu));
    return;
  }
  case 0x90:
    return;
  case 0x98: {
    uint8_t low = (uint8_t)get_reg8(cpu, 0);
    cpu->reg[REG_AX] = (uint16_t)(int16_t)(int8_t)low;
    return;
  }
  case 0x99:
    cpu->reg[REG_DX] = (cpu->reg[REG_AX] & 0x8000u) ? 0xFFFFu : 0x0000u;
    return;
  case 0xC2: {
    uint16_t bytes = fetch16(cpu);
    cpu->ip = pop16(cpu);
    cpu->reg[REG_SP] = (uint16_t)(cpu->reg[REG_SP] + bytes);
    return;
  }
  case 0xC3:
    cpu->ip = pop16(cpu);
    return;
  case 0xC6:
  case 0xC7: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0xC6) {
      write_operand8(cpu, &operand, fetch8(cpu));
    } else {
      write_operand16(cpu, &operand, fetch16(cpu));
    }
    return;
  }
  case 0xC9:
    cpu->reg[REG_SP] = cpu->reg[REG_BP];
    cpu->reg[REG_BP] = pop16(cpu);
    return;
  case 0xCD:
    software_interrupt(cpu, fetch8(cpu));
    return;
  case 0xCF:
    cpu->ip = pop16(cpu);
    cpu->seg[SEG_CS] = pop16(cpu);
    unpack_flags(cpu, pop16(cpu));
    return;
  case 0x9C:
    push16(cpu, pack_flags(cpu));
    return;
  case 0x9D:
    unpack_flags(cpu, pop16(cpu));
    return;
  case 0x06:
    push16(cpu, cpu->seg[SEG_ES]);
    return;
  case 0x07:
    cpu->seg[SEG_ES] = pop16(cpu);
    return;
  case 0x0E:
    push16(cpu, cpu->seg[SEG_CS]);
    return;
  case 0x16:
    push16(cpu, cpu->seg[SEG_SS]);
    return;
  case 0x17:
    cpu->seg[SEG_SS] = pop16(cpu);
    return;
  case 0x1E:
    push16(cpu, cpu->seg[SEG_DS]);
    return;
  case 0x1F:
    cpu->seg[SEG_DS] = pop16(cpu);
    return;
  case 0xFC:
    cpu->direction = 0;
    return;
  case 0xFD:
    cpu->direction = 1;
    return;
  case 0xC0:
  case 0xC1:
  case 0xD0:
  case 0xD1:
  case 0xD2:
  case 0xD3: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    int width = (opcode & 1) ? 2 : 1;
    int count = 1;
    if (opcode == 0xC0 || opcode == 0xC1) {
      count = fetch8(cpu);
    } else if (opcode == 0xD2 || opcode == 0xD3) {
      count = (int)get_reg8(cpu, 1);
    }
    if (width == 1) {
      write_operand8(cpu, &operand,
                     (uint8_t)shift(cpu, reg_field, read_operand8(cpu, &operand),
                                    count, 1));
    } else {
      write_operand16(cpu, &operand,
                      shift(cpu, reg_field, read_operand16(cpu, &operand), count,
                            2));
    }
    return;
  }
  case 0xE8: {
    int16_t displacement = (int16_t)fetch16(cpu);
    push16(cpu, cpu->ip);
    cpu->ip = (uint16_t)(cpu->ip + displacement);
    return;
  }
  case 0xE9: {
    int16_t displacement = (int16_t)fetch16(cpu);
    cpu->ip = (uint16_t)(cpu->ip + displacement);
    return;
  }
  case 0xEB: {
    int8_t displacement = (int8_t)fetch8(cpu);
    cpu->ip = (uint16_t)(cpu->ip + displacement);
    return;
  }
  case 0xEA: {
    uint16_t offset = fetch16(cpu);
    uint16_t selector = fetch16(cpu);
    cpu->ip = offset;
    cpu->seg[SEG_CS] = selector;
    return;
  }
  case 0xF4:
    cpu->halted = 1;
    return;
  case 0xF8: cpu->carry = 0; return;
  case 0xF9: cpu->carry = 1; return;
  case 0xFA:
    cpu->interrupts = 0;
    return;
  case 0xFB:
    cpu->interrupts = 1;
    return;
  case 0xF6:
  case 0xF7: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    int width = opcode == 0xF6 ? 1 : 2;
    if (reg_field == 0) {
      if (width == 1) {
        set_logic_flags(cpu, (uint32_t)(read_operand8(cpu, &operand) & fetch8(cpu)), 1);
      } else {
        set_logic_flags(cpu, (uint32_t)(read_operand16(cpu, &operand) & fetch16(cpu)), 2);
      }
      return;
    }
    if (width == 1) {
      uint8_t value = read_operand8(cpu, &operand);
      switch (reg_field) {
      case 2: write_operand8(cpu, &operand, (uint8_t)~value); return;
      case 3: write_operand8(cpu, &operand, (uint8_t)alu(cpu, 5, 0, value, 1)); return;
      case 4: cpu->reg[REG_AX] = (uint16_t)((uint16_t)get_reg8(cpu, 0) * value); return;
      case 5:
        cpu->reg[REG_AX] =
            (uint16_t)((int16_t)(int8_t)get_reg8(cpu, 0) * (int16_t)(int8_t)value);
        return;
      default: fault(cpu, "unsupported byte group-3 form"); return;
      }
    }
    {
      uint16_t value = read_operand16(cpu, &operand);
      uint32_t wide = ((uint32_t)cpu->reg[REG_DX] << 16) | cpu->reg[REG_AX];
      switch (reg_field) {
      case 2: write_operand16(cpu, &operand, (uint16_t)~value); return;
      case 3: write_operand16(cpu, &operand, (uint16_t)alu(cpu, 5, 0, value, 2)); return;
      case 4: {
        uint32_t product = (uint32_t)cpu->reg[REG_AX] * value;
        cpu->reg[REG_AX] = (uint16_t)product;
        cpu->reg[REG_DX] = (uint16_t)(product >> 16);
        return;
      }
      case 5: {
        int32_t product = (int32_t)(int16_t)cpu->reg[REG_AX] * (int16_t)value;
        cpu->reg[REG_AX] = (uint16_t)product;
        cpu->reg[REG_DX] = (uint16_t)((uint32_t)product >> 16);
        return;
      }
      case 6:
        if (!value) { fault(cpu, "divide by zero"); return; }
        cpu->reg[REG_AX] = (uint16_t)(wide / value);
        cpu->reg[REG_DX] = (uint16_t)(wide % value);
        return;
      case 7: {
        int32_t dividend = (int32_t)wide;
        if (!value) { fault(cpu, "divide by zero"); return; }
        cpu->reg[REG_AX] = (uint16_t)(dividend / (int16_t)value);
        cpu->reg[REG_DX] = (uint16_t)(dividend % (int16_t)value);
        return;
      }
      default: fault(cpu, "unsupported word group-3 form"); return;
      }
    }
  }
  case 0xFE:
  case 0xFF: {
    int reg_field = 0;
    Operand operand = decode_modrm(cpu, &reg_field, segment_override);
    if (opcode == 0xFE) {
      int saved_carry = cpu->carry;
      uint8_t value = read_operand8(cpu, &operand);
      write_operand8(cpu, &operand,
                     (uint8_t)alu(cpu, reg_field == 0 ? 0 : 5, value, 1, 1));
      cpu->carry = saved_carry;
      return;
    }
    switch (reg_field) {
    case 0:
    case 1: {
      int saved_carry = cpu->carry;
      uint16_t value = read_operand16(cpu, &operand);
      write_operand16(cpu, &operand,
                      (uint16_t)alu(cpu, reg_field == 0 ? 0 : 5, value, 1, 2));
      cpu->carry = saved_carry;
      return;
    }
    case 2:
      push16(cpu, cpu->ip);
      cpu->ip = read_operand16(cpu, &operand);
      return;
    case 4:
      cpu->ip = read_operand16(cpu, &operand);
      return;
    case 6:
      push16(cpu, read_operand16(cpu, &operand));
      return;
    default:
      fault(cpu, "unsupported group-5 form");
      return;
    }
  }
  case 0x68:
    push16(cpu, fetch16(cpu));
    return;
  case 0x6A:
    push16(cpu, (uint16_t)(int16_t)(int8_t)fetch8(cpu));
    return;
  default:
    fault(cpu, "unimplemented opcode");
    return;
  }
}

static const char *unescape(const char *text, char *buffer, size_t size) {
  size_t out = 0;
  size_t i = 0;
  while (text[i] && out + 1 < size) {
    if (text[i] == '\\' && text[i + 1]) {
      i++;
      switch (text[i]) {
      case 'n': buffer[out++] = '\n'; break;
      case 'r': buffer[out++] = '\r'; break;
      case 't': buffer[out++] = '\t'; break;
      case '0': buffer[out++] = '\0'; break;
      default: buffer[out++] = text[i]; break;
      }
      i++;
      continue;
    }
    buffer[out++] = text[i++];
  }
  buffer[out] = '\0';
  return buffer;
}

static unsigned load_address = LOAD_ADDRESS;

int main(int argc, char **argv) {
  Cpu cpu;
  char expected_storage[OUTPUT_MAX];
  FILE *file;
  size_t size;
  long steps = 0;
  const char *expected;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <image.bin> <expected-output> [load-address]\n",
            argv[0]);
    return 2;
  }
  expected = unescape(argv[2], expected_storage, sizeof(expected_storage));
  if (argc > 3) {
    load_address = (unsigned)strtoul(argv[3], NULL, 0);
    if (load_address >= MEMORY_BYTES) {
      fprintf(stderr, "load address %s is outside the emulated memory\n",
              argv[3]);
      return 2;
    }
  }

  file = fopen(argv[1], "rb");
  if (!file) {
    fprintf(stderr, "could not open %s\n", argv[1]);
    return 2;
  }
  size = fread(memory + load_address, 1, MEMORY_BYTES - load_address, file);
  fclose(file);
  if (size < 2) {
    fprintf(stderr, "%s is too small to be an image\n", argv[1]);
    return 2;
  }
  if (size == 512 &&
      (memory[load_address + 510] != 0x55 ||
       memory[load_address + 511] != 0xAA)) {
    fprintf(stderr, "%s has no boot signature\n", argv[1]);
    return 1;
  }

  memset(&cpu, 0, sizeof(cpu));
  cpu.ip = (uint16_t)load_address;
  cpu.reg[REG_SP] = 0x7C00;

  while (!cpu.halted && !cpu.fault && steps++ < STEP_LIMIT) {
    step(&cpu);
  }

  if (cpu.fault) {
    fprintf(stderr, "fault at ip=%04x: %s\n", cpu.ip, cpu.fault_reason);
    return 1;
  }
  if (!cpu.halted) {
    fprintf(stderr, "image did not halt within %d steps\n", STEP_LIMIT);
    return 1;
  }
  if (strcmp(output, expected) != 0) {
    fprintf(stderr, "output was \"%s\", expected \"%s\"\n", output, expected);
    return 1;
  }
  printf("16-bit image printed \"%s\" and halted\n", output);
  return 0;
}
