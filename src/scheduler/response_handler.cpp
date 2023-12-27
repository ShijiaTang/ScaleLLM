#include "response_handler.h"

#include <cstdint>
#include <memory>

#include "common/logging.h"
#include "memory/block_manager.h"
#include "request/request.h"
#include "request/sequence.h"

namespace llm {

DEFINE_int32(streaming_token_buffer_size,
             1,
             "number of tokens to buffer before streaming to client");

ResponseHandler::ResponseHandler(BlockManager* block_manager,
    Tokenizer* tokenizer)
  : block_manager_(block_manager), tokenizer_(tokenizer) {}

void ResponseHandler::on_request_finish(Request* request) {
  // release all blocks for the finished request
  block_manager_->release_slots_for_request(request);
  // take over the ownership of the request
  std::unique_ptr<Request> finished_request(request);
  response_threadpool_.schedule([tokenizer = tokenizer_,
                                 request = std::move(finished_request)]() {
    if (request->stream) {
      // just finish the request
      request->on_finish("", FinishReason::NONE, Status());
    } else {
      // generate the final output
      GCHECK(request->sequences.size() == 1);
      Sequence& seq = request->sequences.front();
      const auto output = seq.decode_delta_text(seq.num_tokens(), *tokenizer);
      request->on_finish(output, seq.finish_reason(), Status());
    }
  });
}

void ResponseHandler::on_sequence_stream(Sequence* seq) {
  // check if the sequence has enough tokens to output
  const size_t num_tokens = seq->num_tokens();
  const size_t output_offset = seq->output_offset();
  const size_t num_tokens_to_output = num_tokens - output_offset;
  if (seq->is_finished() ||
      num_tokens_to_output >= FLAGS_streaming_token_buffer_size) {
    const auto finish_reason = seq->finish_reason();
    // output the delta text til the end of the sequence to the client
    response_threadpool_.schedule(
        [seq, tokenizer = tokenizer_, end = num_tokens, finish_reason]() {
          const auto detla = seq->decode_delta_text(end, *tokenizer);
          if (!detla.empty() || finish_reason != FinishReason::NONE) {
            seq->stream_delta(detla, finish_reason);
          };
        });
  }
}

}  // namespace llm