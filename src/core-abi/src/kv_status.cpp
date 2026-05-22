// Error-code string table for kv_status_str().
#include "kvcache/kv_errors.h"

// The shared library hides all C++ symbols by default; explicitly tag
// this entry point as public so it's exported from libkvcache.{so,dylib}.
// The other C ABI entries in kv_abi.cpp use the same attribute through
// the KV_API macro.
extern "C" __attribute__((visibility("default")))
const char* kv_status_str(int status) {
    switch (status) {
        case KV_OK:                 return "OK";
        case KV_E_INVAL:            return "invalid argument";
        case KV_E_NOMEM:            return "out of memory";
        case KV_E_NOT_FOUND:        return "not found";
        case KV_E_BUSY:             return "busy";
        case KV_E_TIMEOUT:          return "timeout";
        case KV_E_PERM:             return "permission denied";
        case KV_E_QUOTA:            return "quota exceeded";
        case KV_E_TIER_DOWN:        return "tier unavailable";
        case KV_E_SAFETY_NET:       return "safety-net trip (recompute faster)";
        case KV_E_SEALED:           return "handle already sealed";
        case KV_E_NOT_SEALED:       return "handle not sealed";
        case KV_E_VERSION_MISMATCH: return "ABI version mismatch";
        case KV_E_TRANSPORT:        return "transport failure";
        case KV_E_INTERNAL:         return "internal error";
        default:                    return "unknown";
    }
}
