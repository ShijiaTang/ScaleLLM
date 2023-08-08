#include "cache_manager.h"

#include <glog/logging.h>
#include <torch/torch.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "block_allocator.h"
#include "cache_planner.h"

namespace llm {

CacheManager::CacheManager(const CacheArg& cache_arg) {
  // TODO: pre-allocate memory for each layer based on the cache_arg
}

}  // namespace llm
