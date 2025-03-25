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
  uint64_t snapshot_rate = 100000;
  std::string json_file_name;
  std::vector<std::string> trace_names;
  std::string replacement_trace_name;
  std::string output_path = "stats.csv";

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

  app.add_option("--snapshot-rate", snapshot_rate, "How many instructions should be executed between making stat snapshots");

  app.add_option("--replacement-trace", replacement_trace_name, "Optional trace to use for simulation, replacing original trace");

  auto json_option =
      app.add_option("--json", json_file_name, "The name of the file to receive JSON output. If no name is specified, stdout will be used")->expected(0, 1);

  app.add_option("traces", trace_names, "The paths to the traces")->required()->expected(NUM_CPUS)->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  const bool warmup_given = (warmup_instr_option->count() > 0) || (deprec_warmup_instr_option->count() > 0);
  const bool simulation_given = (sim_instr_option->count() > 0) || (deprec_sim_instr_option->count() > 0);

  if (deprec_warmup_instr_option->count() > 0)
    fmt::print("WARNING: option --warmup_instructions is deprecated. Use --warmup-instructions instead.\n");

  if (deprec_sim_instr_option->count() > 0)
    fmt::print("WARNING: option --simulation_instructions is deprecated. Use --simulation-instructions instead.\n");


  // @BL
  bool is_luajit = true;
 
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


  // @BL - read list of missed branches to predict perfectly
  {
    O3_CPU& cpu = gen_environment.cpu_view()[0];

    std::vector<int> all_missed_branches;

    std::ifstream file{"/cluster/home/borgelu/master/library/champsim/20250319-1234_tage-scl/guess_correctly.txt"};
    if (!file) {
      puts("ERROR, could not read 'missed_branches.txt'");
      exit(69);
    }

    std::string line;
    int entries = 0;
    while (std::getline(file, line)) {

      std::istringstream iss(line);
      std::string addr_str, count_str;

      // Read the address (hex) and count (decimal), separated by ","
      if (std::getline(iss, addr_str, ',')) {
	int addr = std::stoi(addr_str);  // Parse address as hex
	all_missed_branches.push_back(addr); // Store the pair
      } else {
	puts("ERROR, could not read 'missed_branches.txt'");
	exit(69);
      }
      entries++;
    }

    file.close();

    int thresh = (int)(0.005*entries);
    for (int i = 0; i < thresh; i++) {
      cpu.perfect_branches.insert(all_missed_branches[i]);
    }
  }


  
  // @BL - her får vi ut en vektor av phase stats, som består i alle fasene vi har kjørt gjennom
  // programmets kjøretid
  std::vector<champsim::phase_stats> phase_stats = champsim::main(gen_environment, phases, traces, replacement);
  assert(phase_stats.size() == 1 && "We assume there is only one CPU instance");

  printf("Caches in phase_stats: %ld\n", phase_stats[0].roi_cache_stats.size());
  printf("CPUs in phase_stats: %ld\n", phase_stats[0].roi_cpu_stats.size());

  printf("cache_stats len = %ld\n", phase_stats[0].snapshots[0].cache.size());


  O3_CPU& cpu = gen_environment.cpu_view()[0];

  // Convert map to vector of pairs (IP, Miss Count)
  /*
  std::vector<std::pair<int, int>> sorted_misses(cpu.missed_branches.begin(), cpu.missed_branches.end());

  // Sort the vector by miss count in descending order
  std::sort(sorted_misses.begin(), sorted_misses.end(),
            [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
              return a.second > b.second; // Sort by miss count (higher first)
            });

  std::ofstream missed_branches_file{"missed_branches.txt"};
  for (std::pair<int,int> pair : sorted_misses) {
    missed_branches_file << pair.first;
    missed_branches_file << ",";
    missed_branches_file << pair.second;
    missed_branches_file << "\n";
    //printf("%x, %d\n", pair.first, pair.second);
  }
  missed_branches_file.close();
  */
  {
    // Print branch info
    std::ofstream out{"branch_info.txt"};
    out << "key,total,misses,type\n";
    for (const auto& [key, value] : cpu.branch_miss_info) {
      out << key << "," << value.total << "," << value.misses << "," << static_cast<int>(value.type) << "\n";
    }
    out.close();
  }

  std::ofstream output_file{output_path};
  for (auto& snapshot : phase_stats[0].snapshots) {
    cpu_stats                cpu_stats   = snapshot.cpu;
    std::vector<cache_stats> cache_stats = snapshot.cache;


    // Which state interpreter had for this section
    output_file << snapshot.state;
    output_file << ",";

    // How many instrs this section held
    output_file << cpu_stats.instrs();
    output_file << ",";

    // L1i fetch packets
    output_file << get_fetch_packet_count(champsim::CACHE_TYPES::L1I, cache_stats);
    output_file << ",";

    // L1i fetch packets
    output_file << get_fetch_packet_count(champsim::CACHE_TYPES::L1D, cache_stats);
    output_file << ",";
    
    /*
    // How many pfrs were done in this section
    output_file << cache_stats[3].pf_requested;
    output_file << ",";
    output_file << cache_stats[3].pf_useless;
    output_file << ",";

    output_file << cache_stats[4].pf_requested;
    output_file << ",";
    output_file << cache_stats[4].pf_useless;
    output_file << ",";

    output_file << cpu_stats.l1d_fetch_packet_count;
    output_file << ",";

    output_file << cpu_stats.l1i_fetch_packet_count;
    output_file << ",";

    output_file << cpu_stats.btb_fetch_packet_count;
    output_file << ",";

    */
    
    output_file << "\n";

   }
  output_file.close();

/*
  std::cout << "Current path is " << std::filesystem::current_path() << '\n';
  std::ofstream cpu_file{"cpu_stats.csv"};

  printf("snapshot len: %ld\n", phase_stats[0].snapshots.size());

  for (auto& snapshot : phase_stats[0].snapshots) {
    auto stats = snapshot.cpu;
    std::array<std::pair<std::string, std::size_t>, 6> types{
      {std::pair{"BRANCH_DIRECT_JUMP", BRANCH_DIRECT_JUMP}, std::pair{"BRANCH_INDIRECT", BRANCH_INDIRECT}, std::pair{"BRANCH_CONDITIONAL", BRANCH_CONDITIONAL},
       std::pair{"BRANCH_DIRECT_CALL", BRANCH_DIRECT_CALL}, std::pair{"BRANCH_INDIRECT_CALL", BRANCH_INDIRECT_CALL},
       std::pair{"BRANCH_RETURN", BRANCH_RETURN}}};

    auto total_branch = std::ceil(
				  std::accumulate(std::begin(types), std::end(types), 0ll, [tbt = stats.total_branch_types](auto acc, auto next) { return acc + tbt[next.second]; }));

    auto total_mispredictions = std::ceil(
					  std::accumulate(std::begin(types), std::end(types), 0ll, [btm = stats.branch_type_misses](auto acc, auto next) { return acc + btm[next.second]; }));

  std::vector<double> mpkis;
  std::transform(std::begin(stats.branch_type_misses), std::end(stats.branch_type_misses), std::back_inserter(mpkis),
                 [instrs = stats.instrs()](auto x) { return 1000.0 * std::ceil(x) / std::ceil(instrs); });
    
    // instructions
    cpu_file << stats.instrs();
    cpu_file << ",";
    // cycles
    cpu_file << stats.cycles();
    cpu_file << ",";
    // branch prediction accuracy
    cpu_file << (100.0 * std::ceil(total_branch - total_mispredictions)) / total_branch;
    cpu_file << ",";
    // mpki
    cpu_file << (1000.0 * total_mispredictions) / std::ceil(stats.instrs());
    cpu_file << ",";
    // Avg ROB occupancy at mispredict
    cpu_file << std::ceil(stats.total_rob_occupancy_at_branch_mispredict) / std::ceil(total_mispredictions);
    cpu_file << ",";
    // Mispredicts
    for (auto& mpki : mpkis) {
      cpu_file << mpki;
      cpu_file << ",";
    }
    cpu_file << "\n";
  }
  cpu_file.close();
  */


  /*
  
  std::ofstream cache_file{"cache_stats.csv"};
  for (auto& snapshot : phase_stats[0].snapshots) {
    cache_file << snapshot.state;
    cache_file << ",";
    for (auto& stats : snapshot.cache) {

      constexpr std::array<std::pair<std::string_view, std::size_t>, 5> types{
	{std::pair{"LOAD", champsim::to_underlying(access_type::LOAD)}, std::pair{"RFO", champsim::to_underlying(access_type::RFO)},
	 std::pair{"PREFETCH", champsim::to_underlying(access_type::PREFETCH)}, std::pair{"WRITE", champsim::to_underlying(access_type::WRITE)},
	 std::pair{"TRANSLATION", champsim::to_underlying(access_type::TRANSLATION)}}};


      for (std::size_t cpu = 0; cpu < NUM_CPUS; ++cpu) {
	uint64_t TOTAL_HIT = 0, TOTAL_MISS = 0;
	for (const auto& type : types) {
	  TOTAL_HIT  += stats.hits.at(type.second).at(cpu);
	  TOTAL_MISS += stats.misses.at(type.second).at(cpu);
	}

	cache_file << stats.name;
	cache_file << ",";
	// access
	cache_file << TOTAL_HIT + TOTAL_MISS;
	cache_file << ",";
	cache_file << TOTAL_HIT;
	cache_file << ",";
	cache_file << TOTAL_MISS;
	cache_file << ",";

	for (const auto& type : types) {
	  //cache_file << type.first;
	  //cache_file << ",";

	  // access
	  cache_file << stats.hits[type.second][cpu] + stats.misses[type.second][cpu];
	  cache_file << ",";
	  // hit
	  cache_file << stats.hits[type.second][cpu];
	  cache_file << ",";
	  // miss
	  cache_file << stats.misses[type.second][cpu];
	  cache_file << ",";
	}

	cache_file << stats.pf_requested;
	cache_file << ",";
	cache_file << stats.pf_issued;
	cache_file << ",";
	cache_file << stats.pf_useful;
	cache_file << ",";
	cache_file << stats.pf_useless;
	cache_file << ",";
	cache_file << stats.avg_miss_latency;
	cache_file << ",";
      }
    }
    cache_file << "\n";
  }
  cache_file.close();
*/

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
