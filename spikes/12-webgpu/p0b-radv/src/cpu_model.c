/* compiler-rt's __cpu_model, which __builtin_cpu_supports() reads (addrlib's
 * swizzler asks for "avx2" and nothing else in RADV asks for anything). The
 * title links no compiler-rt builtins. The console's CPU is always a Zen 2,
 * which has AVX2 (bit 10 of the first feature word, FEATURE_AVX2). */
struct {
    unsigned vendor, type, subtype, features[1];
} __cpu_model = { 2 /* VENDOR_AMD */, 0, 0, { 1u << 10 } };
unsigned __cpu_features2[3];
