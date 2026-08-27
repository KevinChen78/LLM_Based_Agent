# Phase 5-C: gRPC toolchain (FetchContent source build, isolated behind
# ENABLE_GRPC which defaults OFF in the root CMakeLists — when OFF none of
# this is configured and the build is byte-identical to before).
#
# All third-party providers are "module" = gRPC's own third_party submodules,
# so no system package (vcpkg/OpenSSL/protoc) is required. protoc and
# grpc_cpp_plugin are built from source as part of the tree.

set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_ABSL_PROVIDER module CACHE STRING "" FORCE)
set(gRPC_CARES_PROVIDER module CACHE STRING "" FORCE)
set(gRPC_PROTOBUF_PROVIDER module CACHE STRING "" FORCE)
set(gRPC_RE2_PROVIDER module CACHE STRING "" FORCE)
set(gRPC_SSL_PROVIDER module CACHE STRING "" FORCE)
set(gRPC_ZLIB_PROVIDER module CACHE STRING "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
# gRPC_INSTALL=OFF alone is not enough: protobuf/utf8_range register their own
# install exports referencing absl targets; with absl install disabled the
# export sets dangle and generation fails. We never install — turn them off.
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

make_github_url("https://github.com/grpc/grpc.git" GRPC_URL)
FetchContent_Declare(
    grpc
    GIT_REPOSITORY ${GRPC_URL}
    GIT_TAG v1.71.0
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(grpc)

# ---------------------------------------------------------------------------
# Proto codegen for the inter-service contract.
# protoc / grpc_cpp_plugin are in-tree targets built above, so generation
# needs no preinstalled tooling.
# ---------------------------------------------------------------------------
set(RETRIEVAL_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/retrieval.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(RETRIEVAL_PROTO_SRCS
    ${PROTO_GEN_DIR}/retrieval.pb.cc
    ${PROTO_GEN_DIR}/retrieval.grpc.pb.cc)
set(RETRIEVAL_PROTO_HDRS
    ${PROTO_GEN_DIR}/retrieval.pb.h
    ${PROTO_GEN_DIR}/retrieval.grpc.pb.h)

add_custom_command(
    OUTPUT ${RETRIEVAL_PROTO_SRCS} ${RETRIEVAL_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
    COMMAND $<TARGET_FILE:protoc>
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
            ${RETRIEVAL_PROTO}
    DEPENDS ${RETRIEVAL_PROTO} protoc grpc_cpp_plugin
    COMMENT "Generating gRPC/Protobuf sources for retrieval.proto")

add_library(retrieval_proto STATIC
    ${RETRIEVAL_PROTO_SRCS} ${RETRIEVAL_PROTO_HDRS})
target_include_directories(retrieval_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(retrieval_proto PUBLIC grpc++ libprotobuf)
if(MSVC)
    # Third-party generated code is warning-heavy under the global /W4.
    target_compile_options(retrieval_proto PRIVATE /W0)
endif()

# Link smoke test: proves generated code links against grpc++ and a message
# round-trips. Not wired into any business path (Stage D does that).
add_executable(grpc_proto_smoke smoke/grpc_proto_smoke.cpp)
target_link_libraries(grpc_proto_smoke PRIVATE retrieval_proto grpc++ libprotobuf)
set_target_properties(grpc_proto_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
