#pragma once

#include <stdint.h>

#define _ATEXIT_SIZE 32
typedef uint32_t __ULong;

/**
 * Holds callback arguments, DSO identities and signature masks for the native ABI fixture.
 */
struct _on_exit_args {
    void   *_fnargs[_ATEXIT_SIZE];     /**< Callback arguments indexed by table slot. */
    void   *_dso_handle[_ATEXIT_SIZE]; /**< DSO identities indexed by table slot. */
    __ULong _fntypes;                  /**< Slots whose callbacks receive arguments. */
    __ULong _is_cxa;                   /**< Slots whose callbacks use the C++ destructor signature. */
};

/**
 * Models one newlib exit table with the full 64-bit-target field layout.
 */
struct _atexit {
    struct _atexit *_next;              /**< Older exit table. */
    int             _ind;               /**< End of the occupied slot range. */
    void (*_fns[_ATEXIT_SIZE])(void);   /**< Registered callback addresses. */
    struct _on_exit_args _on_exit_args; /**< Callback argument and signature data. */
};

// clang-format off
#define _ATEXIT_INIT {0, 0, {0}, {{0}, {0}, 0, 0}}
// clang-format on

#ifdef __cplusplus
extern "C" {
#endif
extern struct _atexit  __atexit0;
extern struct _atexit *__atexit;
#ifdef __cplusplus
}
#endif
