// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/

#include "NesCpu.h"

#include "blargg_endian.h"
#include <limits.h>

#include "nes_cpu_io.h"

#include "blargg_source.h"

namespace gme {
namespace emu {
namespace nes {

#define BLARGG_CPU_X86 1

/* Copyright (C) 2003-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#ifdef BLARGG_ENABLE_OPTIMIZER
#include BLARGG_ENABLE_OPTIMIZER
#endif

#define FLUSH_TIME() (void) (s.time = s_time)
#define CACHE_TIME() (void) (s_time = s.time)

#ifndef CPU_DONE
#define CPU_DONE(cpu, time, result_out) \
  { result_out = -1; }
#endif

#ifndef CPU_READ_PPU
#define CPU_READ_PPU(cpu, addr, out, time) \
  { \
    FLUSH_TIME(); \
    out = CPU_READ(cpu, addr, time); \
    CACHE_TIME(); \
  }
#endif

#if BLARGG_NONPORTABLE
#define PAGE_OFFSET(addr) (addr)
#else
#define PAGE_OFFSET(addr) ((addr) & (PAGE_SIZE - 1))
#endif

static const int ST_N = 0x80;
static const int ST_V = 0x40;
static const int ST_R = 0x20;
static const int ST_B = 0x10;
static const int ST_D = 0x08;
static const int ST_I = 0x04;
static const int ST_Z = 0x02;
static const int ST_C = 0x01;

void NesCpu::reset(const uint8_t *unmapped_page) {
  check(this->m_pState == &this->m_state);
  this->m_pState = &this->m_state;
  this->m_state.time = 0;
  this->m_state.base = 0;
  this->m_regs.status = ST_I;
  this->m_regs.sp = 0xFF;
  this->m_regs.pc = 0;
  this->m_regs.a = 0;
  this->m_regs.x = 0;
  this->m_regs.y = 0;
  this->m_irqTime = future_nes_time;
  this->m_endTime = future_nes_time;
  this->m_errorsNum = 0;

  assert(PAGE_SIZE == 0x800);  // assumes this
  mapCode(0x0000, 0x2000, this->m_lowMem.data(), true);
  mapCode(0x2000, 0xE000, unmapped_page, true);
  this->m_setCodePage(PAGES_NUM, unmapped_page);

  blargg_verify_byte_order();
}

void NesCpu::mapCode(nes_addr_t start, unsigned size, const uint8_t *data, bool mirror) {
  // address range must begin and end on page boundaries
  require(start % PAGE_SIZE == 0);
  require(size % PAGE_SIZE == 0);
  require(start + size <= 0x10000);

  unsigned page = start / PAGE_SIZE;
  unsigned cnt = size / PAGE_SIZE;
  do {
    this->m_setCodePage(page++, data);
    if (!mirror)
      data += PAGE_SIZE;
  } while (--cnt);
}

#define TIME (s_time + s.base)
#define READ_LIKELY_PPU(addr, out) \
  { CPU_READ_PPU(this, (addr), out, TIME); }
#define READ(addr) CPU_READ(this, (addr), TIME)
#define WRITE(addr, data) \
  { CPU_WRITE(this, (addr), (data), TIME); }
#define READ_LOW(addr) (this->m_lowMem[int(addr)])
#define WRITE_LOW(addr, data) (void) (READ_LOW(addr) = (data))
#define READ_PROG(addr) (s.code_map[(addr) >> PAGE_BITS][PAGE_OFFSET(addr)])

#define SET_SP(v) (sp = ((v) + 1) | 0x100)
#define GET_SP() ((sp - 1) & 0xFF)
#define PUSH(v) ((sp = (sp - 1) | 0x100), WRITE_LOW(sp, v))

bool NesCpu::run(nes_time_t end_time) {
  setEndTime(end_time);
  state_t s = this->m_state;
  this->m_pState = &s;
  // even on x86, using s.time in place of s_time was slower
  int16_t s_time = s.time;

  // registers
  uint16_t pc = this->m_regs.pc;
  uint8_t a = this->m_regs.a;
  uint8_t x = this->m_regs.x;
  uint8_t y = this->m_regs.y;
  uint16_t sp;
  SET_SP(this->m_regs.sp);

// status flags
#define IS_NEG (nz & 0x8080)

#define CALC_STATUS(out) \
  do { \
    out = status & (ST_V | ST_D | ST_I); \
    out |= ((nz >> 8) | nz) & ST_N; \
    out |= c >> 8 & ST_C; \
    if (!(nz & 0xFF)) \
      out |= ST_Z; \
  } while (0)

#define SET_STATUS(in) \
  do { \
    status = in & (ST_V | ST_D | ST_I); \
    nz = in << 8; \
    c = nz; \
    nz |= ~in & ST_Z; \
  } while (0)

  uint8_t status;
  uint16_t c;   // carry set if (c & 0x100) != 0
  uint16_t nz;  // Z set if (nz & 0xFF) == 0, N set if (nz & 0x8080) != 0
  {
    uint8_t temp = this->m_regs.status;
    SET_STATUS(temp);
  }

  goto loop;
dec_clock_loop:
  s_time--;
loop:

  check((unsigned) GET_SP() < 0x100);
  check((unsigned) pc < 0x10000);
  check((unsigned) a < 0x100);
  check((unsigned) x < 0x100);
  check((unsigned) y < 0x100);
  check(-32768 <= s_time && s_time < 32767);

  const uint8_t *instr = s.code_map[pc >> PAGE_BITS];
  uint8_t opcode;

// TODO: eliminate this special case
#if BLARGG_NONPORTABLE
  opcode = instr[pc];
  pc++;
  instr += pc;
#else
  instr += PAGE_OFFSET(pc);
  opcode = *instr++;
  pc++;
#endif

  static const uint8_t CLOCK_TABLE[256] = {
      // 0 1 2 3 4 5 6 7 8 9 A B C D E F
      0, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,  // 0
      3, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 1
      6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,  // 2
      3, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 3
      6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,  // 4
      3, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 5
      6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,  // 6
      3, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 7
      2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // 8
      3, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,  // 9
      2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // A
      3, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,  // B
      2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // C
      3, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // D
      2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // E
      3, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7   // F
  };                                                   // 0x00 was 7 and 0xF2 was 2

  uint16_t data;

#if !BLARGG_CPU_X86
  if (s_time >= 0)
    goto out_of_time;
  s_time += CLOCK_TABLE[opcode];

  data = *instr;

  switch (opcode) {
#else

  data = CLOCK_TABLE[opcode];
  if ((s_time += data) >= 0)
    goto possibly_out_of_time;
almost_out_of_time:

  data = *instr;

  switch (opcode) {
  possibly_out_of_time:
    if (s_time < (int) data)
      goto almost_out_of_time;
    s_time -= data;
    goto out_of_time;
#endif

    // Macros

#define GET_MSB() (instr[1])
#define ADD_PAGE() (pc++, data += 0x100 * GET_MSB())
#define GET_ADDR() GET_LE16(instr)

#define NO_PAGE_CROSSING(lsb)
#define HANDLE_PAGE_CROSSING(lsb) s_time += (lsb) >> 8;

#define INC_DEC_XY(reg, n) \
  reg = uint8_t(nz = reg + n); \
  goto loop;

#define IND_Y(cross, out) \
  { \
    uint16_t temp = READ_LOW(data) + y; \
    out = temp + 0x100 * READ_LOW(uint8_t(data + 1)); \
    cross(temp); \
  }

#define IND_X(out) \
  { \
    uint16_t temp = data + x; \
    out = 0x100 * READ_LOW(uint8_t(temp + 1)) + READ_LOW(uint8_t(temp)); \
  }

#define ARITH_ADDR_MODES(op) \
  case op - 0x04: /* (ind,x) */ \
    IND_X(data) \
    goto ptr##op; \
  case op + 0x0C: /* (ind),y */ \
    IND_Y(HANDLE_PAGE_CROSSING, data) \
    goto ptr##op; \
  case op + 0x10:             /* zp,X */ \
    data = uint8_t(data + x); /* FALLTHRU */ \
  case op + 0x00:             /* zp */ \
    data = READ_LOW(data); \
    goto imm##op; \
  case op + 0x14: /* abs,Y */ \
    data += y; \
    goto ind##op; \
  case op + 0x18: /* abs,X */ \
    data += x; \
    ind##op : HANDLE_PAGE_CROSSING(data); /* FALLTHRU */ \
  case op + 0x08:                         /* abs */ \
    ADD_PAGE(); \
    ptr##op : FLUSH_TIME(); \
    data = READ(data); \
    CACHE_TIME(); /*FALLTHRU*/ \
  case op + 0x04: /* imm */ \
    imm##op:

// TODO: more efficient way to handle negative branch that wraps PC around
#define BRANCH(cond) \
  { \
    int16_t offset = (int8_t) data; \
    uint16_t extra_clock = (++pc & 0xFF) + offset; \
    if (!(cond)) \
      goto dec_clock_loop; \
    pc = uint16_t(pc + offset); \
    s_time += extra_clock >> 8 & 1; \
    goto loop; \
  }

      // Often-Used

    case 0xB5:  // LDA zp,x
      a = nz = READ_LOW(uint8_t(data + x));
      pc++;
      goto loop;

    case 0xA5:  // LDA zp
      a = nz = READ_LOW(data);
      pc++;
      goto loop;

    case 0xD0:  // BNE
      BRANCH((uint8_t) nz);

    case 0x20: {  // JSR
      uint16_t temp = pc + 1;
      pc = GET_ADDR();
      WRITE_LOW(0x100 | (sp - 1), temp >> 8);
      sp = (sp - 2) | 0x100;
      WRITE_LOW(sp, temp);
      goto loop;
    }

    case 0x4C:  // JMP abs
      pc = GET_ADDR();
      goto loop;

    case 0xE8:  // INX
      INC_DEC_XY(x, 1)

    case 0x10:  // BPL
      BRANCH(!IS_NEG)

      ARITH_ADDR_MODES(0xC5)  // CMP
      nz = a - data;
      pc++;
      c = ~nz;
      nz &= 0xFF;
      goto loop;

    case 0x30:  // BMI
      BRANCH(IS_NEG)

    case 0xF0:  // BEQ
      BRANCH(!(uint8_t) nz);

    case 0x95:                  // STA zp,x
      data = uint8_t(data + x); /*FALLTHRU*/
    case 0x85:                  // STA zp
      pc++;
      WRITE_LOW(data, a);
      goto loop;

    case 0xC8:  // INY
      INC_DEC_XY(y, 1)

    case 0xA8:  // TAY
      y = a;
      nz = a;
      goto loop;

    case 0x98:  // TYA
      a = y;
      nz = y;
      goto loop;

    case 0xAD: {  // LDA abs
      unsigned addr = GET_ADDR();
      pc += 2;
      READ_LIKELY_PPU(addr, nz);
      a = nz;
      goto loop;
    }

    case 0x60:  // RTS
      pc = 1 + READ_LOW(sp);
      pc += 0x100 * READ_LOW(0x100 | (sp - 0xFF));
      sp = (sp - 0xFE) | 0x100;
      goto loop;

      {
        uint16_t addr;

        case 0x99:  // STA abs,Y
          addr = y + GET_ADDR();
          pc += 2;
          if (addr <= 0x7FF) {
            WRITE_LOW(addr, a);
            goto loop;
          }
          goto sta_ptr;

        case 0x8D:  // STA abs
          addr = GET_ADDR();
          pc += 2;
          if (addr <= 0x7FF) {
            WRITE_LOW(addr, a);
            goto loop;
          }
          goto sta_ptr;

        case 0x9D:  // STA abs,X (slightly more common than STA abs)
          addr = x + GET_ADDR();
          pc += 2;
          if (addr <= 0x7FF) {
            WRITE_LOW(addr, a);
            goto loop;
          }
        sta_ptr:
          FLUSH_TIME();
          WRITE(addr, a);
          CACHE_TIME();
          goto loop;

        case 0x91:  // STA (ind),Y
          IND_Y(NO_PAGE_CROSSING, addr)
          pc++;
          goto sta_ptr;

        case 0x81:  // STA (ind,X)
          IND_X(addr)
          pc++;
          goto sta_ptr;
      }

    case 0xA9:  // LDA #imm
      pc++;
      a = data;
      nz = data;
      goto loop;

      // common read instructions
      {
        uint16_t addr;

        case 0xA1:  // LDA (ind,X)
          IND_X(addr)
          pc++;
          goto a_nz_read_addr;

        case 0xB1:  // LDA (ind),Y
          addr = READ_LOW(data) + y;
          HANDLE_PAGE_CROSSING(addr);
          addr += 0x100 * READ_LOW((uint8_t)(data + 1));
          pc++;
          a = nz = READ_PROG(addr);
          if ((addr ^ 0x8000) <= 0x9FFF)
            goto loop;
          goto a_nz_read_addr;

        case 0xB9:  // LDA abs,Y
          HANDLE_PAGE_CROSSING(data + y);
          addr = GET_ADDR() + y;
          pc += 2;
          a = nz = READ_PROG(addr);
          if ((addr ^ 0x8000) <= 0x9FFF)
            goto loop;
          goto a_nz_read_addr;

        case 0xBD:  // LDA abs,X
          HANDLE_PAGE_CROSSING(data + x);
          addr = GET_ADDR() + x;
          pc += 2;
          a = nz = READ_PROG(addr);
          if ((addr ^ 0x8000) <= 0x9FFF)
            goto loop;
        a_nz_read_addr:
          FLUSH_TIME();
          a = nz = READ(addr);
          CACHE_TIME();
          goto loop;
      }

      // Branch

    case 0x50:  // BVC
      BRANCH(!(status & ST_V))

    case 0x70:  // BVS
      BRANCH(status & ST_V)

    case 0xB0:  // BCS
      BRANCH(c & 0x100)

    case 0x90:  // BCC
      BRANCH(!(c & 0x100))

      // Load/store

    case 0x94:                   // STY zp,x
      data = uint8_t(data + x);  // FALLTHRU
    case 0x84:                   // STY zp
      pc++;
      WRITE_LOW(data, y);
      goto loop;

    case 0x96:                   // STX zp,y
      data = uint8_t(data + y);  // FALLTHRU
    case 0x86:                   // STX zp
      pc++;
      WRITE_LOW(data, x);
      goto loop;

    case 0xB6:                   // LDX zp,y
      data = uint8_t(data + y);  // FALLTHRU
    case 0xA6:                   // LDX zp
      data = READ_LOW(data);     // FALLTHRU
    case 0xA2:                   // LDX #imm
      pc++;
      x = data;
      nz = data;
      goto loop;

    case 0xB4:                   // LDY zp,x
      data = uint8_t(data + x);  // FALLTHRU
    case 0xA4:                   // LDY zp
      data = READ_LOW(data);     // FALLTHRU
    case 0xA0:                   // LDY #imm
      pc++;
      y = data;
      nz = data;
      goto loop;

    case 0xBC:  // LDY abs,X
      data += x;
      HANDLE_PAGE_CROSSING(data); /*FALLTHRU*/
    case 0xAC: {                  // LDY abs
      unsigned addr = data + 0x100 * GET_MSB();
      pc += 2;
      FLUSH_TIME();
      y = nz = READ(addr);
      CACHE_TIME();
      goto loop;
    }

    case 0xBE:  // LDX abs,y
      data += y;
      HANDLE_PAGE_CROSSING(data); /*FALLTHRU*/
    case 0xAE: {                  // LDX abs
      unsigned addr = data + 0x100 * GET_MSB();
      pc += 2;
      FLUSH_TIME();
      x = nz = READ(addr);
      CACHE_TIME();
      goto loop;
    }

      {
        uint8_t temp;
        case 0x8C:  // STY abs
          temp = y;
          goto store_abs;

        case 0x8E:  // STX abs
          temp = x;
        store_abs:
          unsigned addr = GET_ADDR();
          pc += 2;
          if (addr <= 0x7FF) {
            WRITE_LOW(addr, temp);
            goto loop;
          }
          FLUSH_TIME();
          WRITE(addr, temp);
          CACHE_TIME();
          goto loop;
      }

      // Compare

    case 0xEC: {  // CPX abs
      unsigned addr = GET_ADDR();
      pc++;
      FLUSH_TIME();
      data = READ(addr);
      CACHE_TIME();
      goto cpx_data;
    }

    case 0xE4:               // CPX zp
      data = READ_LOW(data); /*FALLTHRU*/
    case 0xE0:               // CPX #imm
    cpx_data:
      nz = x - data;
      pc++;
      c = ~nz;
      nz &= 0xFF;
      goto loop;

    case 0xCC: {  // CPY abs
      unsigned addr = GET_ADDR();
      pc++;
      FLUSH_TIME();
      data = READ(addr);
      CACHE_TIME();
      goto cpy_data;
    }

    case 0xC4:               // CPY zp
      data = READ_LOW(data); /*FALLTHRU*/
    case 0xC0:               // CPY #imm
    cpy_data:
      nz = y - data;
      pc++;
      c = ~nz;
      nz &= 0xFF;
      goto loop;

      // Logical

      ARITH_ADDR_MODES(0x25)  // AND
      nz = (a &= data);
      pc++;
      goto loop;

      ARITH_ADDR_MODES(0x45)  // EOR
      nz = (a ^= data);
      pc++;
      goto loop;

      ARITH_ADDR_MODES(0x05)  // ORA
      nz = (a |= data);
      pc++;
      goto loop;

    case 0x2C: {  // BIT abs
      unsigned addr = GET_ADDR();
      pc += 2;
      status &= ~ST_V;
      READ_LIKELY_PPU(addr, nz);
      status |= nz & ST_V;
      if (a & nz)
        goto loop;
      nz <<= 8;  // result must be zero, even if N bit is set
      goto loop;
    }

    case 0x24:  // BIT zp
      nz = READ_LOW(data);
      pc++;
      status &= ~ST_V;
      status |= nz & ST_V;
      if (a & nz)
        goto loop;
      nz <<= 8;  // result must be zero, even if N bit is set
      goto loop;

      // Add/subtract

      ARITH_ADDR_MODES(0xE5)  // SBC
    case 0xEB:                // unofficial equivalent
      data ^= 0xFF;
      goto adc_imm;

      ARITH_ADDR_MODES(0x65)  // ADC
    adc_imm : {
      int16_t carry = c >> 8 & 1;
      int16_t ov = (a ^ 0x80) + carry + (int8_t) data;  // sign-extend
      status &= ~ST_V;
      status |= ov >> 2 & 0x40;
      c = nz = a + data + carry;
      pc++;
      a = (uint8_t) nz;
      goto loop;
    }

      // Shift/rotate

    case 0x4A:  // LSR A
      c = 0;    /*FALLTHRU*/
    case 0x6A:  // ROR A
      nz = c >> 1 & 0x80;
      c = a << 8;
      nz |= a >> 1;
      a = nz;
      goto loop;

    case 0x0A:  // ASL A
      nz = a << 1;
      c = nz;
      a = (uint8_t) nz;
      goto loop;

    case 0x2A: {  // ROL A
      nz = a << 1;
      int16_t temp = c >> 8 & 1;
      c = nz;
      nz |= temp;
      a = (uint8_t) nz;
      goto loop;
    }

    case 0x5E:   // LSR abs,X
      data += x; /*FALLTHRU*/
    case 0x4E:   // LSR abs
      c = 0;     /*FALLTHRU*/
    case 0x6E:   // ROR abs
    ror_abs : {
      ADD_PAGE();
      FLUSH_TIME();
      int temp = READ(data);
      nz = (c >> 1 & 0x80) | (temp >> 1);
      c = temp << 8;
      goto rotate_common;
    }

    case 0x3E:  // ROL abs,X
      data += x;
      goto rol_abs;

    case 0x1E:   // ASL abs,X
      data += x; /*FALLTHRU*/
    case 0x0E:   // ASL abs
      c = 0;     /*FALLTHRU*/
    case 0x2E:   // ROL abs
    rol_abs:
      ADD_PAGE();
      nz = c >> 8 & 1;
      FLUSH_TIME();
      nz |= (c = READ(data) << 1);
    rotate_common:
      pc++;
      WRITE(data, (uint8_t) nz);
      CACHE_TIME();
      goto loop;

    case 0x7E:  // ROR abs,X
      data += x;
      goto ror_abs;

    case 0x76:  // ROR zp,x
      data = uint8_t(data + x);
      goto ror_zp;

    case 0x56:                  // LSR zp,x
      data = uint8_t(data + x); /*FALLTHRU*/
    case 0x46:                  // LSR zp
      c = 0;                    /*FALLTHRU*/
    case 0x66:                  // ROR zp
    ror_zp : {
      int temp = READ_LOW(data);
      nz = (c >> 1 & 0x80) | (temp >> 1);
      c = temp << 8;
      goto write_nz_zp;
    }

    case 0x36:  // ROL zp,x
      data = uint8_t(data + x);
      goto rol_zp;

    case 0x16:                  // ASL zp,x
      data = uint8_t(data + x); /*FALLTHRU*/
    case 0x06:                  // ASL zp
      c = 0;                    /*FALLTHRU*/
    case 0x26:                  // ROL zp
    rol_zp:
      nz = c >> 8 & 1;
      nz |= (c = READ_LOW(data) << 1);
      goto write_nz_zp;

      // Increment/decrement

    case 0xCA:  // DEX
      INC_DEC_XY(x, -1)

    case 0x88:  // DEY
      INC_DEC_XY(y, -1)

    case 0xF6:                  // INC zp,x
      data = uint8_t(data + x); /*FALLTHRU*/
    case 0xE6:                  // INC zp
      nz = 1;
      goto add_nz_zp;

    case 0xD6:                  // DEC zp,x
      data = uint8_t(data + x); /*FALLTHRU*/
    case 0xC6:                  // DEC zp
      nz = (uint16_t) -1;
    add_nz_zp:
      nz += READ_LOW(data);
    write_nz_zp:
      pc++;
      WRITE_LOW(data, nz);
      goto loop;

    case 0xFE:  // INC abs,x
      data = x + GET_ADDR();
      goto inc_ptr;

    case 0xEE:  // INC abs
      data = GET_ADDR();
    inc_ptr:
      nz = 1;
      goto inc_common;

    case 0xDE:  // DEC abs,x
      data = x + GET_ADDR();
      goto dec_ptr;

    case 0xCE:  // DEC abs
      data = GET_ADDR();
    dec_ptr:
      nz = (uint16_t) -1;
    inc_common:
      FLUSH_TIME();
      nz += READ(data);
      pc += 2;
      WRITE(data, (uint8_t) nz);
      CACHE_TIME();
      goto loop;

      // Transfer

    case 0xAA:  // TAX
      x = a;
      nz = a;
      goto loop;

    case 0x8A:  // TXA
      a = x;
      nz = x;
      goto loop;

    case 0x9A:    // TXS
      SET_SP(x);  // verified (no flag change)
      goto loop;

    case 0xBA:  // TSX
      x = nz = GET_SP();
      goto loop;

      // Stack

    case 0x48:  // PHA
      PUSH(a);  // verified
      goto loop;

    case 0x68:  // PLA
      a = nz = READ_LOW(sp);
      sp = (sp - 0xFF) | 0x100;
      goto loop;

    case 0x40: {  // RTI
      uint8_t temp = READ_LOW(sp);
      pc = READ_LOW(0x100 | (sp - 0xFF));
      pc |= READ_LOW(0x100 | (sp - 0xFE)) * 0x100;
      sp = (sp - 0xFD) | 0x100;
      data = status;
      SET_STATUS(temp);
      if (!((data ^ status) & ST_I))
        goto loop;                   // I flag didn't change
      this->m_regs.status = status;  // update externally-visible I flag
      blargg_long delta = s.base - this->m_irqTime;
      if (delta <= 0)
        goto loop;
      if (status & ST_I)
        goto loop;
      s_time += delta;
      s.base = this->m_irqTime;
      goto loop;
    }

    case 0x28: {  // PLP
      uint8_t temp = READ_LOW(sp);
      sp = (sp - 0xFF) | 0x100;
      uint8_t changed = status ^ temp;
      SET_STATUS(temp);
      if (!(changed & ST_I))
        goto loop;  // I flag didn't change
      if (status & ST_I)
        goto handle_sei;
      goto handle_cli;
    }

    case 0x08: {  // PHP
      uint8_t temp;
      CALC_STATUS(temp);
      PUSH(temp | (ST_B | ST_R));
      goto loop;
    }

    case 0x6C: {  // JMP (ind)
      data = GET_ADDR();
      check(unsigned(data - 0x2000) >= 0x4000);  // ensure it's outside I/O space
      uint8_t const *page = s.code_map[data >> PAGE_BITS];
      pc = page[PAGE_OFFSET(data)];
      data = (data & 0xFF00) | ((data + 1) & 0xFF);
      pc |= page[PAGE_OFFSET(data)] << 8;
      goto loop;
    }

    case 0x00:  // BRK
      goto handle_brk;

      // Flags

    case 0x38:  // SEC
      c = (uint16_t) ~0;
      goto loop;

    case 0x18:  // CLC
      c = 0;
      goto loop;

    case 0xB8:  // CLV
      status &= ~ST_V;
      goto loop;

    case 0xD8:  // CLD
      status &= ~ST_D;
      goto loop;

    case 0xF8:  // SED
      status |= ST_D;
      goto loop;

    case 0x58:  // CLI
      if (!(status & ST_I))
        goto loop;
      status &= ~ST_I;
    handle_cli : {
      // debug_printf( "CLI at %d\n", TIME );
      this->m_regs.status = status;  // update externally-visible I flag
      blargg_long delta = s.base - this->m_irqTime;
      if (delta <= 0) {
        if (TIME < this->m_irqTime)
          goto loop;
        goto delayed_cli;
      }
      s.base = this->m_irqTime;
      s_time += delta;
      if (s_time < 0)
        goto loop;

      if (delta >= s_time + 1) {
        s.base += s_time + 1;
        s_time = -1;
        goto loop;
      }

      // TODO: implement
    delayed_cli:
      debug_printf("Delayed CLI not emulated\n");
      goto loop;
    }

    case 0x78:  // SEI
      if (status & ST_I)
        goto loop;
      status |= ST_I;
    handle_sei : {
      this->m_regs.status = status;  // update externally-visible I flag
      blargg_long delta = s.base - this->m_endTime;
      s.base = this->m_endTime;
      s_time += delta;
      if (s_time < 0)
        goto loop;

      debug_printf("Delayed SEI not emulated\n");
      goto loop;
    }

      // Unofficial

    // SKW - Skip word
    case 0x1C:
    case 0x3C:
    case 0x5C:
    case 0x7C:
    case 0xDC:
    case 0xFC:
      HANDLE_PAGE_CROSSING(data + x); /*FALLTHRU*/
    case 0x0C:
      pc++; /*FALLTHRU*/
    // SKB - Skip byte
    case 0x74:
    case 0x04:
    case 0x14:
    case 0x34:
    case 0x44:
    case 0x54:
    case 0x64:
    case 0x80:
    case 0x82:
    case 0x89:
    case 0xC2:
    case 0xD4:
    case 0xE2:
    case 0xF4:
      pc++;
      goto loop;

    // NOP
    case 0xEA:
    case 0x1A:
    case 0x3A:
    case 0x5A:
    case 0x7A:
    case 0xDA:
    case 0xFA:
      goto loop;

    case BAD_OPCODE:  // HLT
      pc--;
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32:
    case 0x42:
    case 0x52:
    case 0x62:
    case 0x72:
    case 0x92:
    case 0xB2:
    case 0xD2:
      goto stop;

      // Unimplemented

    case 0xFF:  // force 256-entry jump table for optimization purposes
      c |= 1;   /*FALLTHRU*/
    default:
      check((unsigned) opcode <= 0xFF);
      // skip over proper number of bytes
      static unsigned char const illop_lens[8] = {0x40, 0x40, 0x40, 0x80, 0x40, 0x40, 0x80, 0xA0};
      uint8_t opcode = instr[-1];
      int16_t len = illop_lens[opcode >> 2 & 7] >> (opcode << 1 & 6) & 3;
      if (opcode == 0x9C)
        len = 2;
      pc += len;
      this->m_errorsNum++;

      if ((opcode >> 4) == 0x0B) {
        if (opcode == 0xB3)
          data = READ_LOW(data);
        if (opcode != 0xB7)
          HANDLE_PAGE_CROSSING(data + y);
      }
      goto loop;
  }
  assert(false);

  int result_;
handle_brk:
  pc++;
  result_ = 4;

interrupt : {
  s_time += 7;

  WRITE_LOW(0x100 | (sp - 1), pc >> 8);
  WRITE_LOW(0x100 | (sp - 2), pc);
  pc = GET_LE16(&READ_PROG(0xFFFA) + result_);

  sp = (sp - 3) | 0x100;
  uint8_t temp;
  CALC_STATUS(temp);
  temp |= ST_R;
  if (result_)
    temp |= ST_B;  // TODO: incorrectly sets B flag for IRQ
  WRITE_LOW(sp, temp);

  this->m_regs.status = status |= ST_I;
  blargg_long delta = s.base - this->m_endTime;
  if (delta >= 0)
    goto loop;
  s_time += delta;
  s.base = this->m_endTime;
  goto loop;
}

out_of_time:
  pc--;
  FLUSH_TIME();
  CPU_DONE(this, TIME, result_);
  CACHE_TIME();
  if (result_ >= 0)
    goto interrupt;
  if (s_time < 0)
    goto loop;

stop:

  s.time = s_time;

  this->m_regs.pc = pc;
  this->m_regs.sp = GET_SP();
  this->m_regs.a = a;
  this->m_regs.x = x;
  this->m_regs.y = y;

  {
    uint8_t temp;
    CALC_STATUS(temp);
    this->m_regs.status = temp;
  }

  this->m_state = s;
  this->m_pState = &this->m_state;

  return s_time < 0;
}

}  // namespace nes
}  // namespace emu
}  // namespace gme