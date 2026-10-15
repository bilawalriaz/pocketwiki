#pragma once
/* The slice of the PSA hash API the pack installer uses: SHA-256 only. */
#include <stddef.h>
#include <stdint.h>
typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
typedef struct {
    int active;
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t used;
} psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t len);
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t cap, size_t *olen);
psa_status_t psa_hash_abort(psa_hash_operation_t *op);
