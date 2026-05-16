/*
 * KV Cache — error codes returned by the Core ABI.
 *
 * Convention: 0 = success. Negative = error. Positive = informational.
 */
#ifndef KVCACHE_KV_ERRORS_H
#define KVCACHE_KV_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KV_OK                  =   0,
    KV_E_INVAL             =  -1, /* invalid argument                       */
    KV_E_NOMEM             =  -2, /* out of memory / no slot available      */
    KV_E_NOT_FOUND         =  -3, /* prefix / locator not present           */
    KV_E_BUSY              =  -4, /* resource busy, retry                   */
    KV_E_TIMEOUT           =  -5, /* kv_wait timeout                        */
    KV_E_PERM              =  -6, /* RBAC denial                            */
    KV_E_QUOTA             =  -7, /* tenant quota exceeded                  */
    KV_E_TIER_DOWN         =  -8, /* requested tier unavailable             */
    KV_E_SAFETY_NET        =  -9, /* D-PERF-1 trip: fetch slower than recompute */
    KV_E_SEALED            = -10, /* attempt to write a sealed handle       */
    KV_E_NOT_SEALED        = -11, /* read before seal                       */
    KV_E_VERSION_MISMATCH  = -12, /* ABI version mismatch                   */
    KV_E_TRANSPORT         = -13, /* NIXL / network failure                 */
    KV_E_INTERNAL          = -99, /* unexpected internal error              */
} kv_status_t;

const char* kv_status_str(int status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KVCACHE_KV_ERRORS_H */
