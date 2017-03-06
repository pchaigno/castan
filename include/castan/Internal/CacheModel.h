#ifndef CASTAN_INTERNAL_CACHEMODEL_H
#define CASTAN_INTERNAL_CACHEMODEL_H

#include <klee/ExecutionState.h>
#include <klee/Solver.h>

namespace castan {
class CacheModel {
public:
  virtual klee::ref<klee::Expr> load(klee::TimingSolver *solver,
                                     klee::ExecutionState &state,
                                     klee::ref<klee::Expr> address) = 0;
  virtual klee::ref<klee::Expr> store(klee::TimingSolver *solver,
                                      klee::ExecutionState &state,
                                      klee::ref<klee::Expr> address) = 0;
  virtual void exec(klee::ExecutionState &state) = 0;
  virtual bool loop(klee::ExecutionState &state) = 0;

  virtual std::string dumpStats() = 0;
};
}

#endif