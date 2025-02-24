#pragma once

#include <stdio.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <set>
#include <phosg/Strings.hh>
#include <string>

#include "EmulatorBase.hh"
#include "MemoryContext.hh"
#include "InterruptManager.hh"


enum class ValueType {
  // Note: the values here correspond to the values in the Source Specifier (U)
  // field in float opcodes.
  LONG = 0,
  FLOAT = 1,
  EXTENDED = 2,
  PACKED_DECIMAL_REAL = 3,
  WORD = 4,
  DOUBLE = 5,
  BYTE = 6,
  INVALID = 7,
};

struct M68KRegisters {
  union {
    uint32_t u;
    int32_t s;
  } d[8];
  uint32_t a[8];
  uint32_t pc;
  uint16_t sr; // Note: low byte of this is the ccr (condition code register)

  M68KRegisters();

  void import_state(FILE* stream);
  void export_state(FILE* stream) const;

  void set_by_name(const std::string& reg_name, uint32_t value);

  inline uint32_t get_sp() const {
    return this->a[7];
  }
  inline void set_sp(uint32_t sp) {
    this->a[7] = sp;
  }

  uint32_t get_reg_value(bool is_a_reg, uint8_t reg_num);

  inline void reset_access_flags() const { }

  void set_ccr_flags(int64_t x, int64_t n, int64_t z, int64_t v, int64_t c);
  void set_ccr_flags_integer_add(int32_t left_value, int32_t right_value, uint8_t size);
  void set_ccr_flags_integer_subtract(int32_t left_value, int32_t right_value, uint8_t size);

  uint32_t pop_u32(std::shared_ptr<const MemoryContext> mem);
  int32_t pop_s32(std::shared_ptr<const MemoryContext> mem);
  uint16_t pop_u16(std::shared_ptr<const MemoryContext> mem);
  int16_t pop_s16(std::shared_ptr<const MemoryContext> mem);
  uint8_t pop_u8(std::shared_ptr<const MemoryContext> mem);
  int8_t pop_s8(std::shared_ptr<const MemoryContext> mem);

  void push_u32(std::shared_ptr<MemoryContext> mem, uint32_t v);
  void push_s32(std::shared_ptr<MemoryContext> mem, int32_t v);
  void push_u16(std::shared_ptr<MemoryContext> mem, uint16_t v);
  void push_s16(std::shared_ptr<MemoryContext> mem, int16_t v);
  void push_u8(std::shared_ptr<MemoryContext> mem, uint8_t v);
  void push_s8(std::shared_ptr<MemoryContext> mem, int8_t v);

  void write_stack_u32(std::shared_ptr<MemoryContext> mem, uint32_t v);
  void write_stack_s32(std::shared_ptr<MemoryContext> mem, int32_t v);
  void write_stack_u16(std::shared_ptr<MemoryContext> mem, uint16_t v);
  void write_stack_s16(std::shared_ptr<MemoryContext> mem, int16_t v);
  void write_stack_u8(std::shared_ptr<MemoryContext> mem, uint8_t v);
  void write_stack_s8(std::shared_ptr<MemoryContext> mem, int8_t v);
};


class M68KEmulator : public EmulatorBase {
public:
  static constexpr bool is_little_endian = false;

  explicit M68KEmulator(std::shared_ptr<MemoryContext> mem);
  virtual ~M68KEmulator() = default;

  virtual void import_state(FILE* stream);
  virtual void export_state(FILE* stream) const;

  M68KRegisters& registers();

  virtual void print_state_header(FILE* stream) const;
  virtual void print_state(FILE* stream) const;

  static std::string disassemble_one(
      StringReader& r,
      uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);
  static std::string disassemble_one(
      const void* vdata,
      size_t size,
      uint32_t start_address);
  static std::string disassemble(
      const void* vdata,
      size_t size,
      uint32_t start_address = 0,
      const std::multimap<uint32_t, std::string>* labels = nullptr);

  inline void set_syscall_handler(std::function<void(M68KEmulator&, uint16_t)> handler) {
    this->syscall_handler = handler;
  }

  inline void set_debug_hook(std::function<void(M68KEmulator&)> hook) {
    this->debug_hook = hook;
  }

  inline void set_interrupt_manager(std::shared_ptr<InterruptManager> im) {
    this->interrupt_manager = im;
  }

  virtual void print_source_trace(FILE* stream, const std::string& what, size_t max_depth = 0) const;

  virtual void execute();

private:
  M68KRegisters regs;

  std::function<void(M68KEmulator&, uint16_t)> syscall_handler;
  std::function<void(M68KEmulator&)> debug_hook;
  std::shared_ptr<InterruptManager> interrupt_manager;

  struct OpcodeImplementation {
    void (M68KEmulator::*exec)(uint16_t);
    std::string (*dasm)(StringReader& r, uint32_t start_address, std::map<uint32_t, bool>& branch_target_addresses);
  };
  static const OpcodeImplementation fns[0x10];

  struct ResolvedAddress {
    enum class Location {
      MEMORY = 0,
      D_REGISTER = 1,
      A_REGISTER = 2,
      SR = 3,
    };

    uint32_t addr;
    Location location;

    bool is_register() const;
  };

  uint32_t read(const ResolvedAddress& addr, uint8_t size) const;
  uint32_t read(uint32_t addr, uint8_t size) const;
  void write(const ResolvedAddress& addr, uint32_t value, uint8_t size);
  void write(uint32_t addr, uint32_t value, uint8_t size);

  uint16_t fetch_instruction_word(bool advance = true);
  int16_t fetch_instruction_word_signed(bool advance = true);
  uint32_t fetch_instruction_data(uint8_t size, bool advance = true);
  int32_t fetch_instruction_data_signed(uint8_t size, bool advance = true);

  uint32_t resolve_address_extension(uint16_t ext);
  uint32_t resolve_address_control(uint8_t M, uint8_t Xn);
  uint32_t resolve_address_jump(uint8_t M, uint8_t Xn);
  ResolvedAddress resolve_address(uint8_t M, uint8_t Xn, uint8_t size);

  static std::string dasm_reg_mask(uint16_t mask, bool reverse);
  static std::string dasm_address_extension(StringReader& r, uint16_t ext, int8_t An);
  static std::string dasm_address(StringReader& r, uint32_t opcode_start_address,
      uint8_t M, uint8_t Xn, ValueType type,
      std::map<uint32_t, bool>* branch_target_addresses = nullptr,
      bool is_function_call = false);

  bool check_condition(uint8_t condition);

  void exec_unimplemented(uint16_t opcode);
  static std::string dasm_unimplemented(StringReader& r, uint32_t start_address, std::map<uint32_t, bool>& branch_target_addresses);

  void exec_0123(uint16_t opcode);
  static std::string dasm_0123(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_4(uint16_t opcode);
  static std::string dasm_4(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_5(uint16_t opcode);
  static std::string dasm_5(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_6(uint16_t opcode);
  static std::string dasm_6(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_7(uint16_t opcode);
  static std::string dasm_7(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_8(uint16_t opcode);
  static std::string dasm_8(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_9D(uint16_t opcode);
  static std::string dasm_9D(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_A(uint16_t opcode);
  static std::string dasm_A(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_B(uint16_t opcode);
  static std::string dasm_B(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_C(uint16_t opcode);
  static std::string dasm_C(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_E(uint16_t opcode);
  static std::string dasm_E(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void exec_F(uint16_t opcode);
  static std::string dasm_F(StringReader& r, uint32_t start_address,
      std::map<uint32_t, bool>& branch_target_addresses);

  void execute_next_opcode();
};
