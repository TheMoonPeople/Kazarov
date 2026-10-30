#include <sys/reent.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

extern "C" int  __register_exitproc(int type, void (*function)(), void *argument, void *dso);
extern "C" void __call_exitprocs(int status, void *dso);

#ifdef KZ_TEST_WRAP_CALLOC
extern "C" void *__real_calloc(std::size_t count, std::size_t size);

namespace {
bool fail_allocation = false;
}

extern "C" void *__wrap_calloc(std::size_t count, std::size_t size) {
    return fail_allocation ? nullptr : __real_calloc(count, size);
}
#endif

static_assert(sizeof(_atexit) == 792);
static_assert(offsetof(_atexit, _fns) == 16);
static_assert(offsetof(_atexit, _on_exit_args) == 272);
static_assert(offsetof(_on_exit_args, _fntypes) == 512);

namespace {

std::vector<int> calls;
int              dso_a;
int              dso_b;

void Record(void *argument) { calls.push_back(static_cast<int>(reinterpret_cast<std::intptr_t>(argument))); }

void Register(int value, void *dso = nullptr) {
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(Record), reinterpret_cast<void *>(static_cast<std::intptr_t>(value)), dso) == 0);
}

void Plain() { calls.push_back(5000); }

void WithStatus(int status, void *argument) {
    assert(status == 77);
    Record(argument);
}

void RegisterStatus(void *) {
    calls.push_back(900);
    assert(__register_exitproc(1, reinterpret_cast<void (*)()>(WithStatus), reinterpret_cast<void *>(901), nullptr) == 0);
}

void Spawn(void *) {
    calls.push_back(1000);
    for (int index = 1; index < 41; ++index) {
        Register(1000 + index);
    }
}

void FinalizeOther(void *) {
    calls.push_back(2000);
    __call_exitprocs(0, &dso_b);
}

void RegisterFromThread(void *) {
    calls.push_back(3000);
    std::thread worker([] { Register(3001); });
    worker.join();
}

void CheckEmpty() {
    assert(__atexit == &__atexit0);
    assert(__atexit->_ind == 0 && __atexit->_next == nullptr);
    calls.clear();
}

} // namespace

int main() {
    for (int index = 0; index < 128; ++index) {
        Register(index);
    }
    __call_exitprocs(0, nullptr);
    assert(calls.size() == 128);
    for (int index = 0; index < 128; ++index) {
        assert(calls[static_cast<std::size_t>(index)] == 127 - index);
    }
    CheckEmpty();

#ifdef KZ_TEST_WRAP_CALLOC
    for (int index = 0; index < 32; ++index) {
        Register(index);
    }
    fail_allocation = true;
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(Record), reinterpret_cast<void *>(32), nullptr) == -1);
    assert(__atexit->_ind == 32);
    fail_allocation = false;
    Register(32);
    __call_exitprocs(0, nullptr);
    assert(calls.size() == 33);
    for (int index = 0; index < 33; ++index) {
        assert(calls[static_cast<std::size_t>(index)] == 32 - index);
    }
    CheckEmpty();
#endif

    for (int index = 0; index < 96; ++index) {
        Register(index, index % 2 == 0 ? &dso_a : &dso_b);
    }
    __call_exitprocs(0, &dso_a);
    assert(calls.size() == 48);
    for (int index = 0; index < 48; ++index) {
        assert(calls[static_cast<std::size_t>(index)] == 94 - index * 2);
    }
    __call_exitprocs(0, &dso_a);
    assert(calls.size() == 48);
    __call_exitprocs(0, nullptr);
    assert(calls.size() == 96);
    for (int index = 0; index < 48; ++index) {
        assert(calls[static_cast<std::size_t>(48 + index)] == 95 - index * 2);
    }
    CheckEmpty();

    assert(__register_exitproc(0, Plain, nullptr, nullptr) == 0);
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(RegisterStatus), nullptr, &dso_a) == 0);
    __call_exitprocs(0, &dso_a);
    assert((calls == std::vector<int>{900}));
    __call_exitprocs(77, nullptr);
    assert((calls == std::vector<int>{900, 901, 5000}));
    CheckEmpty();

    Register(999);
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(Spawn), nullptr, nullptr) == 0);
    __call_exitprocs(0, nullptr);
    assert(calls.size() == 42 && calls.front() == 1000 && calls.back() == 999);
    for (int index = 1; index <= 40; ++index) {
        assert(calls[static_cast<std::size_t>(index)] == 1041 - index);
    }
    CheckEmpty();

    for (int index = 0; index < 96; ++index) {
        Register(index, &dso_b);
    }
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(FinalizeOther), nullptr, &dso_a) == 0);
    __call_exitprocs(0, &dso_a);
    assert(calls.size() == 97 && calls.front() == 2000);
    for (int index = 0; index < 96; ++index) {
        assert(calls[static_cast<std::size_t>(index + 1)] == 95 - index);
    }
    CheckEmpty();

    Register(2999);
    assert(__register_exitproc(2, reinterpret_cast<void (*)()>(RegisterFromThread), nullptr, nullptr) == 0);
    __call_exitprocs(0, nullptr);
    assert((calls == std::vector<int>{3000, 3001, 2999}));
    CheckEmpty();

    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([worker] {
            for (int index = 0; index < 128; ++index) {
                Register(worker * 128 + index);
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    __call_exitprocs(0, nullptr);
    assert(calls.size() == 1024);
    std::ranges::sort(calls);
    for (int index = 0; index < 1024; ++index) {
        assert(calls[static_cast<std::size_t>(index)] == index);
    }
    CheckEmpty();
    std::cout << "exit registry: LIFO, DSO filters, slot reuse, nested callbacks and threads passed\n";
}
