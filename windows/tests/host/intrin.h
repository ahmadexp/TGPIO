#ifndef STUB_INTRIN_H
#define STUB_INTRIN_H
extern unsigned long long fake_tsc;
static inline unsigned long long __rdtsc(void) { return fake_tsc; }
static inline unsigned long long __readmsr(unsigned long m) { (void)m; return 0; }
static inline void __cpuid(int r[4], int leaf) { r[0]=r[1]=r[2]=r[3]=0; (void)leaf; }
static inline void __cpuidex(int r[4], int leaf, int sub) { r[0]=r[1]=r[2]=r[3]=0; (void)leaf; (void)sub; }
static inline unsigned long long _umul128(unsigned long long a,
	unsigned long long b, unsigned long long *hi)
{ __uint128_t p = (__uint128_t)a * b; *hi = (unsigned long long)(p >> 64); return (unsigned long long)p; }
static inline unsigned long long _udiv128(unsigned long long hi,
	unsigned long long lo, unsigned long long d, unsigned long long *rem)
{ __uint128_t n = ((__uint128_t)hi << 64) | lo; *rem = (unsigned long long)(n % d); return (unsigned long long)(n / d); }
#endif
