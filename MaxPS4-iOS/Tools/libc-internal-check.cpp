// Tests the SysV routines natively (x86-64 host), then prints them as C++ arrays.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define R(n) extern "C" char r_##n[], r_##n##_end[];
R(memcpy) R(memmove) R(memset) R(memcmp) R(strlen) R(strcmp) R(strncmp) R(strncpy) R(memcpy_s) R(strncpy_s)
R(sinf) R(cosf) R(tanf) R(atanf) R(atan2f) R(expf) R(logf) R(sincosf) R(atan) R(cos) R(tan)
#define SYSV __attribute__((sysv_abi))
template <typename F> F fn(char* p) { return reinterpret_cast<F>(p); }
static bool near(double a, double b, double e = 1e-5) { return std::fabs(a - b) <= e * (1 + std::fabs(b)); }
int main() {
    char a[32], b[32];
    using MemCpy = void* (SYSV*)(void*, const void*, size_t);
    std::strcpy(b, "abcdefghij");
    assert(fn<MemCpy>(r_memcpy)(a, b, 11) == a && !std::strcmp(a, "abcdefghij"));
    std::strcpy(a, "0123456789");
    assert(fn<MemCpy>(r_memmove)(a + 2, a, 8) == a + 2 && !std::memcmp(a, "0101234567", 10));
    std::strcpy(a, "0123456789");
    assert(fn<MemCpy>(r_memmove)(a, a + 2, 8) == a && !std::memcmp(a, "2345678989", 10));
    using MemSet = void* (SYSV*)(void*, int, size_t);
    assert(fn<MemSet>(r_memset)(a, 'x', 5) == a && !std::memcmp(a, "xxxxx", 5));
    using MemCmp = int (SYSV*)(const void*, const void*, size_t);
    assert(fn<MemCmp>(r_memcmp)("abc", "abd", 3) < 0 && fn<MemCmp>(r_memcmp)("abc", "abd", 2) == 0 &&
           fn<MemCmp>(r_memcmp)("b", "a", 1) > 0 && fn<MemCmp>(r_memcmp)("a", "b", 0) == 0);
    using StrLen = size_t (SYSV*)(const char*);
    assert(fn<StrLen>(r_strlen)("hello") == 5 && fn<StrLen>(r_strlen)("") == 0);
    using StrCmp = int (SYSV*)(const char*, const char*);
    assert(fn<StrCmp>(r_strcmp)("abc", "abc") == 0 && fn<StrCmp>(r_strcmp)("ab", "abc") < 0 && fn<StrCmp>(r_strcmp)("b", "a") > 0);
    using StrNCmp = int (SYSV*)(const char*, const char*, size_t);
    assert(fn<StrNCmp>(r_strncmp)("abcx", "abcy", 3) == 0 && fn<StrNCmp>(r_strncmp)("abcx", "abcy", 4) < 0 &&
           fn<StrNCmp>(r_strncmp)("ab", "ab", 9) == 0);
    using StrNCpy = char* (SYSV*)(char*, const char*, size_t);
    std::memset(a, 'z', sizeof(a));
    assert(fn<StrNCpy>(r_strncpy)(a, "hi", 5) == a && !std::memcmp(a, "hi\0\0\0z", 6));
    assert(fn<StrNCpy>(r_strncpy)(a, "hello", 3) == a && !std::memcmp(a, "hel\0", 4));
    using MemCpyS = int (SYSV*)(void*, size_t, const void*, size_t);
    std::memset(a, 'z', sizeof(a));
    assert(fn<MemCpyS>(r_memcpy_s)(a, 8, "abcd", 4) == 0 && !std::memcmp(a, "abcdz", 5));
    assert(fn<MemCpyS>(r_memcpy_s)(a, 3, "abcd", 4) == 34 && !std::memcmp(a, "\0\0\0d", 4));
    assert(fn<MemCpyS>(r_memcpy_s)(nullptr, 3, "abcd", 1) == 22 && fn<MemCpyS>(r_memcpy_s)(a, 2, nullptr, 1) == 22 && !a[0]);
    using StrNCpyS = int (SYSV*)(char*, size_t, const char*, size_t);
    std::memset(a, 'z', sizeof(a));
    assert(fn<StrNCpyS>(r_strncpy_s)(a, 8, "hello", 3) == 0 && !std::strcmp(a, "hel"));
    assert(fn<StrNCpyS>(r_strncpy_s)(a, 8, "hello", 99) == 0 && !std::strcmp(a, "hello"));
    assert(fn<StrNCpyS>(r_strncpy_s)(a, 4, "hello", 99) == 34 && !a[0]);
    assert(fn<StrNCpyS>(r_strncpy_s)(a, 6, "hello", 99) == 0 && !std::strcmp(a, "hello"));
    assert(fn<StrNCpyS>(r_strncpy_s)(a, 8, nullptr, 2) == 22 && !a[0]);
    using F1 = float (SYSV*)(float);
    using F2 = float (SYSV*)(float, float);
    using D1 = double (SYSV*)(double);
    for (float x : {-3.0f, -0.5f, 0.0f, 0.25f, 1.0f, 2.5f}) {
        assert(near(fn<F1>(r_sinf)(x), std::sin(x)) && near(fn<F1>(r_cosf)(x), std::cos(x)));
        assert(near(fn<F1>(r_tanf)(x), std::tan(x)) && near(fn<F1>(r_atanf)(x), std::atan(x)));
        assert(near(fn<F1>(r_expf)(x), std::exp(x)));
        assert(near(fn<D1>(r_atan)(x), std::atan(double(x)), 1e-12) && near(fn<D1>(r_cos)(x), std::cos(double(x)), 1e-12) &&
               near(fn<D1>(r_tan)(x), std::tan(double(x)), 1e-12));
        float s = 0, c = 0;
        fn<void (SYSV*)(float, float*, float*)>(r_sincosf)(x, &s, &c);
        assert(near(s, std::sin(x)) && near(c, std::cos(x)));
        for (float y : {-2.0f, 0.0f, 1.5f}) assert(near(fn<F2>(r_atan2f)(y, x), std::atan2(y, x)));
    }
    for (float x : {0.1f, 1.0f, 10.0f, 1000.0f}) assert(near(fn<F1>(r_logf)(x), std::log(x)));
    assert(near(fn<F1>(r_expf)(20.0f), std::exp(20.0f)) && near(fn<F1>(r_expf)(-20.0f), std::exp(-20.0f)));
    std::puts("// All routines checked natively.");
#define P(n) { std::printf("constexpr uint8_t internal_%s_code[] = {", #n); \
        for (char* p = r_##n; p < r_##n##_end; ++p) std::printf("%s0x%02x", p == r_##n ? "" : ",", unsigned(uint8_t(*p))); \
        std::printf("};\n"); }
    P(memcpy) P(memmove) P(memset) P(memcmp) P(strlen) P(strcmp) P(strncmp) P(strncpy) P(memcpy_s) P(strncpy_s)
    P(sinf) P(cosf) P(tanf) P(atanf) P(atan2f) P(expf) P(logf) P(sincosf) P(atan) P(cos) P(tan)
}
