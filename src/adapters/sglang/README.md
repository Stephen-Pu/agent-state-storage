# sglang adapter

Adapter for the sglang engine. LLD §6.1.4.

Integration:
- Plugs in as an L2 cache backend; cooperates with RadixAttention.
- 

Supported versions: latest stable + one prior.

TODO(stephen): scaffold the engine bindings against libkvcache.so via cffi.
