include(cc_library)
include(cc_test)

cc_library(
  NAME
    linear 
  HDRS
    linear.h
    linear_impl.h
  SRCS
    linear.cpp
    linear_impl.cpp
  DEPS
    :state_dict
    :model_parallel
    :quantization
    :kernels
    glog::glog
    gflags::gflags
    torch
)

cc_library(
  NAME 
    pos_embedding
  HDRS 
    pos_embedding.h
  SRCS 
    pos_embedding.cpp
  DEPS
    :state_dict
    :memory
    :kernels
    glog::glog
    gflags::gflags
    torch
)

cc_library(
  NAME 
    layers
  HDRS 
    normalization.h
    embedding.h
    activation.h
  SRCS 
    activation.cpp
  DEPS
    :state_dict
    :memory
    :linear
    :pos_embedding
    :attention
    :kernels
    :flash_attn.kernels
    glog::glog
    gflags::gflags
    torch
)

cc_test(
  NAME
    layers_test
  SRCS
    activation_test.cpp
    layers_test.cpp
    pos_embedding_test.cpp
    normalization_test.cpp
  DEPS
    :layers
    :state_dict
    absl::random_random
    GTest::gtest_main
)

add_subdirectory(attention)