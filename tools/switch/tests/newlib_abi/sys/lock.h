#pragma once

#include <pthread.h>

typedef pthread_mutex_t _LOCK_RECURSIVE_T;

#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define KZ_EXIT_TEST_LOCK_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#elif defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
#define KZ_EXIT_TEST_LOCK_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#else
#error "The native exit-registry test requires a recursive pthread mutex initializer"
#endif

#define __LOCK_INIT_RECURSIVE(scope, name) scope _LOCK_RECURSIVE_T name = KZ_EXIT_TEST_LOCK_INIT
#define __lock_acquire_recursive(name) pthread_mutex_lock(&(name))
#define __lock_release_recursive(name) pthread_mutex_unlock(&(name))
