include(cc_library)

cc_library(
  NAME 
    flash-attn.kernels
  SRCS
    flash_api.cpp
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim32_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim32_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim64_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim64_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim96_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim96_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim128_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim128_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim160_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim160_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim192_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim192_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim224_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim224_bf16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim256_fp16_sm80.cu
    flash-attn/csrc/flash_attn/src/flash_fwd_hdim256_bf16_sm80.cu
  INCLUDES
    flash-attn/csrc/flash_attn
    flash-attn/csrc/flash_attn/src
    flash-attn/csrc/cutlass/include
  DEPS
    torch
)
