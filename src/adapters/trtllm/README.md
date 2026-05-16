# TRT-LLM adapter

C++ backend that plugs into TRT-LLM's `KVCacheManager`. LLD §6.1.4.

This is the only C++ adapter — TRT-LLM's plugin surface is native C++.
Built alongside the rest of the C/C++ tree via CMake.

TODO(stephen): add `CMakeLists.txt` and `src/kvcache_trtllm_backend.cpp`
implementing the `KVCacheManager` interface against `libkvcache.so`.
