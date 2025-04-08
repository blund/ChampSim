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

#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>


#include "champsim.h"
#include <nlohmann/json.hpp>
#include "champsim_constants.h"
#include "core_inst.inc"
#include "phase_info.h"
#include "stats_printer.h"
#include "tracereader.h"
#include "vmem.h"
#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include <iomanip>

namespace champsim
{
  std::vector<phase_stats> main(environment& env, std::vector<phase_info>& phases,
				std::vector<tracereader>& traces, std::optional<tracereader>& replacement);
}

uint64_t get_fetch_packet_count(int index, std::vector<cache_stats> cache_stats) {
  constexpr std::array<std::pair<std::string_view, std::size_t>, 5> types{
    {std::pair{"LOAD", champsim::to_underlying(access_type::LOAD)}, std::pair{"RFO", champsim::to_underlying(access_type::RFO)},
     std::pair{"PREFETCH", champsim::to_underlying(access_type::PREFETCH)}, std::pair{"WRITE", champsim::to_underlying(access_type::WRITE)},
     std::pair{"TRANSLATION", champsim::to_underlying(access_type::TRANSLATION)}}};

  uint64_t TOTAL_ACCESS = 0, TOTAL_HIT = 0, TOTAL_MISS = 0;
  for (const auto& type : types) {
    TOTAL_HIT  += cache_stats[index].hits.at(type.second).at(0);
    TOTAL_MISS += cache_stats[index].misses.at(type.second).at(0);
  }
  TOTAL_ACCESS = TOTAL_HIT + TOTAL_MISS;
  uint64_t fetch_packets = TOTAL_ACCESS - cache_stats[index].pf_requested;


  return fetch_packets;
}

int main(int argc, char** argv)
{
  champsim::configured::generated_environment gen_environment{};

  CLI::App app{"A microarchitecture simulator for research and education"};

  bool knob_cloudsuite{false};
  uint64_t warmup_instructions = 0;
  uint64_t simulation_instructions = std::numeric_limits<uint64_t>::max();
  std::string json_file_name;
  std::vector<std::string> trace_names;
  std::string replacement_trace_name;
  std::string output_path = "stats.csv";

  std::string snapshot_folder;
  uint64_t snapshot_rate = 1000000;
  
  auto set_heartbeat_callback = [&](auto) {
    for (O3_CPU& cpu : gen_environment.cpu_view())
      cpu.show_heartbeat = false;
  };


  app.add_flag("-c,--cloudsuite", knob_cloudsuite, "Read all traces using the cloudsuite format");
  app.add_flag("--hide-heartbeat", set_heartbeat_callback, "Hide the heartbeat output");
  auto warmup_instr_option = app.add_option("-w,--warmup-instructions", warmup_instructions, "The number of instructions in the warmup phase");

  app.add_option("--output", output_path, "Where the resulting csv should be output.");

  auto deprec_warmup_instr_option =
      app.add_option("--warmup_instructions", warmup_instructions, "[deprecated] use --warmup-instructions instead")->excludes(warmup_instr_option);
  auto sim_instr_option = app.add_option("-i,--simulation-instructions", simulation_instructions,
                                         "The number of instructions in the detailed phase. If not specified, run to the end of the trace.");
  auto deprec_sim_instr_option =
      app.add_option("--simulation_instructions", simulation_instructions, "[deprecated] use --simulation-instructions instead")->excludes(sim_instr_option);

  app.add_option("--replacement-trace", replacement_trace_name, "Optional trace to use for simulation, replacing original trace");

  auto json_option =
      app.add_option("--json", json_file_name, "The name of the file to receive JSON output. If no name is specified, stdout will be used")->expected(0, 1);

  auto snapshot_folder_option = app.add_option("--snapshot-folder", snapshot_folder,
                                           "Required. Where to output the snapshot files.")
                                ->expected(1, 1);

  app.add_option("--snapshot-rate", snapshot_rate, "How many instructions should be executed between making stat snapshots");


  app.add_option("traces", trace_names, "The paths to the traces")->required()->expected(NUM_CPUS)->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  const bool warmup_given = (warmup_instr_option->count() > 0) || (deprec_warmup_instr_option->count() > 0);
  const bool simulation_given = (sim_instr_option->count() > 0) || (deprec_sim_instr_option->count() > 0);
  const bool snapshot_folder_given = (snapshot_folder_option->count() == 1);

  if (deprec_warmup_instr_option->count() > 0)
    fmt::print("WARNING: option --warmup_instructions is deprecated. Use --warmup-instructions instead.\n");

  if (deprec_sim_instr_option->count() > 0)
    fmt::print("WARNING: option --simulation_instructions is deprecated. Use --simulation-instructions instead.\n");


  // @BL
  bool is_luajit = true;

  if (!snapshot_folder_given) {
    puts("No snapshot folder given!");
    return -1;
  }

  O3_CPU& cpu = gen_environment.cpu_view()[0];
  cpu.snapshot_folder = snapshot_folder;
  cpu.snapshot_rate = snapshot_rate;

  if (simulation_given && !warmup_given)
    warmup_instructions = simulation_instructions * 2 / 10;

  std::vector<champsim::tracereader> traces;
  std::transform(
      std::begin(trace_names), std::end(trace_names), std::back_inserter(traces),
      [knob_cloudsuite, is_luajit, repeat = simulation_given, i = uint8_t(0)](auto name) mutable { return get_tracereader(name, i++, knob_cloudsuite, is_luajit, repeat); });

  // During simulation, this trace will replace the original trace
  std::optional<champsim::tracereader> replacement = std::nullopt;
  if (!replacement_trace_name.empty()) {
    replacement = get_tracereader(replacement_trace_name, 0, false, true, 0);
  }

  std::vector<champsim::phase_info> phases{
      {champsim::phase_info{"Warmup", true, warmup_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names, 0},
       champsim::phase_info{"Simulation", false, simulation_instructions, std::vector<std::size_t>(std::size(trace_names), 0), trace_names, snapshot_rate}}};

  for (auto& p : phases)
    std::iota(std::begin(p.trace_index), std::end(p.trace_index), 0);

  
  fmt::print("\n*** ChampSim Multicore Out-of-Order Simulator ***\nWarmup Instructions: {}\nSimulation Instructions: {}\nNumber of CPUs: {}\nPage size: {}\n\n",
	   phases.at(0).length, phases.at(1).length, std::size(gen_environment.cpu_view()), PAGE_SIZE);

  std::vector<champsim::phase_stats> phase_stats = champsim::main(gen_environment, phases, traces, replacement);
  assert(phase_stats.size() == 1 && "We assume there is only one CPU instance");

  fmt::print("\nChampSim completed all CPUs\n\n");

  champsim::plain_printer{std::cout}.print(phase_stats);

  for (CACHE& cache : gen_environment.cache_view())
    cache.impl_prefetcher_final_stats();

  for (CACHE& cache : gen_environment.cache_view())
    cache.impl_replacement_final_stats();

  if (json_option->count() > 0) {
    if (json_file_name.empty()) {
      champsim::json_printer{std::cout}.print(phase_stats);
    } else {
      std::ofstream json_file{json_file_name};
      champsim::json_printer{json_file}.print(phase_stats);
    }
  }

  return 0;
}
