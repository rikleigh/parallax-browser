// cxx_test.cpp - C++ allocation through the direct-memory heap: libc++'s
// operator new/delete (plain, array, aligned) call malloc/free/aligned_alloc,
// which the title links --wrap'ed to dheap.c.
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dmem.h"

namespace {
struct alignas(256) Aligned {
    char bytes[512];
};
} // namespace

extern "C" int cxx_test(char *out, size_t n)
{
    int owned = 0, total = 0;
    auto in_heap = [&](const void *p) {
        total++;
        owned += dheap_owns(p) ? 1 : 0;
    };
    int *array = new int[1000];
    in_heap(array);
    auto unique = std::make_unique<double>(3.0);
    in_heap(unique.get());
    std::vector<int> vec;
    for (int i = 0; i < 100000; i++)
        vec.push_back(i); // grows through several reallocations
    in_heap(vec.data());
    std::string str(5000, 'x');
    in_heap(str.data());
    auto shared = std::make_shared<std::string>(300, 'y');
    in_heap(shared.get());
    std::unordered_map<int, std::string> map;
    for (int i = 0; i < 1000; i++)
        map.emplace(i, std::to_string(i * 7919));
    in_heap(&*map.find(500));
    Aligned *al = new Aligned;
    const bool aligned_ok = (reinterpret_cast<uintptr_t>(al) & 255) == 0;
    in_heap(al);
    long long sum = 0;
    for (int v : vec)
        sum += v;
    const bool values_ok = sum == 4999950000LL && map.at(500) == std::to_string(500 * 7919) &&
                           str.size() == 5000;
    delete al;
    delete[] array;
    std::snprintf(out, n,
                  "%d of %d C++ allocations in the direct heap (new[], make_unique, vector growth, "
                  "string, make_shared, unordered_map node, aligned new of alignas(256): aligned %s); "
                  "values %s",
                  owned, total, aligned_ok ? "yes" : "NO", values_ok ? "correct" : "WRONG");
    return owned == total && aligned_ok && values_ok ? 0 : 1;
}
