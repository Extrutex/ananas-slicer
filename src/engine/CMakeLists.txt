# ananas-engine: the headless slicing engine (libslic3r only, no GUI, no Python), driven by JSON-RPC.
# The core is a static library so the engine tests link the same code the executable runs.
add_library(ananas_engine_core STATIC
    Engine.cpp
    Engine.hpp
    Limits.cpp
    Limits.hpp
    Schema.cpp
    Schema.hpp
)
target_link_libraries(ananas_engine_core PUBLIC libslic3r boost_headeronly libcurl OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(ananas_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(ananas_engine_core PUBLIC -DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x602 -DBOOST_SYSTEM_USE_UTF8)

add_executable(ananas-engine main.cpp)
target_link_libraries(ananas-engine ananas_engine_core)
