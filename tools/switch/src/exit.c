#include <limits.h>
#include <stdlib.h>
#include <sys/lock.h>
#include <sys/reent.h>

#ifdef _REENT_SMALL
#error "The Switch exit registry requires newlib's full _atexit structure"
#endif

_Static_assert(_ATEXIT_SIZE <= sizeof(__ULong) * CHAR_BIT, "Exit callback masks must cover every table slot");

struct _atexit __atexit0 = _ATEXIT_INIT;
struct _atexit *__atexit = NULL;
__LOCK_INIT_RECURSIVE(, __atexit_recursive_mutex);

/**
 * Identifies the callback signatures accepted by newlib's exit registration ABI.
 */
enum ExitKind {
    ExitPlain = 0,
    ExitStatus = 1,
    ExitCxa = 2,
};

/**
 * Holds a removed callback independently of registry storage during its invocation.
 */
struct ExitCall {
    void (*function)(void); /**< Callback address using newlib's registration representation. */
    void *argument;         /**< Argument supplied when the callback was registered. */
    enum ExitKind kind;     /**< Signature used to invoke the callback. */
};

/**
 * Adds an exit callback to a dynamically extended newlib-compatible LIFO registry.
 */
int __register_exitproc(int type, void (*function)(void), void *argument, void *dso) {
    __lock_acquire_recursive(__atexit_recursive_mutex);
    if (__atexit == NULL) {
        __atexit = &__atexit0;
    }
    struct _atexit *table = __atexit;
    if (table->_ind >= _ATEXIT_SIZE) {
        table = calloc(1, sizeof(*table));
        if (table == NULL) {
            __lock_release_recursive(__atexit_recursive_mutex);
            return -1;
        }
        table->_next = __atexit;
        __atexit = table;
    }

    const int slot = table->_ind++;
    const __ULong mask = (__ULong)1 << slot;
    struct _on_exit_args *arguments = &table->_on_exit_args;
    arguments->_fntypes &= ~mask;
    arguments->_is_cxa &= ~mask;
    arguments->_fnargs[slot] = type == ExitPlain ? NULL : argument;
    arguments->_dso_handle[slot] = type == ExitPlain ? NULL : dso;
    if (type != ExitPlain) {
        arguments->_fntypes |= mask;
        if (type == ExitCxa) {
            arguments->_is_cxa |= mask;
        }
    }
    table->_fns[slot] = function;
    __lock_release_recursive(__atexit_recursive_mutex);
    return 0;
}

/**
 * Removes the newest matching callback while the exit-registry lock is held.
 */
static int TakeExitCall(void *dso, struct ExitCall *call) {
    struct _atexit **link = &__atexit;
    while (*link != NULL) {
        struct _atexit *table = *link;
        while (table->_ind > 0 && table->_fns[table->_ind - 1] == NULL) {
            --table->_ind;
        }
        if (table->_ind == 0 && table != &__atexit0) {
            *link = table->_next;
            free(table);
            continue;
        }
        struct _on_exit_args *arguments = &table->_on_exit_args;
        for (int slot = table->_ind - 1; slot >= 0; --slot) {
            if (table->_fns[slot] == NULL || (dso != NULL && arguments->_dso_handle[slot] != dso)) {
                continue;
            }
            const __ULong mask = (__ULong)1 << slot;
            call->function = table->_fns[slot];
            call->argument = arguments->_fnargs[slot];
            call->kind = (arguments->_fntypes & mask) == 0  ? ExitPlain
                         : (arguments->_is_cxa & mask) == 0 ? ExitStatus
                                                            : ExitCxa;
            table->_fns[slot] = NULL;
            if (slot == table->_ind - 1) {
                --table->_ind;
            }
            arguments->_fntypes &= ~mask;
            arguments->_is_cxa &= ~mask;
            arguments->_fnargs[slot] = NULL;
            arguments->_dso_handle[slot] = NULL;
            return 1;
        }
        link = &table->_next;
    }
    return 0;
}

/**
 * Invokes registered callbacks in reverse order, optionally restricted to one DSO.
 */
void __call_exitprocs(int status, void *dso) {
    __lock_acquire_recursive(__atexit_recursive_mutex);
    struct ExitCall call;
    while (TakeExitCall(dso, &call)) {
        // Callbacks may register callbacks, finalize another DSO, or wait for a registering thread.
        __lock_release_recursive(__atexit_recursive_mutex);
        if (call.kind == ExitPlain) {
            call.function();
        } else if (call.kind == ExitStatus) {
            ((void (*)(int, void *))call.function)(status, call.argument);
        } else {
            ((void (*)(void *))call.function)(call.argument);
        }
        __lock_acquire_recursive(__atexit_recursive_mutex);
    }
    __lock_release_recursive(__atexit_recursive_mutex);
}
