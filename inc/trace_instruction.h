/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TRACE_INSTRUCTION_H
#define TRACE_INSTRUCTION_H

#include <limits>
#include <string>

enum program_state {
  ERR  = -1,
  STATE_IRRELEVANT = 0,
  STATE_INTERPRET,
  STATE_JIT,
  STATE_TRACE,
};
/*
const char* program_state_to_string(program_state ps) {
  switch (ps) {
  case ERR: return "ERR";
  case STATE_IRRELEVANT: return "STATE_IRRELEVANT";
  case STATE_INTERPRET: return "STATE_INTERPRET";
  case STATE_JIT: return "STATE_JIT";
  case STATE_TRACE: return "STATE_TRACE";
  }
  return "";
}
*/


// special registers that help us identify branches
namespace champsim
{
constexpr char REG_STACK_POINTER = 6;
constexpr char REG_FLAGS = 25;
constexpr char REG_INSTRUCTION_POINTER = 26;
} // namespace champsim

// instruction format
constexpr std::size_t NUM_INSTR_DESTINATIONS_SPARC = 4;
constexpr std::size_t NUM_INSTR_DESTINATIONS = 2;
constexpr std::size_t NUM_INSTR_SOURCES = 4;

struct input_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];           // input memory
};

struct cloudsuite_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS_SPARC]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];                 // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS_SPARC]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];                 // input memory

  unsigned char asid[2];
};

typedef enum {
  LJ_TRACE_IDLE,	/* Trace compiler idle. */
  LJ_TRACE_ACTIVE = 0x10,
  LJ_TRACE_RECORD,	/* Bytecode recording active. */
  LJ_TRACE_RECORD_1ST,	/* Record 1st instruction, too. */
  LJ_TRACE_START,	/* New trace started. */
  LJ_TRACE_END,		/* End of trace. */
  LJ_TRACE_ASM,		/* Assemble trace. */
  LJ_TRACE_ERR		/* Trace aborted with error. */
} TraceState;

struct luajit_instr {
  // instruction pointer or PC (Program Counter)
  unsigned long long ip;

  // branch info
  unsigned char is_branch;
  unsigned char branch_taken;

  unsigned char destination_registers[NUM_INSTR_DESTINATIONS]; // output registers
  unsigned char source_registers[NUM_INSTR_SOURCES];           // input registers

  unsigned long long destination_memory[NUM_INSTR_DESTINATIONS]; // output memory
  unsigned long long source_memory[NUM_INSTR_SOURCES];           // input memory

  program_state state;
  TraceState trace_state;
};

#endif
