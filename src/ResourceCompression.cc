#include "ResourceCompression.hh"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <exception>
#include <phosg/Encoding.hh>
#include <phosg/Time.hh>
#include <stdexcept>
#include <string>
#include <vector>

#include "Emulators/M68KEmulator.hh"
#include "Emulators/PPC32Emulator.hh"
#include "Decompressors/System.hh"

using namespace std;
using Resource = ResourceFile::Resource;



shared_ptr<const Resource> get_system_decompressor(
    bool use_ncmp, int16_t resource_id) {
  static unordered_map<uint64_t, shared_ptr<const Resource>> id_to_res;

  // If it's already in the cache, just return it verbatim
  uint32_t resource_type = use_ncmp ? RESOURCE_TYPE_ncmp : RESOURCE_TYPE_dcmp;
  uint64_t key = (static_cast<uint64_t>(resource_type) << 16) | resource_id;
  try {
    return id_to_res.at(key);
  } catch (const out_of_range&) { }

  string filename = string_printf("system_dcmps/%ccmp_%hd.bin",
      use_ncmp ? 'n' : 'd', resource_id);
  return id_to_res.emplace(key,
      new Resource(resource_type, resource_id, load_file(filename))).first->second;
}

struct M68KDecompressorInputHeader {
  // This is used to tell the program where to return to (stack pointer points
  // here at entry time)
  be_uint32_t return_addr;

  // Parameters to the decompressor - the m68k calling convention passes args on
  // the stack, so these are the actual args to the function
  union {
    struct { // used when header_version == 8
      be_uint32_t data_size;
      be_uint32_t working_buffer_addr;
      be_uint32_t dest_buffer_addr;
      be_uint32_t source_buffer_addr;
    } __attribute__((packed)) v8;
    struct { // used when header_version == 9
      be_uint32_t source_resource_header;
      be_uint32_t dest_buffer_addr;
      be_uint32_t source_buffer_addr;
      be_uint32_t data_size;
    } __attribute__((packed)) v9;
  } __attribute__((packed)) args;

  // This is where the program returns to; we use the reset opcode to stop
  // emulation cleanly
  be_uint16_t reset_opcode;
  be_uint16_t unused;
} __attribute__((packed));

struct PPC32DecompressorInputHeader {
  be_uint32_t saved_r1;
  be_uint32_t saved_cr;
  be_uint32_t saved_lr;
  be_uint32_t reserved1;
  be_uint32_t reserved2;
  be_uint32_t saved_r2;

  be_uint32_t unused[2];

  // This is where the program returns to; we set r2 to -1 (which should never
  // happen normally) and make the syscall handler stop emulation
  be_uint32_t set_r2_opcode;
  be_uint32_t syscall_opcode;
} __attribute__((packed));



void decompress_resource(
    shared_ptr<Resource> res,
    uint64_t decompress_flags,
    ResourceFile* context_rf) {
  // If the resource isn't compressed, or we already failed to decompress it, or
  // recompression is disabled globally, then do nothing
  if (!(res->flags & ResourceFlag::FLAG_COMPRESSED)) {
    return;
  }
  if (!(decompress_flags & DecompressionFlag::RETRY) &&
      (res->flags & ResourceFlag::FLAG_DECOMPRESSION_FAILED)) {
    return;
  }
  if (decompress_flags & DecompressionFlag::DISABLED) {
    return;
  }

  bool debug_execution = !!(decompress_flags & DecompressionFlag::DEBUG_EXECUTION);
  bool trace_execution = debug_execution || !!(decompress_flags & DecompressionFlag::TRACE_EXECUTION);
  bool verbose = trace_execution || !!(decompress_flags & DecompressionFlag::VERBOSE);

  if (res->data.size() < sizeof(CompressedResourceHeader)) {
    throw runtime_error("resource marked as compressed but is too small");
  }

  const auto& header = *reinterpret_cast<const CompressedResourceHeader*>(
      res->data.data());
  if (header.magic != 0xA89F6572) {
    // It looks like some resources have the compression bit set but aren't
    // actually compressed. Reverse-engineering ResEdit makes it look like the
    // Resource Manager just treats the resource as uncompressed if this value
    // is missing, so let's also not fail in that case.
    res->flags = res->flags & ~ResourceFlag::FLAG_COMPRESSED;
    return;
  }

  if (!(header.attributes & 0x01)) {
    throw runtime_error("resource marked as compressed but does not have compression attribute set");
  }

  int16_t dcmp_resource_id;
  uint16_t output_extra_bytes;
  if (header.header_version == 9) {
    dcmp_resource_id = header.version.v9.dcmp_resource_id;
    output_extra_bytes = header.version.v9.output_extra_bytes;
  } else if (header.header_version == 8) {
    dcmp_resource_id = header.version.v8.dcmp_resource_id;
    output_extra_bytes = header.version.v8.output_extra_bytes;
  } else {
    throw runtime_error("compressed resource header version is not 8 or 9");
  }

  // In order of priority, we try:
  // 1. dcmp resource from the context ResourceFile
  // 2. ncmp resource from the context ResourceFile
  // 3. internal implementation from src/Decompressors/SystemN.cc
  // 4. system dcmp from system_dcmps/dcmp_N.bin
  // 5. system ncmp from system_dcmps/ncmp_N.bin
  // As an awful hack, we use nullptr to represent the internal implementation,
  // since it's the only one that can't be represented by a Resource struct.
  vector<shared_ptr<const Resource>> dcmp_resources;
  if (context_rf) {
    if (!(decompress_flags & DecompressionFlag::SKIP_FILE_DCMP)) {
      try {
        dcmp_resources.emplace_back(context_rf->get_resource(
            RESOURCE_TYPE_dcmp, dcmp_resource_id));
      } catch (const out_of_range&) { }
    }
    if (!(decompress_flags & DecompressionFlag::SKIP_FILE_NCMP)) {
      try {
        dcmp_resources.emplace_back(context_rf->get_resource(
            RESOURCE_TYPE_ncmp, dcmp_resource_id));
      } catch (const out_of_range&) { }
    }
  }
  if (!(decompress_flags & DecompressionFlag::SKIP_INTERNAL)) {
    if ((dcmp_resource_id >= 0) && (dcmp_resource_id <= 3)) {
      dcmp_resources.emplace_back(nullptr);
    }
  }
  if (!(decompress_flags & DecompressionFlag::SKIP_SYSTEM_DCMP)) {
    try {
      dcmp_resources.emplace_back(get_system_decompressor(false, dcmp_resource_id));
    } catch (const cannot_open_file&) { }
  }
  if (!(decompress_flags & DecompressionFlag::SKIP_SYSTEM_NCMP)) {
    try {
      dcmp_resources.emplace_back(get_system_decompressor(true, dcmp_resource_id));
    } catch (const cannot_open_file&) { }
  }

  if (dcmp_resources.empty()) {
    throw runtime_error("no decompressors are available for this resource");
  }

  if (verbose) {
    fprintf(stderr, "using dcmp/ncmp %hd (%zu implementation(s) available)\n",
        dcmp_resource_id, dcmp_resources.size());
    fprintf(stderr, "note: data size is %zu (0x%zX); decompressed data size is %" PRIu32 " (0x%" PRIX32 ") bytes\n",
        res->data.size(), res->data.size(),
        header.decompressed_size.load(), header.decompressed_size.load());
  }

  for (size_t z = 0; z < dcmp_resources.size(); z++) {
    shared_ptr<const Resource> dcmp_res = dcmp_resources[z];
    if (verbose) {
      fprintf(stderr, "attempting decompression with implementation %zu of %zu\n",
          z + 1, dcmp_resources.size());
    }

    try {
      if (!dcmp_res.get()) {
        string (*decompress)(
            const CompressedResourceHeader& header,
            const void* source,
            size_t size) = nullptr;
        if (dcmp_resource_id == 0) {
          decompress = &decompress_system0;
        } else if (dcmp_resource_id == 1) {
          decompress = &decompress_system1;
        } else if (dcmp_resource_id == 2) {
          decompress = &decompress_system2;
        } else if (dcmp_resource_id == 3) {
          decompress = &decompress_system3;
        }

        if (!decompress) {
          throw logic_error(string_printf(
              "internal implementation of dcmp %hd requested, but does not exist",
              dcmp_resource_id));
        } else {
          uint64_t start_time = now();
          string decompressed_data = decompress(
              header,
              res->data.data() + sizeof(CompressedResourceHeader),
              res->data.size() - sizeof(CompressedResourceHeader));
          if (decompressed_data.size() != header.decompressed_size) {
            throw runtime_error(string_printf(
                "internal decompressor produced the wrong amount of data (%" PRIu32 " bytes expected, %zu bytes received)",
                header.decompressed_size.load(), decompressed_data.size()));
          }
          if (verbose) {
            float duration = static_cast<float>(now() - start_time) / 1000000.0f;
            fprintf(stderr, "note: decompressed resource using internal decompressor in %g seconds (%zu -> %zu bytes)\n",
                duration, res->data.size(), decompressed_data.size());
          }
          res->data = move(decompressed_data);
          res->flags = (res->flags & ~ResourceFlag::FLAG_COMPRESSED) | ResourceFlag::FLAG_DECOMPRESSED;
          return;
        }

      } else {
        shared_ptr<MemoryContext> mem(new MemoryContext());

        uint32_t entry_pc = 0;
        uint32_t entry_r2 = 0;
        bool is_ppc;
        if (dcmp_res->type == RESOURCE_TYPE_dcmp) {
          is_ppc = false;

          // Figure out where in the dcmp to start execution. There appear to be
          // two formats: one that has 'dcmp' in bytes 4-8 where execution appears
          // to just start at byte 0 (usually it's a branch opcode), and one where
          // the first three words appear to be offsets to various functions,
          // followed by code. The second word appears to be the main entry point
          // in this format, so we use that to determine where to start execution.
          // TODO: It looks like the decompression implementation in ResEdit
          // assumes the second format (with the three offsets) if and only if the
          // compressed resource has header format 9. This feels kind of bad
          // because... shouldn't the dcmp format be a property of the dcmp
          // resource, not the resource being decompressed? We use a heuristic
          // here instead, which seems correct for all decompressors I've seen.
          // TODO: Call init and exit for decompressors that have them. It's not
          // clear (yet) what the arguments to init and exit should be... they
          // each apparently take one argument based on how the adjust the stack
          // before returning, but every decompressor I've seen ignores the
          // argument.
          uint32_t entry_offset;
          if (dcmp_res->data.size() < 10) {
            throw runtime_error("decompressor resource is too short");
          }
          if (dcmp_res->data.substr(4, 4) == "dcmp") {
            entry_offset = 0;
          } else {
            entry_offset = *reinterpret_cast<const be_uint16_t*>(
                dcmp_res->data.data() + 2);
          }

          // Load the dcmp into emulated memory
          size_t code_region_size = dcmp_res->data.size();
          uint32_t code_addr = 0xF0000000;
          mem->allocate_at(code_addr, code_region_size);
          mem->memcpy(code_addr, dcmp_res->data.data(), dcmp_res->data.size());

          entry_pc = code_addr + entry_offset;
          if (verbose) {
            fprintf(stderr, "loaded code at %08" PRIX32 ":%zX\n", code_addr, code_region_size);
            fprintf(stderr, "dcmp entry offset is %08" PRIX32 " (loaded at %" PRIX32 ")\n",
                entry_offset, entry_pc);
          }

        } else if (dcmp_res->type == RESOURCE_TYPE_ncmp) {
          PEFFFile f("<ncmp>", dcmp_res->data);
          f.load_into("<ncmp>", mem, 0xF0000000);
          is_ppc = f.is_ppc();

          // ncmp decompressors don't appear to define any of the standard export
          // symbols (init/main/term); instead, they define a single export symbol
          // in the export table.
          if (!f.init().name.empty()) {
            throw runtime_error("ncmp decompressor has init symbol");
          }
          if (!f.main().name.empty()) {
            throw runtime_error("ncmp decompressor has main symbol");
          }
          if (!f.term().name.empty()) {
            throw runtime_error("ncmp decompressor has term symbol");
          }
          const auto& exports = f.exports();
          if (exports.size() != 1) {
            throw runtime_error("ncmp decompressor does not export exactly one symbol");
          }

          // The start symbol is actually a transition vector, which is the code
          // addr followed by the desired value in r2
          string start_symbol_name = "<ncmp>:" + exports.begin()->second.name;
          uint32_t start_symbol_addr = mem->get_symbol_addr(start_symbol_name.c_str());
          entry_pc = mem->read_u32b(start_symbol_addr);
          entry_r2 = mem->read_u32b(start_symbol_addr + 4);

          if (verbose) {
            fprintf(stderr, "ncmp entry pc is %08" PRIX32 " with r2 = %08" PRIX32 "\n",
                entry_pc, entry_r2);
          }

        } else {
          throw runtime_error("decompressor resource is not dcmp or ncmp");
        }

        size_t stack_region_size = 1024 * 16; // 16KB should be enough
        size_t output_region_size = header.decompressed_size + output_extra_bytes;
        // TODO: Looks like some decompressors expect zero bytes after the
        // compressed input? Find out if this is actually true and fix it if not.
        size_t input_region_size = res->data.size() + 0x100;
        // TODO: This is probably way too big; probably we should use
        // ((data.size() * 256) / working_buffer_fractional_size) instead here?
        size_t working_buffer_region_size = res->data.size() * 256;

        // Set up data memory regions. Slightly awkward assumption: decompressed
        // data is never more than 256 times the size of the input data.
        uint32_t stack_addr = 0x10000000;
        mem->allocate_at(stack_addr, stack_region_size);
        if (!stack_addr) {
          throw runtime_error("cannot allocate stack region");
        }
        uint32_t output_addr = 0x20000000;
        mem->allocate_at(output_addr, output_region_size);
        if (!output_addr) {
          throw runtime_error("cannot allocate output region");
        }
        uint32_t working_buffer_addr = 0x80000000;
        mem->allocate_at(working_buffer_addr, working_buffer_region_size);
        if (!working_buffer_addr) {
          throw runtime_error("cannot allocate working buffer region");
        }
        uint32_t input_addr = 0xC0000000;
        mem->allocate_at(input_addr, input_region_size);
        if (!input_addr) {
          throw runtime_error("cannot allocate input region");
        }
        if (verbose) {
          fprintf(stderr, "memory:\n");
          fprintf(stderr, "  stack region at %08" PRIX32 ":%zX\n", stack_addr, stack_region_size);
          fprintf(stderr, "  output region at %08" PRIX32 ":%zX\n", output_addr, output_region_size);
          fprintf(stderr, "  working region at %08" PRIX32 ":%zX\n", working_buffer_addr, working_buffer_region_size);
          fprintf(stderr, "  input region at %08" PRIX32 ":%zX\n", input_addr, input_region_size);
        }
        mem->memcpy(input_addr, res->data.data(), res->data.size());

        uint64_t execution_start_time;
        if (is_ppc) {
          // Set up header in stack region
          uint32_t return_addr = stack_addr + stack_region_size - sizeof(PPC32DecompressorInputHeader) + offsetof(PPC32DecompressorInputHeader, set_r2_opcode);
          auto* input_header = mem->at<PPC32DecompressorInputHeader>(
              stack_addr + stack_region_size - sizeof(PPC32DecompressorInputHeader));
          input_header->saved_r1 = 0xAAAAAAAA;
          input_header->saved_cr = 0x00000000;
          input_header->saved_lr = return_addr;
          input_header->reserved1 = 0x00000000;
          input_header->reserved2 = 0x00000000;
          input_header->saved_r2 = entry_r2;
          input_header->unused[0] = 0x00000000;
          input_header->unused[1] = 0x00000000;
          input_header->set_r2_opcode = 0x3840FFFF; // li r2, -1
          input_header->syscall_opcode = 0x44000002; // sc

          // Create emulator
          shared_ptr<InterruptManager> interrupt_manager(new InterruptManager());
          PPC32Emulator emu(mem);
          emu.set_interrupt_manager(interrupt_manager);

          // Set up registers
          auto& regs = emu.registers();
          regs.r[1].u = stack_addr + stack_region_size - sizeof(PPC32DecompressorInputHeader);
          regs.r[2].u = entry_r2;
          regs.r[3].u = input_addr + sizeof(CompressedResourceHeader);
          regs.r[4].u = output_addr;
          regs.r[5].u = (header.header_version == 9) ? input_addr : working_buffer_addr;
          regs.r[6].u = input_region_size - sizeof(CompressedResourceHeader);
          regs.lr = return_addr;
          regs.pc = entry_pc;
          if (verbose) {
            fprintf(stderr, "initial stack contents (input header data):\n");
            print_data(stderr, input_header, sizeof(*input_header), regs.r[1].u);
          }

          // Set up debugger
          shared_ptr<EmulatorDebugger<PPC32Emulator>> debugger;
          if (trace_execution || debug_execution) {
            debugger.reset(new EmulatorDebugger<PPC32Emulator>());
            debugger->bind(emu);
            debugger->state.mode = debug_execution ? DebuggerMode::STEP : DebuggerMode::TRACE;
          }

          // Set up environment
          emu.set_syscall_handler([&](PPC32Emulator& emu) -> void {
            auto& regs = emu.registers();
            // We don't support any syscalls in PPC mode - the only syscall that
            // should occur is the one at the end of emulation, when r2 == -1.
            if (regs.r[2].u != 0xFFFFFFFF) {
              throw runtime_error("unimplemented syscall");
            }
            throw PPC32Emulator::terminate_emulation();
          });

          // Run the decompressor
          execution_start_time = now();
          try {
            emu.execute();
          } catch (const exception& e) {
            if (verbose) {
              uint64_t diff = now() - execution_start_time;
              float duration = static_cast<float>(diff) / 1000000.0f;
              fprintf(stderr, "powerpc decompressor execution failed (%gsec): %s\n", duration, e.what());
            }
            throw;
          }

        } else {
          // Set up header in stack region
          auto* input_header = mem->at<M68KDecompressorInputHeader>(
              stack_addr + stack_region_size - sizeof(M68KDecompressorInputHeader));
          input_header->return_addr = stack_addr + stack_region_size - sizeof(M68KDecompressorInputHeader) + offsetof(M68KDecompressorInputHeader, reset_opcode);
          if (header.header_version == 9) {
            input_header->args.v9.data_size = input_region_size - sizeof(CompressedResourceHeader);
            input_header->args.v9.source_resource_header = input_addr;
            input_header->args.v9.dest_buffer_addr = output_addr;
            input_header->args.v9.source_buffer_addr = input_addr + sizeof(CompressedResourceHeader);
          } else {
            input_header->args.v8.data_size = input_region_size - sizeof(CompressedResourceHeader);
            input_header->args.v8.working_buffer_addr = working_buffer_addr;
            input_header->args.v8.dest_buffer_addr = output_addr;
            input_header->args.v8.source_buffer_addr = input_addr + sizeof(CompressedResourceHeader);
          }

          input_header->reset_opcode = 0x4E70;
          input_header->unused = 0x0000;

          // Create emulator
          M68KEmulator emu(mem);

          // Set up registers
          auto& regs = emu.registers();
          regs.a[7] = stack_addr + stack_region_size - sizeof(M68KDecompressorInputHeader);
          regs.pc = entry_pc;
          if (verbose) {
            fprintf(stderr, "initial stack contents (input header data):\n");
            print_data(stderr, input_header, sizeof(*input_header), regs.a[7]);
          }

          // Set up debugger
          shared_ptr<EmulatorDebugger<M68KEmulator>> debugger;
          if (trace_execution || debug_execution) {
            debugger.reset(new EmulatorDebugger<M68KEmulator>());
            debugger->bind(emu);
            debugger->state.mode = debug_execution ? DebuggerMode::STEP : DebuggerMode::TRACE;
          }

          // Set up environment
          unordered_map<uint16_t, uint32_t> trap_to_call_stub_addr;
          emu.set_syscall_handler([&](M68KEmulator& emu, uint16_t opcode) -> void {
            auto& regs = emu.registers();
            uint16_t trap_number;
            bool auto_pop = false;
            uint8_t flags = 0;

            if (opcode & 0x0800) {
              trap_number = opcode & 0x0BFF;
              auto_pop = opcode & 0x0400;
            } else {
              trap_number = opcode & 0x00FF;
              flags = (opcode >> 9) & 3;
            }

            // We only support a few traps here. Specifically:
            // - System dcmp 2 uses BlockMove
            // - Ben Mickaelian's self-modifying decompressor uses
            //   GetTrapAddress, but it suffices to simulate the asked-for traps
            //   with stubs
            if (trap_number == 0x002E) { // BlockMove
              // A0 = src, A1 = dst, D0 = size
              const void* src = mem->at<void>(regs.a[0], regs.d[0].u);
              void* dst = mem->at<void>(regs.a[1], regs.d[0].u);
              memcpy(dst, src, regs.d[0].u);
              regs.d[0].u = 0; // Result code (success)

            } else if (trap_number == 0x0046) { // GetTrapAddress
              uint16_t trap_number = regs.d[0].u & 0xFFFF;
              if ((trap_number > 0x4F) && (trap_number != 0x54) && (trap_number != 0x57)) {
                trap_number |= 0x0800;
              }

              // If it already has a call routine, just return that
              try {
                regs.a[0] = trap_to_call_stub_addr.at(trap_number);
                if (verbose) {
                  fprintf(stderr, "GetTrapAddress: using cached call stub for trap %04hX -> %08" PRIX32 "\n",
                      trap_number, regs.a[0]);
                }

              } catch (const out_of_range&) {
                // Create a call stub
                uint32_t call_stub_addr = mem->allocate(4);
                be_uint16_t* call_stub = mem->at<be_uint16_t>(call_stub_addr, 4);
                trap_to_call_stub_addr.emplace(trap_number, call_stub_addr);
                call_stub[0] = 0xA000 | trap_number; // A-trap opcode
                call_stub[1] = 0x4E75; // rts

                // Return the address
                regs.a[0] = call_stub_addr;

                if (verbose) {
                  fprintf(stderr, "GetTrapAddress: created call stub for trap %04hX -> %08" PRIX32 "\n",
                      trap_number, regs.a[0]);
                }
              }

            } else if (verbose) {
              if (trap_number & 0x0800) {
                fprintf(stderr, "warning: skipping unimplemented toolbox trap (num=%hX, auto_pop=%s)\n",
                    static_cast<uint16_t>(trap_number & 0x0BFF), auto_pop ? "true" : "false");
              } else {
                fprintf(stderr, "warning: skipping unimplemented os trap (num=%hX, flags=%hhu)\n",
                    static_cast<uint16_t>(trap_number & 0x00FF), flags);
              }
            }
          });

          // Run the decompressor
          execution_start_time = now();
          try {
            emu.execute();
          } catch (const exception& e) {
            if (verbose) {
              uint64_t diff = now() - execution_start_time;
              float duration = static_cast<float>(diff) / 1000000.0f;
              fprintf(stderr, "m68k decompressor execution failed (%gsec): %s\n", duration, e.what());
              emu.print_state(stderr);
            }
            throw;
          }
        }

        if (verbose) {
          uint64_t diff = now() - execution_start_time;
          float duration = static_cast<float>(diff) / 1000000.0f;
          fprintf(stderr, "note: decompressed resource using %s %hd in %g seconds (%zu -> %" PRIu32 " bytes)\n",
              (dcmp_res->type == RESOURCE_TYPE_dcmp) ? "dcmp" : "ncmp", dcmp_res->id,
              duration, res->data.size(), header.decompressed_size.load());
        }

        res->data = mem->read(output_addr, header.decompressed_size);
        res->flags = (res->flags & ~ResourceFlag::FLAG_COMPRESSED) | ResourceFlag::FLAG_DECOMPRESSED;
        return;
      }

    } catch (const exception& e) {
      if (verbose) {
        fprintf(stderr, "decompressor implementation %zu of %zu failed: %s\n",
            z + 1, dcmp_resources.size(), e.what());
      }
    }
  }

  throw runtime_error("no decompressor succeeded");
}
