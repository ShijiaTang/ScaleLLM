#pragma once

#include <absl/time/time.h>
#include <folly/MPMCQueue.h>

#include <cstdint>
#include <memory>
#include <queue>

#include "engine/engine.h"
#include "memory/block_manager.h"
#include "request/request.h"
#include "scheduler.h"

namespace llm {

class ContinuousBatchingScheduler final : public Scheduler {
 public:
  ContinuousBatchingScheduler(Engine* engine);

  ~ContinuousBatchingScheduler();

  // schedule a request, thread safe and non-blocking
  // may return false if the queue is full
  bool schedule(std::unique_ptr<Request>& request) override;

  // step the scheduler forward by one step
  // may get blocked if there are no requests to process
  void step(const absl::Duration& timeout) override;

 private:
  // get a batch of requests from the priority queue
  std::vector<Sequence*> create_sequence_batch();

  // the engine to run the batch
  Engine* engine_;

  // the block manager to manage the cache blocks
  BlockManager* block_manager_;

  // tokenizer
  const Tokenizer* tokenizer_;

  // a thread safe queue of requests, bounded by kRequestQueueSize
  // the schedule owns the requests and manages their lifetimes.
  folly::MPMCQueue<Request*> request_queue_;

  // Requests with HIGH priority are processed first, followed by MEDIUM
  // priority requests, and finally LOW priority requests. Within each priority
  // level, requests are handled on First-Come-First-Served (FCFS) basis.
  using MinHeap =
      std::priority_queue<Request*, std::vector<Request*>, RequestPtrGreater>;
  MinHeap priority_queue_;

  // a batch of requests to be processed
  std::vector<Request*> running_;

  // the executor to handle responses
  Executor response_executor_;
};

}  // namespace llm
