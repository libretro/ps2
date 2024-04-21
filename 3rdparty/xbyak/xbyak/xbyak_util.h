#ifndef XBYAK_XBYAK_UTIL_H_
#define XBYAK_XBYAK_UTIL_H_

#include <string.h>
#include "xbyak.h"

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	#define XBYAK_INTEL_CPU_SPECIFIC
#endif

#ifdef XBYAK_INTEL_CPU_SPECIFIC
#ifdef _MSC_VER
	#if (_MSC_VER < 1400) && defined(XBYAK32)
		static inline __declspec(naked) void __cpuid(int[4], int)
		{
			__asm {
				push	ebx
				push	esi
				mov		eax, dword ptr [esp + 4 * 2 + 8] // eaxIn
				cpuid
				mov		esi, dword ptr [esp + 4 * 2 + 4] // data
				mov		dword ptr [esi], eax
				mov		dword ptr [esi + 4], ebx
				mov		dword ptr [esi + 8], ecx
				mov		dword ptr [esi + 12], edx
				pop		esi
				pop		ebx
				ret
			}
		}
	#else
		#include <intrin.h> // for __cpuid
	#endif
#else
	#ifndef __GNUC_PREREQ
    	#define __GNUC_PREREQ(major, minor) ((((__GNUC__) << 16) + (__GNUC_MINOR__)) >= (((major) << 16) + (minor)))
	#endif
	#if __GNUC_PREREQ(4, 3) && !defined(__APPLE__)
		#include <cpuid.h>
	#else
		#if defined(__APPLE__) && defined(XBYAK32) // avoid err : can't find a register in class `BREG' while reloading `asm'
			#define __cpuid(eaxIn, a, b, c, d) __asm__ __volatile__("pushl %%ebx\ncpuid\nmovl %%ebp, %%esi\npopl %%ebx" : "=a"(a), "=S"(b), "=c"(c), "=d"(d) : "0"(eaxIn))
			#define __cpuid_count(eaxIn, ecxIn, a, b, c, d) __asm__ __volatile__("pushl %%ebx\ncpuid\nmovl %%ebp, %%esi\npopl %%ebx" : "=a"(a), "=S"(b), "=c"(c), "=d"(d) : "0"(eaxIn), "2"(ecxIn))
		#else
			#define __cpuid(eaxIn, a, b, c, d) __asm__ __volatile__("cpuid\n" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(eaxIn))
			#define __cpuid_count(eaxIn, ecxIn, a, b, c, d) __asm__ __volatile__("cpuid\n" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(eaxIn), "2"(ecxIn))
		#endif
	#endif
#endif
#endif

namespace Xbyak { namespace util {

typedef enum {
   SmtLevel = 1,
   CoreLevel = 2
} IntelCpuTopologyLevel;

/**
	CPU detection class
*/
class Cpu {
	uint64_t type_;
	//system topology
	bool x2APIC_supported_;
	static const size_t maxTopologyLevels = 2;
	unsigned int numCores_[maxTopologyLevels];

	static const unsigned int maxNumberCacheLevels = 10;
	unsigned int dataCacheSize_[maxNumberCacheLevels];
	unsigned int coresSharignDataCache_[maxNumberCacheLevels];
	unsigned int dataCacheLevels_;

	unsigned int get32bitAsBE(const char *x) const
	{
		return x[0] | (x[1] << 8) | (x[2] << 16) | (x[3] << 24);
	}
	unsigned int mask(int n) const
	{
		return (1U << n) - 1;
	}
	void setFamily()
	{
		unsigned int data[4] = {};
		getCpuid(1, data);
		stepping = data[0] & mask(4);
		model = (data[0] >> 4) & mask(4);
		family = (data[0] >> 8) & mask(4);
		// type = (data[0] >> 12) & mask(2);
		extModel = (data[0] >> 16) & mask(4);
		extFamily = (data[0] >> 20) & mask(8);
		if (family == 0x0f) {
			displayFamily = family + extFamily;
		} else {
			displayFamily = family;
		}
		if (family == 6 || family == 0x0f) {
			displayModel = (extModel << 4) + model;
		} else {
			displayModel = model;
		}
	}
	unsigned int extractBit(unsigned int val, unsigned int base, unsigned int end)
	{
		return (val >> base) & ((1u << (end - base)) - 1);
	}
	void setNumCores()
	{
		if ((type_ & tINTEL) == 0) return;

		unsigned int data[4] = {};

		 /* CAUTION: These numbers are configuration as shipped by Intel. */
		getCpuidEx(0x0, 0, data);
		if (data[0] >= 0xB) {
			 /*
				if leaf 11 exists(x2APIC is supported),
				we use it to get the number of smt cores and cores on socket

				leaf 0xB can be zeroed-out by a hypervisor
			*/
			x2APIC_supported_ = true;
			for (unsigned int i = 0; i < maxTopologyLevels; i++) {
				getCpuidEx(0xB, i, data);
				IntelCpuTopologyLevel level = (IntelCpuTopologyLevel)extractBit(data[2], 8, 15);
				if (level == SmtLevel || level == CoreLevel) {
					numCores_[level - 1] = extractBit(data[1], 0, 15);
				}
			}
			/*
				Fallback values in case a hypervisor has 0xB leaf zeroed-out.
			*/
			numCores_[SmtLevel - 1] = (std::max)(1u, numCores_[SmtLevel - 1]);
			numCores_[CoreLevel - 1] = (std::max)(numCores_[SmtLevel - 1], numCores_[CoreLevel - 1]);
		} else {
			/*
				Failed to deremine num of cores without x2APIC support.
				TODO: USE initial APIC ID to determine ncores.
			*/
			numCores_[SmtLevel - 1] = 0;
			numCores_[CoreLevel - 1] = 0;
		}

	}
	void setCacheHierarchy()
	{
		if ((type_ & tINTEL) == 0) return;
		const unsigned int NO_CACHE = 0;
		const unsigned int DATA_CACHE = 1;
//		const unsigned int INSTRUCTION_CACHE = 2;
		const unsigned int UNIFIED_CACHE = 3;
		unsigned int smt_width = 0;
		unsigned int logical_cores = 0;
		unsigned int data[4] = {};

		if (x2APIC_supported_) {
			smt_width = numCores_[0];
			logical_cores = numCores_[1];
		}

		/*
			Assumptions:
			the first level of data cache is not shared (which is the
			case for every existing architecture) and use this to
			determine the SMT width for arch not supporting leaf 11.
			when leaf 4 reports a number of core less than numCores_
			on socket reported by leaf 11, then it is a correct number
			of cores not an upperbound.
		*/
		for (int i = 0; dataCacheLevels_ < maxNumberCacheLevels; i++) {
			getCpuidEx(0x4, i, data);
			unsigned int cacheType = extractBit(data[0], 0, 4);
			if (cacheType == NO_CACHE) break;
			if (cacheType == DATA_CACHE || cacheType == UNIFIED_CACHE) {
				unsigned int actual_logical_cores = extractBit(data[0], 14, 25) + 1;
				if (logical_cores != 0) { // true only if leaf 0xB is supported and valid
					actual_logical_cores = (std::min)(actual_logical_cores, logical_cores);
				}
				assert(actual_logical_cores != 0);
				dataCacheSize_[dataCacheLevels_] =
					(extractBit(data[1], 22, 31) + 1)
					* (extractBit(data[1], 12, 21) + 1)
					* (extractBit(data[1], 0, 11) + 1)
					* (data[2] + 1);
				if (cacheType == DATA_CACHE && smt_width == 0) smt_width = actual_logical_cores;
				assert(smt_width != 0);
				coresSharignDataCache_[dataCacheLevels_] = (std::max)(actual_logical_cores / smt_width, 1u);
				dataCacheLevels_++;
			}
		}
	}

public:
	int model;
	int family;
	int stepping;
	int extModel;
	int extFamily;
	int displayFamily; // family + extFamily
	int displayModel; // model + extModel

	/*
		data[] = { eax, ebx, ecx, edx }
	*/
	static inline void getCpuid(unsigned int eaxIn, unsigned int data[4])
	{
#ifdef XBYAK_INTEL_CPU_SPECIFIC
	#ifdef _MSC_VER
		__cpuid(reinterpret_cast<int*>(data), eaxIn);
	#else
		__cpuid(eaxIn, data[0], data[1], data[2], data[3]);
	#endif
#else
		(void)eaxIn;
		(void)data;
#endif
	}
	static inline void getCpuidEx(unsigned int eaxIn, unsigned int ecxIn, unsigned int data[4])
	{
#ifdef XBYAK_INTEL_CPU_SPECIFIC
	#ifdef _MSC_VER
		__cpuidex(reinterpret_cast<int*>(data), eaxIn, ecxIn);
	#else
		__cpuid_count(eaxIn, ecxIn, data[0], data[1], data[2], data[3]);
	#endif
#else
		(void)eaxIn;
		(void)ecxIn;
		(void)data;
#endif
	}
	static inline uint64_t getXfeature()
	{
#ifdef XBYAK_INTEL_CPU_SPECIFIC
	#ifdef _MSC_VER
		return _xgetbv(0);
	#else
		unsigned int eax, edx;
		// xgetvb is not support on gcc 4.2
//		__asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
		__asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
		return ((uint64_t)edx << 32) | eax;
	#endif
#else
		return 0;
#endif
	}
	typedef uint64_t Type;

	static const Type NONE = 0;
	static const Type tMMX = 1 << 0;
	static const Type tMMX2 = 1 << 1;
	static const Type tCMOV = 1 << 2;
	static const Type tSSE = 1 << 3;
	static const Type tSSE2 = 1 << 4;
	static const Type tSSE3 = 1 << 5;
	static const Type tSSSE3 = 1 << 6;
	static const Type tSSE41 = 1 << 7;
	static const Type tSSE42 = 1 << 8;
	static const Type tPOPCNT = 1 << 9;
	static const Type tAESNI = 1 << 10;
	static const Type tOSXSAVE = 1 << 12;
	static const Type tPCLMULQDQ = 1 << 13;
	static const Type tAVX = 1 << 14;
	static const Type tFMA = 1 << 15;

	static const Type t3DN = 1 << 16;
	static const Type tE3DN = 1 << 17;
	static const Type tRDTSCP = 1 << 19;
	static const Type tAVX2 = 1 << 20;
	static const Type tBMI1 = 1 << 21; // andn, bextr, blsi, blsmsk, blsr, tzcnt
	static const Type tBMI2 = 1 << 22; // bzhi, mulx, pdep, pext, rorx, sarx, shlx, shrx
	static const Type tLZCNT = 1 << 23;

	static const Type tINTEL = 1 << 24;
	static const Type tAMD = 1 << 25;

	static const Type tENHANCED_REP = 1 << 26; // enhanced rep movsb/stosb
	static const Type tRDRAND = 1 << 27;
	static const Type tADX = 1 << 28; // adcx, adox
	static const Type tRDSEED = 1 << 29; // rdseed
	static const Type tSMAP = 1 << 30; // stac
	static const Type tHLE = uint64_t(1) << 31; // xacquire, xrelease, xtest
	static const Type tRTM = uint64_t(1) << 32; // xbegin, xend, xabort
	static const Type tF16C = uint64_t(1) << 33; // vcvtph2ps, vcvtps2ph
	static const Type tMOVBE = uint64_t(1) << 34; // mobve
	static const Type tAVX512F = uint64_t(1) << 35;
	static const Type tAVX512DQ = uint64_t(1) << 36;
	static const Type tAVX512_IFMA = uint64_t(1) << 37;
	static const Type tAVX512IFMA = tAVX512_IFMA;
	static const Type tAVX512PF = uint64_t(1) << 38;
	static const Type tAVX512ER = uint64_t(1) << 39;
	static const Type tAVX512CD = uint64_t(1) << 40;
	static const Type tAVX512BW = uint64_t(1) << 41;
	static const Type tAVX512VL = uint64_t(1) << 42;
	static const Type tAVX512_VBMI = uint64_t(1) << 43;
	static const Type tAVX512VBMI = tAVX512_VBMI; // changed by Intel's manual
	static const Type tAVX512_4VNNIW = uint64_t(1) << 44;
	static const Type tAVX512_4FMAPS = uint64_t(1) << 45;
	static const Type tPREFETCHWT1 = uint64_t(1) << 46;
	static const Type tPREFETCHW = uint64_t(1) << 47;
	static const Type tSHA = uint64_t(1) << 48;
	static const Type tMPX = uint64_t(1) << 49;
	static const Type tAVX512_VBMI2 = uint64_t(1) << 50;
	static const Type tGFNI = uint64_t(1) << 51;
	static const Type tVAES = uint64_t(1) << 52;
	static const Type tVPCLMULQDQ = uint64_t(1) << 53;
	static const Type tAVX512_VNNI = uint64_t(1) << 54;
	static const Type tAVX512_BITALG = uint64_t(1) << 55;
	static const Type tAVX512_VPOPCNTDQ = uint64_t(1) << 56;
	static const Type tAVX512_BF16 = uint64_t(1) << 57;
	static const Type tAVX512_VP2INTERSECT = uint64_t(1) << 58;
	static const Type tAMX_TILE = uint64_t(1) << 59;
	static const Type tAMX_INT8 = uint64_t(1) << 60;
	static const Type tAMX_BF16 = uint64_t(1) << 61;
	static const Type tAVX_VNNI = uint64_t(1) << 62;
	static const Type tAVX512_FP16 = uint64_t(1) << 11;
	// 18, 63

	Cpu()
		: type_(NONE)
		, x2APIC_supported_(false)
		, numCores_()
		, dataCacheSize_()
		, coresSharignDataCache_()
		, dataCacheLevels_(0)
	{
		unsigned int data[4] = {};
		const unsigned int& EAX = data[0];
		const unsigned int& EBX = data[1];
		const unsigned int& ECX = data[2];
		const unsigned int& EDX = data[3];
		getCpuid(0, data);
		const unsigned int maxNum = EAX;
		static const char intel[] = "ntel";
		static const char amd[] = "cAMD";
		if (ECX == get32bitAsBE(amd)) {
			type_ |= tAMD;
			getCpuid(0x80000001, data);
			if (EDX & (1U << 31)) {
				type_ |= t3DN;
				// 3DNow! implies support for PREFETCHW on AMD
				type_ |= tPREFETCHW;
			}

			if (EDX & (1U << 29)) {
				// Long mode implies support for PREFETCHW on AMD
				type_ |= tPREFETCHW;
			}
		}
		if (ECX == get32bitAsBE(intel)) {
			type_ |= tINTEL;
		}

		// Extended flags information
		getCpuid(0x80000000, data);
		if (EAX >= 0x80000001) {
			getCpuid(0x80000001, data);

			if (EDX & (1U << 31)) type_ |= t3DN;
			if (EDX & (1U << 30)) type_ |= tE3DN;
			if (EDX & (1U << 27)) type_ |= tRDTSCP;
			if (EDX & (1U << 22)) type_ |= tMMX2;
			if (EDX & (1U << 15)) type_ |= tCMOV;
			if (ECX & (1U << 5)) type_ |= tLZCNT;
			if (ECX & (1U << 8)) type_ |= tPREFETCHW;
		}

		getCpuid(1, data);
		if (ECX & (1U << 0)) type_ |= tSSE3;
		if (ECX & (1U << 9)) type_ |= tSSSE3;
		if (ECX & (1U << 19)) type_ |= tSSE41;
		if (ECX & (1U << 20)) type_ |= tSSE42;
		if (ECX & (1U << 22)) type_ |= tMOVBE;
		if (ECX & (1U << 23)) type_ |= tPOPCNT;
		if (ECX & (1U << 25)) type_ |= tAESNI;
		if (ECX & (1U << 1)) type_ |= tPCLMULQDQ;
		if (ECX & (1U << 27)) type_ |= tOSXSAVE;
		if (ECX & (1U << 30)) type_ |= tRDRAND;
		if (ECX & (1U << 29)) type_ |= tF16C;

		if (EDX & (1U << 15)) type_ |= tCMOV;
		if (EDX & (1U << 23)) type_ |= tMMX;
		if (EDX & (1U << 25)) type_ |= tMMX2 | tSSE;
		if (EDX & (1U << 26)) type_ |= tSSE2;

		if (type_ & tOSXSAVE) {
			// check XFEATURE_ENABLED_MASK[2:1] = '11b'
			uint64_t bv = getXfeature();
			if ((bv & 6) == 6) {
				if (ECX & (1U << 28)) type_ |= tAVX;
				if (ECX & (1U << 12)) type_ |= tFMA;
				// do *not* check AVX-512 state on macOS because it has on-demand AVX-512 support
#if !defined(__APPLE__)
				if (((bv >> 5) & 7) == 7)
#endif
				{
					getCpuidEx(7, 0, data);
					if (EBX & (1U << 16)) type_ |= tAVX512F;
					if (type_ & tAVX512F) {
						if (EBX & (1U << 17)) type_ |= tAVX512DQ;
						if (EBX & (1U << 21)) type_ |= tAVX512_IFMA;
						if (EBX & (1U << 26)) type_ |= tAVX512PF;
						if (EBX & (1U << 27)) type_ |= tAVX512ER;
						if (EBX & (1U << 28)) type_ |= tAVX512CD;
						if (EBX & (1U << 30)) type_ |= tAVX512BW;
						if (EBX & (1U << 31)) type_ |= tAVX512VL;
						if (ECX & (1U << 1)) type_ |= tAVX512_VBMI;
						if (ECX & (1U << 6)) type_ |= tAVX512_VBMI2;
						if (ECX & (1U << 8)) type_ |= tGFNI;
						if (ECX & (1U << 9)) type_ |= tVAES;
						if (ECX & (1U << 10)) type_ |= tVPCLMULQDQ;
						if (ECX & (1U << 11)) type_ |= tAVX512_VNNI;
						if (ECX & (1U << 12)) type_ |= tAVX512_BITALG;
						if (ECX & (1U << 14)) type_ |= tAVX512_VPOPCNTDQ;
						if (EDX & (1U << 2)) type_ |= tAVX512_4VNNIW;
						if (EDX & (1U << 3)) type_ |= tAVX512_4FMAPS;
						if (EDX & (1U << 8)) type_ |= tAVX512_VP2INTERSECT;
						if ((type_ & tAVX512BW) && (EDX & (1U << 23))) type_ |= tAVX512_FP16;
					}
				}
			}
		}
		if (maxNum >= 7) {
			getCpuidEx(7, 0, data);
			const uint32_t maxNumSubLeaves = EAX;
			if (type_ & tAVX && (EBX & (1U << 5))) type_ |= tAVX2;
			if (EBX & (1U << 3)) type_ |= tBMI1;
			if (EBX & (1U << 8)) type_ |= tBMI2;
			if (EBX & (1U << 9)) type_ |= tENHANCED_REP;
			if (EBX & (1U << 18)) type_ |= tRDSEED;
			if (EBX & (1U << 19)) type_ |= tADX;
			if (EBX & (1U << 20)) type_ |= tSMAP;
			if (EBX & (1U << 4)) type_ |= tHLE;
			if (EBX & (1U << 11)) type_ |= tRTM;
			if (EBX & (1U << 14)) type_ |= tMPX;
			if (EBX & (1U << 29)) type_ |= tSHA;
			if (ECX & (1U << 0)) type_ |= tPREFETCHWT1;
			if (EDX & (1U << 24)) type_ |= tAMX_TILE;
			if (EDX & (1U << 25)) type_ |= tAMX_INT8;
			if (EDX & (1U << 22)) type_ |= tAMX_BF16;
			if (maxNumSubLeaves >= 1) {
				getCpuidEx(7, 1, data);
				if (EAX & (1U << 4)) type_ |= tAVX_VNNI;
				if (type_ & tAVX512F) {
					if (EAX & (1U << 5)) type_ |= tAVX512_BF16;
				}
			}
		}
		setFamily();
		setNumCores();
		setCacheHierarchy();
	}
	bool has(Type type) const
	{
		return (type & type_) != 0;
	}
};

} } // end of util

#endif
