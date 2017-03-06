#include <castan/Internal/GenericCacheModel.h>

#include "../Core/TimingSolver.h"
#include "klee/CommandLine.h"
#include "klee/Internal/Support/Debug.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include <klee/Internal/Module/InstructionInfoTable.h>
#include <klee/Internal/Module/KInstruction.h>

#define BLOCK_BITS 6

static struct {
  unsigned int size;          // bytes
  unsigned int associativity; // ways
  char writeBack;             // 0 = write-through; 1 = write-back.
  unsigned int latency;       // cycles (if hit).
} cacheConfig[] = {
//     // Fake cache.
//     {4 << BLOCK_BITS, 2, 1, 1},
//     {16 << BLOCK_BITS, 1, 1, 8},
//     {64 << BLOCK_BITS, 1, 1, 32},
//     {0, 0, 0, 128},

//     // Fake cache with a single entry.
//     {1 << BLOCK_BITS, 1, 1, 32},
//     {0, 0, 0, 128},

// Intel(R) Core(TM) i7-2600S
#define CPU_CPI 1
#define CPU_HZ 2800000000
    {256 * 1024, 8, 1, 4},
    {1024 * 1024, 8, 1, 10},
    {8192 * 1024, 16, 1, 40},
    {0, 0, 0, 168},

    //     // Intel(R) Core(TM) i7-2600S - Just L3
    //     {8192 * 1024, 16, 1, 40},
    //     {0, 0, 0, 168},
};

namespace {
llvm::cl::opt<bool> WorstCaseSymIndices(
    "worst-case-sym-indices", llvm::cl::init(false),
    llvm::cl::desc("Pick values for symbolic indices that exercise worst case "
                   "cache scenarios (default=off)"));

llvm::cl::opt<unsigned>
    MaxLoops("max-loops", llvm::cl::init(0),
             llvm::cl::desc("Maximum number of loops to explore "
                            "(default: until cache state loops)"));
}

namespace castan {
GenericCacheModel::GenericCacheModel() {
  for (unsigned int level = 0; cacheConfig[level].size; level++) {
    klee::klee_message(
        "Modeling L%d cache of %d kiB as %d-way associative, %s.\n", level + 1,
        cacheConfig[level].size / 1024, cacheConfig[level].associativity,
        cacheConfig[level].writeBack ? "write-back" : "write-through");
  }
}

void GenericCacheModel::updateCache(uint64_t address, bool isWrite,
                                    uint8_t level) {
  // Check if accessing beyond last cache (DRAM).
  if (!cacheConfig[level].size) {
    loopStats.back().hitCount[level]++;
    //         klee::klee_message("  DRAM Access.");
    return;
  }

  uint64_t blockPtr = address >> BLOCK_BITS;
  uint32_t line =
      blockPtr % (cacheConfig[level].size / cacheConfig[level].associativity /
                  (1 << BLOCK_BITS));

  // Advance time counter for LRU algorithm.
  if (level == 0) {
    currentTime++;
  }

  // Check if cache hit.
  if (cache[level][line].count(blockPtr)) {
    if (isWrite && !cacheConfig[level].writeBack) {
      // Write-through to next level.
      updateCache(address, isWrite, level + 1);
    } else {
      // Write-back or read, don't propagate deeper.
      loopStats.back().hitCount[level]++;
    }

    // Update use time.
    cache[level][line][blockPtr].useTime = currentTime;
    // Read hit doesn't affect dirtiness.
    if (isWrite) {
      cache[level][line][blockPtr].dirty = cacheConfig[level].writeBack;
    }
    //         klee::klee_message("  L%d Hit.", level + 1);
    return;
  }

  // Cache miss.
  // Check if an old entry must be evicted.
  if (cache[level][line].size() >= cacheConfig[level].associativity) {
    // Find oldest entry in cache line.
    uint64_t lruPtr = cache[level][line].begin()->first;
    for (auto entry : cache[level][line]) {
      if (entry.second.useTime < cache[level][line][lruPtr].useTime) {
        lruPtr = entry.first;
      }
    }
    // Write out if dirty.
    if (cache[level][line][lruPtr].dirty) {
      // Write dirty evicted entry to next level.
      updateCache(lruPtr << BLOCK_BITS, 1, level + 1);
    }
    cache[level][line].erase(lruPtr);
    //         klee::klee_message("  L%d Eviction.", level + 1);
  }

  if ((!isWrite) || !cacheConfig[level].writeBack) {
    // Read in or write-through new entry from next level.
    updateCache(address, isWrite, level + 1);
  }

  cache[level][line][blockPtr].useTime = currentTime;
  cache[level][line][blockPtr].dirty = isWrite && cacheConfig[level].writeBack;
}

unsigned long GenericCacheModel::getCost(uint64_t address, bool isWrite,
                                         uint8_t level) {
  // Check if accessing beyond last cache (DRAM).
  if (!cacheConfig[level].size) {
    return cacheConfig[level].latency;
  }

  uint64_t blockPtr = address >> BLOCK_BITS;
  uint32_t line =
      blockPtr % (cacheConfig[level].size / cacheConfig[level].associativity /
                  (1 << BLOCK_BITS));

  // Check if cache hit.
  if (cache[level][line].count(blockPtr)) {
    if (isWrite && !cacheConfig[level].writeBack) {
      // Write-through to next level.
      return getCost(address, isWrite, level + 1);
    } else {
      // Write-back or read, don't propagate deeper.
      return cacheConfig[level].latency;
    }
  }

  // Cache miss.
  // Check if an old entry must be evicted.
  unsigned long cost = 0;
  if (cache[level][line].size() >= cacheConfig[level].associativity) {
    // Find oldest entry in cache line.
    uint64_t lruPtr = cache[level][line].begin()->first;
    for (auto entry : cache[level][line]) {
      if (entry.second.useTime < cache[level][line][lruPtr].useTime) {
        lruPtr = entry.first;
      }
    }
    // Write out if dirty.
    if (cache[level][line][lruPtr].dirty) {
      // Write dirty evicted entry to next level.
      cost += getCost(lruPtr << BLOCK_BITS, 1, level + 1);
    }
  }

  if ((!isWrite) || !cacheConfig[level].writeBack) {
    // Read in or write-through new entry from next level.
    cost += getCost(address, isWrite, level + 1);
  } else {
    // Write-back to current level.
    cost += cacheConfig[level].latency;
  }

  return cost;
}

unsigned long GenericCacheModel::getMissCost(uint64_t address, bool isWrite,
                                             uint8_t level) {
  // Check if accessing beyond last cache (DRAM).
  if (!cacheConfig[level].size) {
    return cacheConfig[level].latency;
  }

  uint64_t blockPtr = address >> BLOCK_BITS;
  uint32_t line =
      blockPtr % (cacheConfig[level].size / cacheConfig[level].associativity /
                  (1 << BLOCK_BITS));

  // Check if an old entry must be evicted.
  unsigned long cost = 0;
  if (cache[level][line].size() >= cacheConfig[level].associativity) {
    // Find oldest entry in cache line.
    uint64_t lruPtr = cache[level][line].begin()->first;
    for (auto entry : cache[level][line]) {
      if (entry.second.useTime < cache[level][line][lruPtr].useTime) {
        lruPtr = entry.first;
      }
    }
    // Write out if dirty.
    if (cache[level][line][lruPtr].dirty) {
      // Write dirty evicted entry to next level.
      cost += getCost(lruPtr << BLOCK_BITS, 1, level + 1);
    }
  }

  if ((!isWrite) || !cacheConfig[level].writeBack) {
    // Read in or write-through new entry from next level.
    cost += getMissCost(address, isWrite, level + 1);
  } else {
    // Write-back to current level.
    cost += cacheConfig[level].latency;
  }

  return cost;
}

klee::ref<klee::Expr> GenericCacheModel::memoryOperation(
    klee::TimingSolver *solver, klee::ExecutionState &state,
    klee::ref<klee::Expr> address, bool isWrite) {
  //     klee::klee_message("Memory %s at %s:%d.", isWrite ? "write" : "read",
  //                        state.pc->info->file.c_str(), state.pc->info->line);

  if (!isa<klee::ConstantExpr>(address)) {
    //         klee::klee_message("  Symbolic pointer.");
    address = state.constraints.simplifyExpr(address);

    bool found = false;
    if (WorstCaseSymIndices) {
      // Symbolic pointer, may hold several values: try the worst case scenarios
      // in turn until the constraints are SAT.
      // Find the maximum line granularity of all levels.
      uint32_t maxLines = 0;
      for (uint8_t level = 0; cacheConfig[level].size; level++) {
        uint32_t numLines = cacheConfig[level].size /
                            cacheConfig[level].associativity /
                            (1 << BLOCK_BITS);
        if (numLines > maxLines) {
          maxLines = numLines;
        }
      }
      // Sort lines by how much damage a miss would cause.
      // [-cycles] -> line.
      std::map<long, uint32_t> lineCosts;
      for (uint32_t line = 0; line < maxLines; line++) {
        lineCosts[-getMissCost(line << BLOCK_BITS, isWrite, 0)] = line;
      }

      //       klee::klee_message("%s symbolic pointer. Checking %d cache lines
      //       with "
      //                          "costs between %ld and %ld cycles.",
      //                          isWrite ? "Writing" : "Reading", maxLines,
      //                          -lineCosts.rbegin()->first,
      //                          -lineCosts.begin()->first);
      for (auto line : lineCosts) {
        //         klee::klee_message("Trying line %d with cost %ld cycles.",
        //         line.second,
        //                            -line.first);

        klee::ConstraintManager constraints(state.constraints);
        // Generate constraints on address that would make the miss happen.
        // Constraint cache line:
        // (line<<BLOCK_BITS) == ((maxLines-1)<<BLOCK_BITS & address)
        constraints.addConstraint(klee::EqExpr::create(
            klee::ConstantExpr::create(line.second << BLOCK_BITS,
                                       address->getWidth()),
            klee::AndExpr::create(
                klee::ConstantExpr::create((maxLines - 1) << BLOCK_BITS,
                                           address->getWidth()),
                address)));

        // Constrain against hit addresses.
        for (uint8_t level = 0; cacheConfig[level].size; level++) {
          uint32_t numLines = cacheConfig[level].size /
                              cacheConfig[level].associativity /
                              (1 << BLOCK_BITS);
          for (auto hitAddress : cache[level][line.second % numLines]) {
            // (! (hit<<BLOCK_BITS == ((-1)<<BLOCK_BITS) & address))
            constraints.addConstraint(
                klee::NotExpr::create(klee::EqExpr::create(
                    klee::ConstantExpr::create(hitAddress.first << BLOCK_BITS,
                                               address->getWidth()),
                    klee::AndExpr::create(
                        klee::ConstantExpr::create((-1) << BLOCK_BITS,
                                                   address->getWidth()),
                        address))));
          }
        }

        klee::ref<klee::ConstantExpr> concreteAddress;
        if (solver->solver->getValue(klee::Query(constraints, address),
                                     concreteAddress) &&
            "Solver fail.") {
          //           klee::klee_message("Line fits constraints.");
          state.addConstraint(klee::EqExpr::create(concreteAddress, address));
          address = concreteAddress;
          found = true;
          break;
        }
      }
    }

    // If unable to find a good candidate for worst case cache performance,
    // just concretize the address.
    if (!found) {
      //       klee::klee_message("Concretizing address without worst-case
      //       analysis.");
      klee::ref<klee::ConstantExpr> concreteAddress;
      assert(solver->getValue(state, address, concreteAddress) &&
             "Failed to concretize symbolic address.");
      state.constraints.addConstraint(
          klee::EqExpr::create(concreteAddress, address));
      address = concreteAddress;
    }
  }

  updateCache(dyn_cast<klee::ConstantExpr>(address)->getZExtValue(), isWrite,
              0);
  return address;
}

bool GenericCacheModel::loop(klee::ExecutionState &state) {
  if (enabled) {
    static unsigned iteration = 0;
    iteration++;
    //     klee::klee_message("Cache after iteration %d:", iteration);
    //     for (auto level : cache) {
    //       klee::klee_message("  L%d (%d lines):", level.first + 1,
    //                          cacheConfig[level.first].size /
    //                              cacheConfig[level.first].associativity /
    //                              (1 << BLOCK_BITS));
    //       for (auto line : level.second) {
    //         klee::klee_message("    Line %d (%ld/%d ways):", line.first,
    //                            line.second.size(),
    //                            cacheConfig[level.first].associativity);
    //         for (auto way : line.second) {
    //           klee::klee_message(
    //               "      Address: 0x%016lx, Dirty: %d, Accessed %ld units
    //               ago.",
    //               way.first << BLOCK_BITS, way.second.dirty,
    //               currentTime - way.second.useTime);
    //         }
    //       }
    //     }

    if (iteration >= MaxLoops) {
      //       klee::klee_message("Exhausted loop count.");
      return false;
    }

    loopStats.emplace_back();
    return true;
  } else {
    enabled = 1;
    loopStats.emplace_back();
    return true;
  }
}

void GenericCacheModel::exec(klee::ExecutionState &state) {
  if (enabled) {
    loopStats.back().instructionCount++;
  }
}

std::string GenericCacheModel::dumpStats() {
  std::stringstream stats;

  for (unsigned i = 0; i < loopStats.size(); i++) {
    stats << "Loop Iteration " << i << "\n";
    stats << "  Instructions: " << loopStats[i].instructionCount << "\n";
    stats << "  Reads: " << loopStats[i].readCount << "\n";
    stats << "  Writes: " << loopStats[i].writeCount << "\n";
    unsigned long cycles = loopStats[i].instructionCount * CPU_CPI;
    for (auto h : loopStats[i].hitCount) {
      if (cacheConfig[h.first].size) {
        stats << "  L" << (h.first + 1) << " Hits: " << h.second << "\n";
      } else {
        stats << "  DRAM Accesses: " << h.second << "\n";
      }
      cycles += h.second * cacheConfig[h.first].latency;
    }
    stats << "  Estimated Execution Time: " << cycles << " cycles ("
          << (cycles * 1E9 / CPU_HZ) << "ns @ " << (CPU_HZ / 1E9) << "GHz)\n";
    stats << "  Estimated Throughput (Single Core): " << (CPU_HZ / cycles / 1E6)
          << "Mpps, " << (64 * CPU_HZ / cycles / 1E9) << "Gbps (64B packets), "
          << (1500 * CPU_HZ / cycles / 1E9) << "Gbps (1500B packets), "
          << (9000 * CPU_HZ / cycles / 1E9) << "Gbps (9000B packets)\n";
  }

  return stats.str();
}
}