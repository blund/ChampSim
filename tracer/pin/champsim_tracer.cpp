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

/*! @file
 *  This is an example of the PIN tool that demonstrates some basic PIN APIs
 *  and could serve as the starting point for developing your first PIN tool
 */

#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "../../inc/trace_instruction.h"
#include "pin.H"
#include "../../../LuaJIT/src/lj_bc.h"

using trace_instr_format_t = luajit_instr;

/* ================================================================== */
// Global variables
/* ================================================================== */

UINT64 instrCount = 0;

std::ofstream outfile;

trace_instr_format_t curr_instr;

ADDRINT base_address;

/* ===================================================================== */
// Command line switches
/* ===================================================================== */
KNOB<std::string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "champsim.trace", "specify file name for Champsim tracer output");

KNOB<UINT64> KnobSkipInstructions(KNOB_MODE_WRITEONCE, "pintool", "s", "0", "How many instructions to skip before tracing begins");

KNOB<UINT64> KnobTraceInstructions(KNOB_MODE_WRITEONCE, "pintool", "t", "1000000", "How many instructions to trace");

/* ===================================================================== */
// Utilities
/* ===================================================================== */

/*!
 *  Print out help message.
 */
INT32 Usage()
{
  std::cerr << "This tool creates a register and memory access trace" << std::endl
            << "Specify the output trace file with -o" << std::endl
            << "Specify the number of instructions to skip before tracing with -s" << std::endl
            << "Specify the number of instructions to trace with -t" << std::endl
            << std::endl;

  std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;

  return -1;
}

/* ===================================================================== */
// Analysis routines
/* ===================================================================== */

// Callback for loaded images - to find the base and high of the program, and thus calculate offsets

VOID RegisterTraceStart(ADDRINT address) {
  puts(" [tracer] - setting mode to TRACING");
  curr_instr.int_state = TRACING;
}

VOID Image(IMG img, VOID* v)
{
  // Store base address so we can identitify addresses
  if (IMG_IsMainExecutable(img)) {
    base_address = IMG_LowAddress(img);
    printf("based address: %lx\n", base_address);
  }
}



void ResetCurrentInstruction(VOID* ip)
{
  curr_instr = {};
  curr_instr.ip = (unsigned long long int)ip;
  curr_instr.int_state = IRRELEVANT;
}

BOOL ShouldWrite()
{
  ++instrCount;
  return (instrCount > KnobSkipInstructions.Value()) && (instrCount <= (KnobTraceInstructions.Value() + KnobSkipInstructions.Value()));
}

void WriteCurrentInstruction()
{
  typename decltype(outfile)::char_type buf[sizeof(trace_instr_format_t)];
  std::memcpy(buf, &curr_instr, sizeof(trace_instr_format_t));
  outfile.write(buf, sizeof(trace_instr_format_t));
}

void BranchOrNot(UINT32 taken)
{
  curr_instr.is_branch = 1;
  curr_instr.branch_taken = taken;
}

template <typename T>
void WriteToSet(T* begin, T* end, UINT32 r)
{
  auto set_end = std::find(begin, end, 0);
  auto found_reg = std::find(begin, set_end, r); // check to see if this register is already in the list
  *found_reg = r;
}

ADDRINT addr_ring[16];
int ring_index = 0;

void ring_add(ADDRINT addr) {
  addr_ring[ring_index] = addr;
  ring_index = (ring_index+1) % 16;
  //printf(" - %d\n", ring_index);
}


VOID MemoryReadStoreBaseAddress(ADDRINT addr) {
  ring_add(addr);
}

/* ===================================================================== */
// Instrumentation callbacks
/* ===================================================================== */
// @BL
UINT64 dispatch_base = 0xc0de000fe0;
VOID CheckIfDispatch(ADDRINT base, ADDRINT opcode) {
  // @BL(TODO) - ensure that the ring buffer works correctly....
  if (base == dispatch_base) {

    // find last addr read in the ring addr:
    //ADDRINT last_addr;
    int ring_end = (ring_index + 1) % 16;
    for (int i = ring_index; i % 16 != ring_end; i--) {
      //printf(" -- %d, %d\n", i, ring_end);
      ADDRINT addr = addr_ring[i%16];
      if (addr != dispatch_base) {
	//printf("%lx\n", addr);
	//puts("bom!");
	break;
      }
    }

    switch(opcode) {
    case BC_JFORI:
    case BC_JFORL:
    case BC_JITERL:
    case BC_JLOOP:
    case BC_JFUNCF:
    case BC_JFUNCV:
      // JIT code will be executed
      puts(" [tracer] - setting mode to JIT");
      curr_instr.int_state = JIT;
      break;
    default:
      // Normal interpreter execution
      assert(opcode >= 0 && opcode <= 243); // Make sure the opcode is actually an opcode!
      //puts(" [tracer] - setting mode to INTERPRETER");
      curr_instr.int_state = INTERPRETER;
      break;
    }
  }
}

VOID DetectedTracing(ADDRINT ip)
{
  puts(" [tracer] - setting mode to TRACING");
  curr_instr.int_state = TRACING;
}

ADDRINT target_address = 0x000000ec30;

// Is called for every instruction and instruments reads and writes
VOID Instruction(INS ins, VOID* v)
{
  // begin each instruction with this function
  INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)ResetCurrentInstruction, IARG_INST_PTR, IARG_END);

  if (INS_Address(ins) == base_address+target_address) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)DetectedTracing,
		   IARG_INST_PTR,  // Instruction pointer (address)
		   IARG_END);
  }
  
  if (INS_IsMemoryRead(ins)) {
      REG base_reg  = INS_MemoryBaseReg(ins);
      if(REG_valid(base_reg)) {
	INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryReadStoreBaseAddress,
		       IARG_REG_VALUE, base_reg,
		       IARG_END);

      }
  }
  
  // @BL - instrument branches to check if they are the vm jumping to the dispatch table
  if (INS_IsBranch(ins)) {
    if (INS_IsIndirectControlFlow(ins)) {
      // Effective address = Displacement + BaseReg + IndexReg * Scale
      // We just need base and index, since they correspond to dispatch table and opcode :)
      REG base_reg  = INS_MemoryBaseReg(ins);
      REG index_reg = INS_MemoryIndexReg(ins);
      //UINT32 scale  = INS_MemoryScale(ins);
      if(REG_valid(base_reg) && REG_valid(index_reg)) {
	INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CheckIfDispatch,
		       IARG_REG_VALUE, base_reg,
		       IARG_REG_VALUE, index_reg,
		       //IARG_UINT64,    scale,
		       IARG_END);
      }
    }
  }

  // instrument branch instructions
  if (INS_IsBranch(ins))
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)BranchOrNot, IARG_BRANCH_TAKEN, IARG_END);

  // instrument register reads
  UINT32 readRegCount = INS_MaxNumRRegs(ins);
  for (UINT32 i = 0; i < readRegCount; i++) {
    UINT32 regNum = INS_RegR(ins, i);
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteToSet<unsigned char>, IARG_PTR, curr_instr.source_registers, IARG_PTR,
                   curr_instr.source_registers + NUM_INSTR_SOURCES, IARG_UINT32, regNum, IARG_END);
  }

  // instrument register writes
  UINT32 writeRegCount = INS_MaxNumWRegs(ins);
  for (UINT32 i = 0; i < writeRegCount; i++) {
    UINT32 regNum = INS_RegW(ins, i);
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteToSet<unsigned char>, IARG_PTR, curr_instr.destination_registers, IARG_PTR,
                   curr_instr.destination_registers + NUM_INSTR_DESTINATIONS, IARG_UINT32, regNum, IARG_END);
  }

  // instrument memory reads and writes
  UINT32 memOperands = INS_MemoryOperandCount(ins);

  // Iterate over each memory operand of the instruction.
  for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
    if (INS_MemoryOperandIsRead(ins, memOp))
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteToSet<unsigned long long int>, IARG_PTR, curr_instr.source_memory, IARG_PTR,
                     curr_instr.source_memory + NUM_INSTR_SOURCES, IARG_MEMORYOP_EA, memOp, IARG_END);
    if (INS_MemoryOperandIsWritten(ins, memOp))
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteToSet<unsigned long long int>, IARG_PTR, curr_instr.destination_memory, IARG_PTR,
                     curr_instr.destination_memory + NUM_INSTR_DESTINATIONS, IARG_MEMORYOP_EA, memOp, IARG_END);
  }

  // finalize each instruction with this function
  INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite, IARG_END);
  INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteCurrentInstruction, IARG_END);
}

/*!
 * Print out analysis results.
 * This function is called when the application exits.
 * @param[in]   code            exit code of the application
 * @param[in]   v               value specified by the tool in the
 *                              PIN_AddFiniFunction function call
 */
VOID Fini(INT32 code, VOID* v) { outfile.close(); }

/*!
 * The main procedure of the tool.
 * This function is called when the application image is loaded but not yet started.
 * @param[in]   argc            total number of elements in the argv array
 * @param[in]   argv            array of command line arguments,
 *                              including pin -t <toolname> -- ...
 */
int main(int argc, char* argv[])
{
  // Initialize PIN library. Print help message if -h(elp) is specified
  // in the command line or the command line is invalid

  PIN_InitSymbols();

  if (PIN_Init(argc, argv))
    return Usage();

  outfile.open(KnobOutputFile.Value().c_str(), std::ios_base::binary | std::ios_base::trunc);
  if (!outfile) {
    std::cout << "Couldn't open output trace file. Exiting." << std::endl;
    exit(1);
  }

  // @BL - register function to instrument image
  IMG_AddInstrumentFunction(Image, 0);

  // Register function to be called to instrument instructions
  INS_AddInstrumentFunction(Instruction, 0);

  // Register function to be called when the application exits
  PIN_AddFiniFunction(Fini, 0);

  // Start the program, never returns
  PIN_StartProgram();

  return 0;
}
