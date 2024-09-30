/*
 * Copyright 2018, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <cpu.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <algorithm>

#include <ACPI.h>

#include <boot_device.h>
#include <commpage.h>
#include <debug.h>
#include <elf.h>
#include <safemode.h>
#include <smp.h>
#include <util/BitUtils.h>
#include <vm/vm.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>

#include <arch_system_info.h>
#include <arch/x86/apic.h>
#include <boot/kernel_args.h>

#include "paging/X86PagingStructures.h"
#include "paging/X86VMTranslationMap.h"


#define DUMP_FEATURE_STRING	1
#define DUMP_CPU_TOPOLOGY	1
#define DUMP_CPU_PATCHLEVEL_TYPE	1


/* cpu vendor info */
struct cpu_vendor_info {
	const char *vendor;
	const char *ident_string[2];
};

static const struct cpu_vendor_info vendor_info[VENDOR_NUM] = {
	{ "Intel", { "GenuineIntel" } },
	{ "AMD", { "AuthenticAMD" } },
	{ "Cyrix", { "CyrixInstead" } },
	{ "UMC", { "UMC UMC UMC" } },
	{ "NexGen", { "NexGenDriven" } },
	{ "Centaur", { "CentaurHauls" } },
	{ "Rise", { "RiseRiseRise" } },
	{ "Transmeta", { "GenuineTMx86", "TransmetaCPU" } },
	{ "NSC", { "Geode by NSC" } },
	{ "Hygon", { "HygonGenuine" } },
};

#define K8_SMIONCMPHALT			(1ULL << 27)
#define K8_C1EONCMPHALT			(1ULL << 28)

#define K8_CMPHALT				(K8_SMIONCMPHALT | K8_C1EONCMPHALT)

struct set_mtrr_parameter {
	int32	index;
	uint64	base;
	uint64	length;
	uint8	type;
};

struct set_mtrrs_parameter {
	const x86_mtrr_info*	infos;
	uint32					count;
	uint8					defaultType;
};


#ifdef __x86_64__
extern addr_t _stac;
extern addr_t _clac;
extern addr_t _xsave;
extern addr_t _xsavec;
extern addr_t _xrstor;
uint64 gXsaveMask;
uint64 gFPUSaveLength = 512;
bool gHasXsave = false;
bool gHasXsavec = false;
#endif

extern "C" void x86_reboot(void);
	// from arch.S

void (*gCpuIdleFunc)(void);
#ifndef __x86_64__
void (*gX86SwapFPUFunc)(void* oldState, const void* newState) = x86_noop_swap;
bool gHasSSE = false;
#endif

static uint32 sCpuRendezvous;
static uint32 sCpuRendezvous2;
static uint32 sCpuRendezvous3;
static vint32 sTSCSyncRendezvous;

/* Some specials for the double fault handler */
static addr_t sDoubleFaultStacks = 0;
static const size_t kDoubleFaultStackSize = 4096;	// size per CPU

static x86_cpu_module_info* sCpuModule;


/* CPU topology information */
static uint32 (*sGetCPUTopologyID)(int currentCPU);
static uint32 sHierarchyMask[CPU_TOPOLOGY_LEVELS];
static uint32 sHierarchyShift[CPU_TOPOLOGY_LEVELS];

/* Cache topology information */
static uint32 sCacheSharingMask[CPU_MAX_CACHE_LEVEL];

static void* sUcodeData = NULL;
static size_t sUcodeDataSize = 0;
static void* sLoadedUcodeUpdate;
static spinlock sUcodeUpdateLock = B_SPINLOCK_INITIALIZER;

static bool sUsePAT = false;


static status_t
acpi_shutdown(bool rebootSystem)
{
	if (debug_debugger_running() || !are_interrupts_enabled())
		return B_ERROR;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_NOT_SUPPORTED;

	status_t status;
	if (rebootSystem) {
		status = acpi->reboot();
	} else {
		status = acpi->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
		if (status == B_OK) {
			//cpu_status state = disable_interrupts();
			status = acpi->enter_sleep_state(ACPI_POWER_STATE_OFF);
			//restore_interrupts(state);
		}
	}

	put_module(B_ACPI_MODULE_NAME);
	return status;
}


/*!	Disable CPU caches, and invalidate them. */
static void
disable_caches()
{
	x86_write_cr0((x86_read_cr0() | CR0_CACHE_DISABLE)
		& ~CR0_NOT_WRITE_THROUGH);
	wbinvd();
	arch_cpu_global_TLB_invalidate();
}


/*!	Invalidate CPU caches, and enable them. */
static void
enable_caches()
{
	wbinvd();
	arch_cpu_global_TLB_invalidate();
	x86_write_cr0(x86_read_cr0()
		& ~(CR0_CACHE_DISABLE | CR0_NOT_WRITE_THROUGH));
}


static void
set_mtrr(void* _parameter, int cpu)
{
	struct set_mtrr_parameter* parameter
		= (struct set_mtrr_parameter*)_parameter;

	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous);

	// One CPU has to reset sCpuRendezvous3 -- it is needed to prevent the CPU
	// that initiated the call_all_cpus() from doing that again and clearing
	// sCpuRendezvous2 before the last CPU has actually left the loop in
	// smp_cpu_rendezvous();
	if (cpu == 0)
		atomic_set((int32*)&sCpuRendezvous3, 0);

	disable_caches();

	sCpuModule->set_mtrr(parameter->index, parameter->base, parameter->length,
		parameter->type);

	enable_caches();

	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous2);
	smp_cpu_rendezvous(&sCpuRendezvous3);
}


static void
set_mtrrs(void* _parameter, int cpu)
{
	set_mtrrs_parameter* parameter = (set_mtrrs_parameter*)_parameter;

	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous);

	// One CPU has to reset sCpuRendezvous3 -- it is needed to prevent the CPU
	// that initiated the call_all_cpus() from doing that again and clearing
	// sCpuRendezvous2 before the last CPU has actually left the loop in
	// smp_cpu_rendezvous();
	if (cpu == 0)
		atomic_set((int32*)&sCpuRendezvous3, 0);

	disable_caches();

	sCpuModule->set_mtrrs(parameter->defaultType, parameter->infos,
		parameter->count);

	enable_caches();

	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous2);
	smp_cpu_rendezvous(&sCpuRendezvous3);
}


static void
init_mtrrs(void* _unused, int cpu)
{
	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous);

	// One CPU has to reset sCpuRendezvous3 -- it is needed to prevent the CPU
	// that initiated the call_all_cpus() from doing that again and clearing
	// sCpuRendezvous2 before the last CPU has actually left the loop in
	// smp_cpu_rendezvous();
	if (cpu == 0)
		atomic_set((int32*)&sCpuRendezvous3, 0);

	disable_caches();

	sCpuModule->init_mtrrs();

	enable_caches();

	// wait until all CPUs have arrived here
	smp_cpu_rendezvous(&sCpuRendezvous2);
	smp_cpu_rendezvous(&sCpuRendezvous3);
}


uint32
x86_count_mtrrs(void)
{
	if (sUsePAT) {
		// When PAT is supported, we completely ignore MTRRs and leave them as
		// initialized by firmware. This follows the suggestion in Intel SDM
		// that these don't usually need to be touched by anything after system
		// init. Using page attributes is the more flexible and modern approach
		// to memory type handling and they can override MTRRs in the critical
		// case of write-combining, usually used for framebuffers.
		dprintf("ignoring MTRRs due to PAT support\n");
		return 0;
	}

	if (sCpuModule == NULL)
		return 0;

	return sCpuModule->count_mtrrs();
}


void
x86_set_mtrr(uint32 index, uint64 base, uint64 length, uint8 type)
{
	struct set_mtrr_parameter parameter;
	parameter.index = index;
	parameter.base = base;
	parameter.length = length;
	parameter.type = type;

	sCpuRendezvous = sCpuRendezvous2 = 0;
	call_all_cpus(&set_mtrr, &parameter);
}


status_t
x86_get_mtrr(uint32 index, uint64* _base, uint64* _length, uint8* _type)
{
	// the MTRRs are identical on all CPUs, so it doesn't matter
	// on which CPU this runs
	return sCpuModule->get_mtrr(index, _base, _length, _type);
}


void
x86_set_mtrrs(uint8 defaultType, const x86_mtrr_info* infos, uint32 count)
{
	if (sCpuModule == NULL)
		return;

	struct set_mtrrs_parameter parameter;
	parameter.defaultType = defaultType;
	parameter.infos = infos;
	parameter.count = count;

	sCpuRendezvous = sCpuRendezvous2 = 0;
	call_all_cpus(&set_mtrrs, &parameter);
}


static void
init_pat(int cpu)
{
	disable_caches();

	uint64 value = x86_read_msr(IA32_MSR_PAT);
	dprintf("PAT MSR on CPU %d before init: %#" B_PRIx64 "\n", cpu, value);

	// Use PAT entry 4 for write-combining, leave the rest as is
	value &= ~(IA32_MSR_PAT_ENTRY_MASK << IA32_MSR_PAT_ENTRY_SHIFT(4));
	value |= IA32_MSR_PAT_TYPE_WRITE_COMBINING << IA32_MSR_PAT_ENTRY_SHIFT(4);

	dprintf("PAT MSR on CPU %d after init: %#" B_PRIx64 "\n", cpu, value);
	x86_write_msr(IA32_MSR_PAT, value);

	enable_caches();
}


void
x86_init_fpu(void)
{
	// All x86_64 CPUs support SSE, don't need to bother checking for it.
#ifndef __x86_64__
	if (!x86_check_feature(IA32_FEATURE_FPU, FEATURE_COMMON)) {
		// No FPU... time to install one in your 386?
		dprintf("%s: Warning: CPU has no reported FPU.\n", __func__);
		gX86SwapFPUFunc = x86_noop_swap;
		return;
	}

	if (!x86_check_feature(IA32_FEATURE_SSE, FEATURE_COMMON)
		|| !x86_check_feature(IA32_FEATURE_FXSR, FEATURE_COMMON)) {
		dprintf("%s: CPU has no SSE... just enabling FPU.\n", __func__);
		// we don't have proper SSE support, just enable FPU
		x86_write_cr0(x86_read_cr0() & ~(CR0_FPU_EMULATION | CR0_MONITOR_FPU));
		gX86SwapFPUFunc = x86_fnsave_swap;
		return;
	}
#endif

	dprintf("%s: CPU has SSE... enabling FXSR and XMM.\n", __func__);
#ifndef __x86_64__
	// enable OS support for SSE
	x86_write_cr4(x86_read_cr4() | CR4_OS_FXSR | CR4_OS_XMM_EXCEPTION);
	x86_write_cr0(x86_read_cr0() & ~(CR0_FPU_EMULATION | CR0_MONITOR_FPU));

	gX86SwapFPUFunc = x86_fxsave_swap;
	gHasSSE = true;
#endif
}


#if DUMP_FEATURE_STRING
static void
dump_feature_string(int currentCPU, cpu_ent* cpu)
{
	char features[768];
	features[0] = 0;

	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_FPU)
		strlcat(features, "fpu ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_VME)
		strlcat(features, "vme ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_DE)
		strlcat(features, "de ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PSE)
		strlcat(features, "pse ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_TSC)
		strlcat(features, "tsc ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_MSR)
		strlcat(features, "msr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PAE)
		strlcat(features, "pae ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_MCE)
		strlcat(features, "mce ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_CX8)
		strlcat(features, "cx8 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_APIC)
		strlcat(features, "apic ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_SEP)
		strlcat(features, "sep ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_MTRR)
		strlcat(features, "mtrr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PGE)
		strlcat(features, "pge ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_MCA)
		strlcat(features, "mca ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_CMOV)
		strlcat(features, "cmov ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PAT)
		strlcat(features, "pat ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PSE36)
		strlcat(features, "pse36 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PSN)
		strlcat(features, "psn ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_CLFSH)
		strlcat(features, "clfsh ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_DS)
		strlcat(features, "ds ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_ACPI)
		strlcat(features, "acpi ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_MMX)
		strlcat(features, "mmx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_FXSR)
		strlcat(features, "fxsr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_SSE)
		strlcat(features, "sse ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_SSE2)
		strlcat(features, "sse2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_SS)
		strlcat(features, "ss ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_HTT)
		strlcat(features, "htt ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_TM)
		strlcat(features, "tm ", sizeof(features));
	if (cpu->arch.feature[FEATURE_COMMON] & IA32_FEATURE_PBE)
		strlcat(features, "pbe ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_SSE3)
		strlcat(features, "sse3 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_PCLMULQDQ)
		strlcat(features, "pclmulqdq ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_DTES64)
		strlcat(features, "dtes64 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_MONITOR)
		strlcat(features, "monitor ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_DSCPL)
		strlcat(features, "dscpl ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_VMX)
		strlcat(features, "vmx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_SMX)
		strlcat(features, "smx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_EST)
		strlcat(features, "est ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_TM2)
		strlcat(features, "tm2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_SSSE3)
		strlcat(features, "ssse3 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_CNXTID)
		strlcat(features, "cnxtid ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_FMA)
		strlcat(features, "fma ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_CX16)
		strlcat(features, "cx16 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_XTPR)
		strlcat(features, "xtpr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_PDCM)
		strlcat(features, "pdcm ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_PCID)
		strlcat(features, "pcid ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_DCA)
		strlcat(features, "dca ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_SSE4_1)
		strlcat(features, "sse4_1 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_SSE4_2)
		strlcat(features, "sse4_2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_X2APIC)
		strlcat(features, "x2apic ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_MOVBE)
		strlcat(features, "movbe ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_POPCNT)
		strlcat(features, "popcnt ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_TSCDEADLINE)
		strlcat(features, "tscdeadline ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_AES)
		strlcat(features, "aes ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_XSAVE)
		strlcat(features, "xsave ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_OSXSAVE)
		strlcat(features, "osxsave ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_AVX)
		strlcat(features, "avx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_F16C)
		strlcat(features, "f16c ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_RDRND)
		strlcat(features, "rdrnd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_HYPERVISOR)
		strlcat(features, "hypervisor ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_SYSCALL)
		strlcat(features, "syscall ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_NX)
		strlcat(features, "nx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_MMXEXT)
		strlcat(features, "mmxext ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_FFXSR)
		strlcat(features, "ffxsr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_PDPE1GB)
		strlcat(features, "pdpe1gb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_LONG)
		strlcat(features, "long ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_3DNOWEXT)
		strlcat(features, "3dnowext ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_AMD] & IA32_FEATURE_AMD_EXT_3DNOW)
		strlcat(features, "3dnow ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_DTS)
		strlcat(features, "dts ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_ITB)
		strlcat(features, "itb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_ARAT)
		strlcat(features, "arat ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_PLN)
		strlcat(features, "pln ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_ECMD)
		strlcat(features, "ecmd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_PTM)
		strlcat(features, "ptm ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP)
		strlcat(features, "hwp ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_NOTIFY)
		strlcat(features, "hwp_notify ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_ACTWIN)
		strlcat(features, "hwp_actwin ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_EPP)
		strlcat(features, "hwp_epp ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_PLR)
		strlcat(features, "hwp_plr ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HDC)
		strlcat(features, "hdc ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_TBMT3)
		strlcat(features, "tbmt3 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_CAP)
		strlcat(features, "hwp_cap ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_PECI)
		strlcat(features, "hwp_peci ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_FLEX)
		strlcat(features, "hwp_flex ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_FAST)
		strlcat(features, "hwp_fast ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HW_FEEDBACK)
		strlcat(features, "hw_feedback ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_EAX] & IA32_FEATURE_HWP_IGNIDL)
		strlcat(features, "hwp_ignidl ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_ECX] & IA32_FEATURE_APERFMPERF)
		strlcat(features, "aperfmperf ", sizeof(features));
	if (cpu->arch.feature[FEATURE_6_ECX] & IA32_FEATURE_EPB)
		strlcat(features, "epb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_TSC_ADJUST)
		strlcat(features, "tsc_adjust ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_SGX)
		strlcat(features, "sgx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_BMI1)
		strlcat(features, "bmi1 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_HLE)
		strlcat(features, "hle ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX2)
		strlcat(features, "avx2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_SMEP)
		strlcat(features, "smep ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_BMI2)
		strlcat(features, "bmi2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_ERMS)
		strlcat(features, "erms ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_INVPCID)
		strlcat(features, "invpcid ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_RTM)
		strlcat(features, "rtm ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_CQM)
		strlcat(features, "cqm ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_MPX)
		strlcat(features, "mpx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_RDT_A)
		strlcat(features, "rdt_a ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512F)
		strlcat(features, "avx512f ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512DQ)
		strlcat(features, "avx512dq ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_RDSEED)
		strlcat(features, "rdseed ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_ADX)
		strlcat(features, "adx ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_SMAP)
		strlcat(features, "smap ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512IFMA)
		strlcat(features, "avx512ifma ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_PCOMMIT)
		strlcat(features, "pcommit ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_CLFLUSHOPT)
		strlcat(features, "cflushopt ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_CLWB)
		strlcat(features, "clwb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_INTEL_PT)
		strlcat(features, "intel_pt ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512PF)
		strlcat(features, "avx512pf ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512ER)
		strlcat(features, "avx512er ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512CD)
		strlcat(features, "avx512cd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_SHA_NI)
		strlcat(features, "sha_ni ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512BW)
		strlcat(features, "avx512bw ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EBX] & IA32_FEATURE_AVX512VI)
		strlcat(features, "avx512vi ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_AVX512VMBI)
		strlcat(features, "avx512vmbi ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_UMIP)
		strlcat(features, "umip ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_PKU)
		strlcat(features, "pku ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_OSPKE)
		strlcat(features, "ospke ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_AVX512VMBI2)
		strlcat(features, "avx512vmbi2 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_GFNI)
		strlcat(features, "gfni ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_VAES)
		strlcat(features, "vaes ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_VPCLMULQDQ)
		strlcat(features, "vpclmulqdq ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_AVX512_VNNI)
		strlcat(features, "avx512vnni ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_AVX512_BITALG)
		strlcat(features, "avx512bitalg ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_AVX512_VPOPCNTDQ)
		strlcat(features, "avx512vpopcntdq ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_LA57)
		strlcat(features, "la57 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_RDPID)
		strlcat(features, "rdpid ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_ECX] & IA32_FEATURE_SGX_LC)
		strlcat(features, "sgx_lc ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_HYBRID_CPU)
		strlcat(features, "hybrid ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_IBRS)
		strlcat(features, "ibrs ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_STIBP)
		strlcat(features, "stibp ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_L1D_FLUSH)
		strlcat(features, "l1d_flush ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_ARCH_CAPABILITIES)
		strlcat(features, "msr_arch ", sizeof(features));
	if (cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_SSBD)
		strlcat(features, "ssbd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_7_EDX] & IA32_FEATURE_AMD_HW_PSTATE)
		strlcat(features, "hwpstate ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_7_EDX] & IA32_FEATURE_INVARIANT_TSC)
		strlcat(features, "constant_tsc ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_7_EDX] & IA32_FEATURE_CPB)
		strlcat(features, "cpb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_7_EDX] & IA32_FEATURE_PROC_FEEDBACK)
		strlcat(features, "proc_feedback ", sizeof(features));
	if (cpu->arch.feature[FEATURE_D_1_EAX] & IA32_FEATURE_XSAVEOPT)
		strlcat(features, "xsaveopt ", sizeof(features));
	if (cpu->arch.feature[FEATURE_D_1_EAX] & IA32_FEATURE_XSAVEC)
		strlcat(features, "xsavec ", sizeof(features));
	if (cpu->arch.feature[FEATURE_D_1_EAX] & IA32_FEATURE_XGETBV1)
		strlcat(features, "xgetbv1 ", sizeof(features));
	if (cpu->arch.feature[FEATURE_D_1_EAX] & IA32_FEATURE_XSAVES)
		strlcat(features, "xsaves ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_CLZERO)
		strlcat(features, "clzero ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_IBPB)
		strlcat(features, "ibpb ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_AMD_SSBD)
		strlcat(features, "amd_ssbd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_VIRT_SSBD)
		strlcat(features, "virt_ssbd ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_AMD_SSB_NO)
		strlcat(features, "amd_ssb_no ", sizeof(features));
	if (cpu->arch.feature[FEATURE_EXT_8_EBX] & IA32_FEATURE_CPPC)
		strlcat(features, "cppc ", sizeof(features));
	dprintf("CPU %d: features: %s\n", currentCPU, features);
}
#endif	// DUMP_FEATURE_STRING


static void
compute_cpu_hierarchy_masks(int maxLogicalID, int maxCoreID)
{
	ASSERT(maxLogicalID >= maxCoreID);
	const int kMaxSMTID = maxLogicalID / maxCoreID;

	sHierarchyMask[CPU_TOPOLOGY_SMT] = kMaxSMTID - 1;
	sHierarchyShift[CPU_TOPOLOGY_SMT] = 0;

	sHierarchyMask[CPU_TOPOLOGY_CORE] = (maxCoreID - 1) * kMaxSMTID;
	sHierarchyShift[CPU_TOPOLOGY_CORE]
		= count_set_bits(sHierarchyMask[CPU_TOPOLOGY_SMT]);

	const uint32 kSinglePackageMask = sHierarchyMask[CPU_TOPOLOGY_SMT]
		| sHierarchyMask[CPU_TOPOLOGY_CORE];
	sHierarchyMask[CPU_TOPOLOGY_PACKAGE] = ~kSinglePackageMask;
	sHierarchyShift[CPU_TOPOLOGY_PACKAGE] = count_set_bits(kSinglePackageMask);
}


static uint32
get_cpu_legacy_initial_apic_id(int /* currentCPU */)
{
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 1, 0);
	return cpuid.regs.ebx >> 24;
}


static inline status_t
detect_amd_cpu_topology(uint32 maxBasicLeaf, uint32 maxExtendedLeaf)
{
	sGetCPUTopologyID = get_cpu_legacy_initial_apic_id;

	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 1, 0);
	int maxLogicalID = next_power_of_2((cpuid.regs.ebx >> 16) & 0xff);

	int maxCoreID = 1;
	if (maxExtendedLeaf >= 0x80000008) {
		get_current_cpuid(&cpuid, 0x80000008, 0);
		maxCoreID = (cpuid.regs.ecx >> 12) & 0xf;
		if (maxCoreID != 0)
			maxCoreID = 1 << maxCoreID;
		else
			maxCoreID = next_power_of_2((cpuid.regs.edx & 0xf) + 1);
	}

	if (maxExtendedLeaf >= 0x80000001) {
		get_current_cpuid(&cpuid, 0x80000001, 0);
		if (x86_check_feature(IA32_FEATURE_AMD_EXT_CMPLEGACY,
				FEATURE_EXT_AMD_ECX))
			maxCoreID = maxLogicalID;
	}

	compute_cpu_hierarchy_masks(maxLogicalID, maxCoreID);

	return B_OK;
}


static void
detect_amd_cache_topology(uint32 maxExtendedLeaf)
{
	if (!x86_check_feature(IA32_FEATURE_AMD_EXT_TOPOLOGY, FEATURE_EXT_AMD_ECX))
		return;

	if (maxExtendedLeaf < 0x8000001d)
		return;

	uint8 hierarchyLevels[CPU_MAX_CACHE_LEVEL];
	int maxCacheLevel = 0;

	int currentLevel = 0;
	int cacheType;
	do {
		cpuid_info cpuid;
		get_current_cpuid(&cpuid, 0x8000001d, currentLevel);

		cacheType = cpuid.regs.eax & 0x1f;
		if (cacheType == 0)
			break;

		int cacheLevel = (cpuid.regs.eax >> 5) & 0x7;
		int coresCount = next_power_of_2(((cpuid.regs.eax >> 14) & 0x3f) + 1);
		hierarchyLevels[cacheLevel - 1]
			= coresCount * (sHierarchyMask[CPU_TOPOLOGY_SMT] + 1);
		maxCacheLevel = std::max(maxCacheLevel, cacheLevel);

		currentLevel++;
	} while (true);

	for (int i = 0; i < maxCacheLevel; i++)
		sCacheSharingMask[i] = ~uint32(hierarchyLevels[i] - 1);
	gCPUCacheLevelCount = maxCacheLevel;
}


static uint32
get_intel_cpu_initial_x2apic_id(int /* currentCPU */)
{
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 11, 0);
	return cpuid.regs.edx;
}


static inline status_t
detect_intel_cpu_topology_x2apic(uint32 maxBasicLeaf)
{

	uint32 leaf = 0;
	cpuid_info cpuid;
	if (maxBasicLeaf >= 0x1f) {
		get_current_cpuid(&cpuid, 0x1f, 0);
		if (cpuid.regs.ebx != 0)
			leaf = 0x1f;
	}
	if (maxBasicLeaf >= 0xb && leaf == 0) {
		get_current_cpuid(&cpuid, 0xb, 0);
		if (cpuid.regs.ebx != 0)
			leaf = 0xb;
	}
	if (leaf == 0)
		return B_UNSUPPORTED;

	uint8 hierarchyLevels[CPU_TOPOLOGY_LEVELS] = { 0 };

	int currentLevel = 0;
	unsigned int levelsSet = 0;
	do {
		cpuid_info cpuid;
		get_current_cpuid(&cpuid, leaf, currentLevel++);
		int levelType = (cpuid.regs.ecx >> 8) & 0xff;
		int levelValue = cpuid.regs.eax & 0x1f;

		if (levelType == 0)
			break;

		switch (levelType) {
			case 1:	// SMT
				hierarchyLevels[CPU_TOPOLOGY_SMT] = levelValue;
				levelsSet |= 1;
				break;
			case 2:	// core
				hierarchyLevels[CPU_TOPOLOGY_CORE] = levelValue;
				levelsSet |= 2;
				break;
		}

	} while (levelsSet != 3);

	sGetCPUTopologyID = get_intel_cpu_initial_x2apic_id;

	for (int i = 1; i < CPU_TOPOLOGY_LEVELS; i++) {
		if ((levelsSet & (1u << i)) != 0)
			continue;
		hierarchyLevels[i] = hierarchyLevels[i - 1];
	}

	for (int i = 0; i < CPU_TOPOLOGY_LEVELS; i++) {
		uint32 mask = ~uint32(0);
		if (i < CPU_TOPOLOGY_LEVELS - 1)
			mask = (1u << hierarchyLevels[i]) - 1;
		if (i > 0)
			mask &= ~sHierarchyMask[i - 1];
		sHierarchyMask[i] = mask;
		sHierarchyShift[i] = i > 0 ? hierarchyLevels[i - 1] : 0;
	}

	return B_OK;
}


static inline status_t
detect_intel_cpu_topology_legacy(uint32 maxBasicLeaf)
{
	sGetCPUTopologyID = get_cpu_legacy_initial_apic_id;

	cpuid_info cpuid;

	get_current_cpuid(&cpuid, 1, 0);
	int maxLogicalID = next_power_of_2((cpuid.regs.ebx >> 16) & 0xff);

	int maxCoreID = 1;
	if (maxBasicLeaf >= 4) {
		get_current_cpuid(&cpuid, 4, 0);
		maxCoreID = next_power_of_2((cpuid.regs.eax >> 26) + 1);
	}

	compute_cpu_hierarchy_masks(maxLogicalID, maxCoreID);

	return B_OK;
}


static void
detect_intel_cache_topology(uint32 maxBasicLeaf)
{
	if (maxBasicLeaf < 4)
		return;

	uint8 hierarchyLevels[CPU_MAX_CACHE_LEVEL];
	int maxCacheLevel = 0;

	int currentLevel = 0;
	int cacheType;
	do {
		cpuid_info cpuid;
		get_current_cpuid(&cpuid, 4, currentLevel);

		cacheType = cpuid.regs.eax & 0x1f;
		if (cacheType == 0)
			break;

		int cacheLevel = (cpuid.regs.eax >> 5) & 0x7;
		hierarchyLevels[cacheLevel - 1]
			= next_power_of_2(((cpuid.regs.eax >> 14) & 0x3f) + 1);
		maxCacheLevel = std::max(maxCacheLevel, cacheLevel);

		currentLevel++;
	} while (true);

	for (int i = 0; i < maxCacheLevel; i++)
		sCacheSharingMask[i] = ~uint32(hierarchyLevels[i] - 1);

	gCPUCacheLevelCount = maxCacheLevel;
}


static uint32
get_simple_cpu_topology_id(int currentCPU)
{
	return currentCPU;
}


static inline int
get_topology_level_id(uint32 id, cpu_topology_level level)
{
	ASSERT(level < CPU_TOPOLOGY_LEVELS);
	return (id & sHierarchyMask[level]) >> sHierarchyShift[level];
}


static void
detect_cpu_topology(int currentCPU, cpu_ent* cpu, uint32 maxBasicLeaf,
	uint32 maxExtendedLeaf)
{
	if (currentCPU == 0) {
		memset(sCacheSharingMask, 0xff, sizeof(sCacheSharingMask));

		status_t result = B_UNSUPPORTED;
		if (x86_check_feature(IA32_FEATURE_HTT, FEATURE_COMMON)) {
			if (cpu->arch.vendor == VENDOR_AMD
				|| cpu->arch.vendor == VENDOR_HYGON) {
				result = detect_amd_cpu_topology(maxBasicLeaf, maxExtendedLeaf);

				if (result == B_OK)
					detect_amd_cache_topology(maxExtendedLeaf);
			}

			if (cpu->arch.vendor == VENDOR_INTEL) {
				result = detect_intel_cpu_topology_x2apic(maxBasicLeaf);
				if (result != B_OK)
					result = detect_intel_cpu_topology_legacy(maxBasicLeaf);

				if (result == B_OK)
					detect_intel_cache_topology(maxBasicLeaf);
			}
		}

		if (result != B_OK) {
			dprintf("No CPU topology information available.\n");

			sGetCPUTopologyID = get_simple_cpu_topology_id;

			sHierarchyMask[CPU_TOPOLOGY_PACKAGE] = ~uint32(0);
		}
	}

	ASSERT(sGetCPUTopologyID != NULL);
	int topologyID = sGetCPUTopologyID(currentCPU);
	cpu->topology_id[CPU_TOPOLOGY_SMT]
		= get_topology_level_id(topologyID, CPU_TOPOLOGY_SMT);
	cpu->topology_id[CPU_TOPOLOGY_CORE]
		= get_topology_level_id(topologyID, CPU_TOPOLOGY_CORE);
	cpu->topology_id[CPU_TOPOLOGY_PACKAGE]
		= get_topology_level_id(topologyID, CPU_TOPOLOGY_PACKAGE);

	unsigned int i;
	for (i = 0; i < gCPUCacheLevelCount; i++)
		cpu->cache_id[i] = topologyID & sCacheSharingMask[i];
	for (; i < CPU_MAX_CACHE_LEVEL; i++)
		cpu->cache_id[i] = -1;

#if DUMP_CPU_TOPOLOGY
	dprintf("CPU %d: apic id %d, package %d, core %d, smt %d\n", currentCPU,
		topologyID, cpu->topology_id[CPU_TOPOLOGY_PACKAGE],
		cpu->topology_id[CPU_TOPOLOGY_CORE],
		cpu->topology_id[CPU_TOPOLOGY_SMT]);

	if (gCPUCacheLevelCount > 0) {
		char cacheLevels[256];
		unsigned int offset = 0;
		for (i = 0; i < gCPUCacheLevelCount; i++) {
			offset += snprintf(cacheLevels + offset,
					sizeof(cacheLevels) - offset,
					" L%d id %d%s", i + 1, cpu->cache_id[i],
					i < gCPUCacheLevelCount - 1 ? "," : "");

			if (offset >= sizeof(cacheLevels))
				break;
		}

		dprintf("CPU %d: cache sharing:%s\n", currentCPU, cacheLevels);
	}
#endif
}


static void
detect_intel_patch_level(cpu_ent* cpu)
{
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_HYPERVISOR) {
		cpu->arch.patch_level = 0;
		return;
	}

	x86_write_msr(IA32_MSR_UCODE_REV, 0);
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 1, 0);

	uint64 value = x86_read_msr(IA32_MSR_UCODE_REV);
	cpu->arch.patch_level = value >> 32;
}


static void
detect_amd_patch_level(cpu_ent* cpu)
{
	if (cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_HYPERVISOR) {
		cpu->arch.patch_level = 0;
		return;
	}

	uint64 value = x86_read_msr(IA32_MSR_UCODE_REV);
	cpu->arch.patch_level = (uint32)value;
}


static struct intel_microcode_header*
find_microcode_intel(addr_t data, size_t size, uint32 patchLevel)
{
	// 9.11.3 Processor Identification
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 1, 0);
	uint32 signature = cpuid.regs.eax;
	// 9.11.4 Platform Identification
	uint64 platformBits = (x86_read_msr(IA32_MSR_PLATFORM_ID) >> 50) & 0x7;
	uint64 mask = 1 << platformBits;

	while (size > 0) {
		if (size < sizeof(struct intel_microcode_header)) {
			dprintf("find_microcode_intel update is too small for header\n");
			break;
		}
		struct intel_microcode_header* header =
			(struct intel_microcode_header*)data;

		uint32 totalSize = header->total_size;
		uint32 dataSize = header->data_size;
		if (dataSize == 0) {
			dataSize = 2000;
			totalSize = sizeof(struct intel_microcode_header)
				+ dataSize;
		}
		if (totalSize > size) {
			dprintf("find_microcode_intel update is too small for data\n");
			break;
		}

		uint32* dwords = (uint32*)data;
		// prepare the next update
		size -= totalSize;
		data += totalSize;

		if (header->loader_revision != 1) {
			dprintf("find_microcode_intel incorrect loader version\n");
			continue;
		}
		// 9.11.6 The microcode update data requires a 16-byte boundary
		// alignment.
		if (((addr_t)header % 16) != 0) {
			dprintf("find_microcode_intel incorrect alignment\n");
			continue;
		}
		uint32 sum = 0;
		for (uint32 i = 0; i < totalSize / 4; i++) {
			sum += dwords[i];
		}
		if (sum != 0) {
			dprintf("find_microcode_intel incorrect checksum\n");
			continue;
		}
		if (patchLevel > header->update_revision) {
			dprintf("find_microcode_intel update_revision is lower\n");
			continue;
		}
		if (signature == header->processor_signature
			&& (mask & header->processor_flags) != 0) {
			return header;
		}
		if (totalSize <= (sizeof(struct intel_microcode_header) + dataSize
			+ sizeof(struct intel_microcode_extended_signature_header))) {
			continue;
		}
		struct intel_microcode_extended_signature_header* extSigHeader =
			(struct intel_microcode_extended_signature_header*)((addr_t)header
				+ sizeof(struct intel_microcode_header) + dataSize);
		struct intel_microcode_extended_signature* extended_signature =
			(struct intel_microcode_extended_signature*)((addr_t)extSigHeader
				+ sizeof(struct intel_microcode_extended_signature_header));
		for (uint32 i = 0; i < extSigHeader->extended_signature_count; i++) {
			if (signature == extended_signature[i].processor_signature
				&& (mask & extended_signature[i].processor_flags) != 0)
				return header;
		}
	}
	return NULL;
}


static void
load_microcode_intel(int currentCPU, cpu_ent* cpu)
{
	// serialize for HT cores
	if (currentCPU != 0)
		acquire_spinlock(&sUcodeUpdateLock);
	detect_intel_patch_level(cpu);
	uint32 revision = cpu->arch.patch_level;
	struct intel_microcode_header* update = (struct intel_microcode_header*)sLoadedUcodeUpdate;
	if (update == NULL) {
		update = find_microcode_intel((addr_t)sUcodeData, sUcodeDataSize,
			revision);
	}
	if (update == NULL) {
		dprintf("CPU %d: no update found\n", currentCPU);
	} else if (update->update_revision != revision) {
		addr_t data = (addr_t)update + sizeof(struct intel_microcode_header);
		wbinvd();
		x86_write_msr(IA32_MSR_UCODE_WRITE, data);
		detect_intel_patch_level(cpu);
		if (revision == cpu->arch.patch_level) {
			dprintf("CPU %d: update failed\n", currentCPU);
		} else {
			if (sLoadedUcodeUpdate == NULL)
				sLoadedUcodeUpdate = update;
			dprintf("CPU %d: updated from revision 0x%" B_PRIx32 " to 0x%" B_PRIx32
				"\n", currentCPU, revision, cpu->arch.patch_level);
		}
	}
	if (currentCPU != 0)
		release_spinlock(&sUcodeUpdateLock);
}


static struct amd_microcode_header*
find_microcode_amd(addr_t data, size_t size, uint32 patchLevel)
{
	// 9.11.3 Processor Identification
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 1, 0);
	uint32 signature = cpuid.regs.eax;

	if (size < sizeof(struct amd_container_header)) {
		dprintf("find_microcode_amd update is too small for header\n");
		return NULL;
	}
	struct amd_container_header* container = (struct amd_container_header*)data;
	if (container->magic != 0x414d44) {
		dprintf("find_microcode_amd update invalid magic\n");
		return NULL;
	}

	size -= sizeof(*container);
	data += sizeof(*container);

	struct amd_section_header* section =
		(struct amd_section_header*)data;
	if (section->type != 0 || section->size == 0) {
		dprintf("find_microcode_amd update first section invalid\n");
		return NULL;
	}

	size -= sizeof(*section);
	data += sizeof(*section);

	amd_equiv_cpu_entry* table = (amd_equiv_cpu_entry*)data;
	size -= section->size;
	data += section->size;

	uint16 equiv_id = 0;
	for (uint32 i = 0; table[i].installed_cpu != 0; i++) {
		if (signature == table[i].equiv_cpu) {
			equiv_id = table[i].equiv_cpu;
			dprintf("find_microcode_amd found equiv cpu: %x\n", equiv_id);
			break;
		}
	}
	if (equiv_id == 0) {
		dprintf("find_microcode_amd update cpu not found in equiv table\n");
		return NULL;
	}

	while (size > sizeof(amd_section_header)) {
		struct amd_section_header* section = (struct amd_section_header*)data;
		size -= sizeof(*section);
		data += sizeof(*section);

		if (section->type != 1 || section->size > size
			|| section->size < sizeof(amd_microcode_header)) {
			dprintf("find_microcode_amd update firmware section invalid\n");
			return NULL;
		}
		struct amd_microcode_header* header = (struct amd_microcode_header*)data;
		size -= section->size;
		data += section->size;

		if (header->processor_rev_id != equiv_id) {
			dprintf("find_microcode_amd update found rev_id %x\n", header->processor_rev_id);
			continue;
		}
		if (patchLevel >= header->patch_id) {
			dprintf("find_microcode_intel update_revision is lower\n");
			continue;
		}
		if (header->nb_dev_id != 0 || header->sb_dev_id != 0) {
			dprintf("find_microcode_amd update chipset specific firmware\n");
			continue;
		}
		if (((addr_t)header % 16) != 0) {
			dprintf("find_microcode_amd incorrect alignment\n");
			continue;
		}

		return header;
	}
	dprintf("find_microcode_amd no fw update found for this cpu\n");
	return NULL;
}


static void
load_microcode_amd(int currentCPU, cpu_ent* cpu)
{
	// serialize for HT cores
	if (currentCPU != 0)
		acquire_spinlock(&sUcodeUpdateLock);
	detect_amd_patch_level(cpu);
	uint32 revision = cpu->arch.patch_level;
	struct amd_microcode_header* update = (struct amd_microcode_header*)sLoadedUcodeUpdate;
	if (update == NULL) {
		update = find_microcode_amd((addr_t)sUcodeData, sUcodeDataSize,
			revision);
	}
	if (update != NULL) {
		addr_t data = (addr_t)update;
		wbinvd();

		x86_write_msr(MSR_K8_UCODE_UPDATE, data);

		detect_amd_patch_level(cpu);
		if (revision == cpu->arch.patch_level) {
			dprintf("CPU %d: update failed\n", currentCPU);
		} else {
			if (sLoadedUcodeUpdate == NULL)
				sLoadedUcodeUpdate = update;
			dprintf("CPU %d: updated from revision 0x%" B_PRIx32 " to 0x%" B_PRIx32
				"\n", currentCPU, revision, cpu->arch.patch_level);
		}

	} else {
		dprintf("CPU %d: no update found\n", currentCPU);
	}

	if (currentCPU != 0)
		release_spinlock(&sUcodeUpdateLock);
}


static void
load_microcode(int currentCPU)
{
	if (sUcodeData == NULL)
		return;
	cpu_ent* cpu = get_cpu_struct();
	if ((cpu->arch.feature[FEATURE_EXT] & IA32_FEATURE_EXT_HYPERVISOR) != 0)
		return;
	if (cpu->arch.vendor == VENDOR_INTEL)
		load_microcode_intel(currentCPU, cpu);
	else if (cpu->arch.vendor == VENDOR_AMD)
		load_microcode_amd(currentCPU, cpu);
}


static uint8
get_hybrid_cpu_type()
{
	cpu_ent* cpu = get_cpu_struct();
	if ((cpu->arch.feature[FEATURE_7_EDX] & IA32_FEATURE_HYBRID_CPU) == 0)
		return 0;

#define X86_HYBRID_CPU_TYPE_ID_SHIFT       24
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 0x1a, 0);
	return cpuid.regs.eax >> X86_HYBRID_CPU_TYPE_ID_SHIFT;
}


static const char*
get_hybrid_cpu_type_string(uint8 type)
{
	switch (type) {
		case 0x20:
			return "Atom";
		case 0x40:
			return "Core";
		default:
			return "";
	}
}


static void
detect_cpu(int currentCPU, bool full = true)
{
	cpu_ent* cpu = get_cpu_struct();
	char vendorString[17];
	cpuid_info cpuid;

	// clear out the cpu info data
	cpu->arch.vendor = VENDOR_UNKNOWN;
	cpu->arch.vendor_name = "UNKNOWN VENDOR";
	cpu->arch.feature[FEATURE_COMMON] = 0;
	cpu->arch.feature[FEATURE_EXT] = 0;
	cpu->arch.feature[FEATURE_EXT_AMD] = 0;
	cpu->arch.feature[FEATURE_7_EBX] = 0;
	cpu->arch.feature[FEATURE_7_ECX] = 0;
	cpu->arch.feature[FEATURE_7_EDX] = 0;
	cpu->arch.feature[FEATURE_D_1_EAX] = 0;
	cpu->arch.model_name[0] = 0;

	// print some fun data
	get_current_cpuid(&cpuid, 0, 0);
	uint32 maxBasicLeaf = cpuid.eax_0.max_eax;

	// build the vendor string
	memset(vendorString, 0, sizeof(vendorString));
	memcpy(vendorString, cpuid.eax_0.vendor_id, sizeof(cpuid.eax_0.vendor_id));

	// get the family, model, stepping
	get_current_cpuid(&cpuid, 1, 0);
	cpu->arch.type = cpuid.eax_1.type;
	cpu->arch.family = cpuid.eax_1.family;
	cpu->arch.extended_family = cpuid.eax_1.extended_family;
	cpu->arch.model = cpuid.eax_1.model;
	cpu->arch.extended_model = cpuid.eax_1.extended_model;
	cpu->arch.stepping = cpuid.eax_1.stepping;
	if (full) {
		dprintf("CPU %d: type %d family %d extended_family %d model %d "
			"extended_model %d stepping %d, string '%s'\n",
			currentCPU, cpu->arch.type, cpu->arch.family,
			cpu->arch.extended_family, cpu->arch.model,
			cpu->arch.extended_model, cpu->arch.stepping, vendorString);
	}

	// figure out what vendor we have here

	for (int32 i = 0; i < VENDOR_NUM; i++) {
		if (vendor_info[i].ident_string[0]
			&& !strcmp(vendorString, vendor_info[i].ident_string[0])) {
			cpu->arch.vendor = (x86_vendors)i;
			cpu->arch.vendor_name = vendor_info[i].vendor;
			break;
		}
		if (vendor_info[i].ident_string[1]
			&& !strcmp(vendorString, vendor_info[i].ident_string[1])) {
			cpu->arch.vendor = (x86_vendors)i;
			cpu->arch.vendor_name = vendor_info[i].vendor;
			break;
		}
	}

	// see if we can get the model name
	get_current_cpuid(&cpuid, 0x80000000, 0);
	uint32 maxExtendedLeaf = cpuid.eax_0.max_eax;
	if (maxExtendedLeaf >= 0x80000004) {
		// build the model string (need to swap ecx/edx data before copying)
		unsigned int temp;
		memset(cpu->arch.model_name, 0, sizeof(cpu->arch.model_name));

		get_current_cpuid(&cpuid, 0x80000002, 0);
		temp = cpuid.regs.edx;
		cpuid.regs.edx = cpuid.regs.ecx;
		cpuid.regs.ecx = temp;
		memcpy(cpu->arch.model_name, cpuid.as_chars, sizeof(cpuid.as_chars));

		get_current_cpuid(&cpuid, 0x80000003, 0);
		temp = cpuid.regs.edx;
		cpuid.regs.edx = cpuid.regs.ecx;
		cpuid.regs.ecx = temp;
		memcpy(cpu->arch.model_name + 16, cpuid.as_chars,
			sizeof(cpuid.as_chars));

		get_current_cpuid(&cpuid, 0x80000004, 0);
		temp = cpuid.regs.edx;
		cpuid.regs.edx = cpuid.regs.ecx;
		cpuid.regs.ecx = temp;
		memcpy(cpu->arch.model_name + 32, cpuid.as_chars,
			sizeof(cpuid.as_chars));

		// some cpus return a right-justified string
		int32 i = 0;
		while (cpu->arch.model_name[i] == ' ')
			i++;
		if (i > 0) {
			memmove(cpu->arch.model_name, &cpu->arch.model_name[i],
				strlen(&cpu->arch.model_name[i]) + 1);
		}

		if (full) {
			dprintf("CPU %d: vendor '%s' model name '%s'\n",
				currentCPU, cpu->arch.vendor_name, cpu->arch.model_name);
		}
	} else {
		strlcpy(cpu->arch.model_name, "unknown", sizeof(cpu->arch.model_name));
	}

	// load feature bits
	get_current_cpuid(&cpuid, 1, 0);
	cpu->arch.feature[FEATURE_COMMON] = cpuid.eax_1.features; // edx
	cpu->arch.feature[FEATURE_EXT] = cpuid.eax_1.extended_features; // ecx

	if (!full)
		return;

	if (maxExtendedLeaf >= 0x80000001) {
		get_current_cpuid(&cpuid, 0x80000001, 0);
		if (cpu->arch.vendor == VENDOR_AMD)
			cpu->arch.feature[FEATURE_EXT_AMD_ECX] = cpuid.regs.ecx; // ecx
		cpu->arch.feature[FEATURE_EXT_AMD] = cpuid.regs.edx; // edx
		if (cpu->arch.vendor != VENDOR_AMD)
			cpu->arch.feature[FEATURE_EXT_AMD] &= IA32_FEATURES_INTEL_EXT;
	}

	if (maxBasicLeaf >= 5) {
		get_current_cpuid(&cpuid, 5, 0);
		cpu->arch.feature[FEATURE_5_ECX] = cpuid.regs.ecx;
	}

	if (maxBasicLeaf >= 6) {
		get_current_cpuid(&cpuid, 6, 0);
		cpu->arch.feature[FEATURE_6_EAX] = cpuid.regs.eax;
		cpu->arch.feature[FEATURE_6_ECX] = cpuid.regs.ecx;
	}

	if (maxBasicLeaf >= 7) {
		get_current_cpuid(&cpuid, 7, 0);
		cpu->arch.feature[FEATURE_7_EBX] = cpuid.regs.ebx;
		cpu->arch.feature[FEATURE_7_ECX] = cpuid.regs.ecx;
		cpu->arch.feature[FEATURE_7_EDX] = cpuid.regs.edx;
	}

	if (maxBasicLeaf >= 0xd) {
		get_current_cpuid(&cpuid, 0xd, 1);
		cpu->arch.feature[FEATURE_D_1_EAX] = cpuid.regs.eax;
	}

	if (maxExtendedLeaf >= 0x80000007) {
		get_current_cpuid(&cpuid, 0x80000007, 0);
		cpu->arch.feature[FEATURE_EXT_7_EDX] = cpuid.regs.edx;
	}

	if (maxExtendedLeaf >= 0x80000008) {
		get_current_cpuid(&cpuid, 0x80000008, 0);
			cpu->arch.feature[FEATURE_EXT_8_EBX] = cpuid.regs.ebx;
	}

	detect_cpu_topology(currentCPU, cpu, maxBasicLeaf, maxExtendedLeaf);

	if (cpu->arch.vendor == VENDOR_INTEL)
		detect_intel_patch_level(cpu);
	else if (cpu->arch.vendor == VENDOR_AMD)
		detect_amd_patch_level(cpu);

	cpu->arch.hybrid_type = get_hybrid_cpu_type();

#if DUMP_FEATURE_STRING
	dump_feature_string(currentCPU, cpu);
#endif
#if DUMP_CPU_PATCHLEVEL_TYPE
	dprintf("CPU %d: patch_level 0x%" B_PRIx32 "%s%s\n", currentCPU,
		cpu->arch.patch_level,
		cpu->arch.hybrid_type != 0 ? ", hybrid type ": "",
		get_hybrid_cpu_type_string(cpu->arch.hybrid_type));
#endif
}


bool
x86_check_feature(uint32 feature, enum x86_feature_type type)
{
	cpu_ent* cpu = get_cpu_struct();

#if 0
	int i;
	dprintf("x86_check_feature: feature 0x%x, type %d\n", feature, type);
	for (i = 0; i < FEATURE_NUM; i++) {
		dprintf("features %d: 0x%x\n", i, cpu->arch.feature[i]);
	}
#endif

	return (cpu->arch.feature[type] & feature) != 0;
}


bool
x86_use_pat()
{
	return sUsePAT;
}


void*
x86_get_double_fault_stack(int32 cpu, size_t* _size)
{
	*_size = kDoubleFaultStackSize;
	return (void*)(sDoubleFaultStacks + kDoubleFaultStackSize * cpu);
}


/*!	Returns the index of the current CPU. Can only be called from the double
	fault handler.
*/
int32
x86_double_fault_get_cpu(void)
{
	addr_t stack = x86_get_stack_frame();
	return (stack - sDoubleFaultStacks) / kDoubleFaultStackSize;
}


//	#pragma mark -


status_t
arch_cpu_preboot_init_percpu(kernel_args* args, int cpu)
{
	if (cpu == 0) {
		// We can't allocate pages at this stage in the boot process, only virtual addresses.
		sDoubleFaultStacks = vm_allocate_early(args,
			kDoubleFaultStackSize * smp_get_num_cpus(), 0, 0, 0);
	}

	// On SMP system we want to synchronize the CPUs' TSCs, so system_time()
	// will return consistent values.
	if (smp_get_num_cpus() > 1) {
		// let the first CPU prepare the rendezvous point
		if (cpu == 0)
			sTSCSyncRendezvous = smp_get_num_cpus() - 1;

		// One CPU after the other will drop out of this loop and be caught by
		// the loop below, until the last CPU (0) gets there. Save for +/- a few
		// cycles the CPUs should pass the second loop at the same time.
		while (sTSCSyncRendezvous != cpu) {
		}

		sTSCSyncRendezvous = cpu - 1;

		while (sTSCSyncRendezvous != -1) {
		}

		// reset TSC to 0
		x86_write_msr(IA32_MSR_TSC, 0);
	}

	x86_descriptors_preboot_init_percpu(args, cpu);

	return B_OK;
}


static void
halt_idle(void)
{
	asm("hlt");
}


static void
amdc1e_noarat_idle(void)
{
	uint64 msr = x86_read_msr(K8_MSR_IPM);
	if (msr & K8_CMPHALT)
		x86_write_msr(K8_MSR_IPM, msr & ~K8_CMPHALT);
	halt_idle();
}


static bool
detect_amdc1e_noarat()
{
	cpu_ent* cpu = get_cpu_struct();

	if (cpu->arch.vendor != VENDOR_AMD)
		return false;

	// Family 0x12 and higher processors support ARAT
	// Family lower than 0xf processors doesn't support C1E
	// Family 0xf with model <= 0x40 procssors doesn't support C1E
	uint32 family = cpu->arch.family + cpu->arch.extended_family;
	uint32 model = (cpu->arch.extended_model << 4) | cpu->arch.model;
	return (family < 0x12 && family > 0xf) || (family == 0xf && model > 0x40);
}


static void
init_tsc_with_cpuid(kernel_args* args, uint32* conversionFactor)
{
	cpu_ent* cpu = get_cpu_struct();
	if (cpu->arch.vendor != VENDOR_INTEL)
		return;

	uint32 model = (cpu->arch.extended_model << 4) | cpu->arch.model;
	cpuid_info cpuid;
	get_current_cpuid(&cpuid, 0, 0);
	uint32 maxBasicLeaf = cpuid.eax_0.max_eax;
	if (maxBasicLeaf < 0x15)
		return;

	get_current_cpuid(&cpuid, 0x15, 0);
	if (cpuid.regs.eax == 0 || cpuid.regs.ebx == 0)
		return;
	uint32 khz = cpuid.regs.ecx / 1000;
	uint32 denominator = cpuid.regs.eax;
	uint32 numerator = cpuid.regs.ebx;
	if (khz == 0 && model == 0x5f) {
		// CPUID 0x16 isn't supported, hardcoding
		khz = 25000;
	}

	if (khz == 0 && maxBasicLeaf >= 0x16) {
		// for these CPUs the base frequency is also the tsc frequency
		get_current_cpuid(&cpuid, 0x16, 0);
		khz = cpuid.regs.eax * 1000 * denominator / numerator;
	}
	if (khz == 0)
		return;

	dprintf("CPU: using TSC frequency from CPUID\n");
	// compute for microseconds as follows (1000000 << 32) / (tsc freq in Hz),
	// or (1000 << 32) / (tsc freq in kHz)
	*conversionFactor = (1000ULL << 32) / (khz * numerator / denominator);
	// overwrite the bootloader value
	args->arch_args.system_time_cv_factor = *conversionFactor;
}


static void
init_tsc_with_msr(kernel_args* args, uint32* conversionFactor)
{
	cpu_ent* cpuEnt = get_cpu_struct();
	if (cpuEnt->arch.vendor != VENDOR_AMD)
		return;

	uint32 family = cpuEnt->arch.family + cpuEnt->arch.extended_family;
	if (family < 0x10)
		return;
	uint64 value = x86_read_msr(MSR_F10H_HWCR);
	if ((value & HWCR_TSCFREQSEL) == 0)
		return;

	value = x86_read_msr(MSR_F10H_PSTATEDEF(0));
	if ((value & PSTATEDEF_EN) == 0)
		return;
	if (family != 0x17 && family != 0x19)
		return;

	uint64 khz = 200 * 1000;
	uint32 denominator = (value >> 8) & 0x3f;
	if (denominator < 0x8 || denominator > 0x2c)
		return;
	if (denominator > 0x1a && (denominator % 2) == 1)
		return;
	uint32 numerator = value & 0xff;
	if (numerator < 0x10)
		return;

	dprintf("CPU: using TSC frequency from MSR %" B_PRIu64 "\n", khz * numerator / denominator);
	// compute for microseconds as follows (1000000 << 32) / (tsc freq in Hz),
	// or (1000 << 32) / (tsc freq in kHz)
	*conversionFactor = (1000ULL << 32) / (khz * numerator / denominator);
	// overwrite the bootloader value
	args->arch_args.system_time_cv_factor = *conversionFactor;
}


static void
init_tsc(kernel_args* args)
{
	// init the TSC -> system_time() conversion factors

	// try to find the TSC frequency with CPUID
	uint32 conversionFactor = args->arch_args.system_time_cv_factor;
	init_tsc_with_cpuid(args, &conversionFactor);
	init_tsc_with_msr(args, &conversionFactor);
	uint64 conversionFactorNsecs = (uint64)conversionFactor * 1000;

#ifdef __x86_64__
	// The x86_64 system_time() implementation uses 64-bit multiplication and
	// therefore shifting is not necessary for low frequencies (it's also not
	// too likely that there'll be any x86_64 CPUs clocked under 1GHz).
	__x86_setup_system_time((uint64)conversionFactor << 32,
		conversionFactorNsecs);
#else
	if (conversionFactorNsecs >> 32 != 0) {
		// the TSC frequency is < 1 GHz, which forces us to shift the factor
		__x86_setup_system_time(conversionFactor, conversionFactorNsecs >> 16,
			true);
	} else {
		// the TSC frequency is >= 1 GHz
		__x86_setup_system_time(conversionFactor, conversionFactorNsecs, false);
	}
#endif
}


status_t
arch_cpu_init_percpu(kernel_args* args, int cpu)
{
	detect_cpu(cpu, false);
	load_microcode(cpu);
	detect_cpu(cpu);

	if (cpu == 0)
		init_tsc(args);

	if (!gCpuIdleFunc) {
		if (detect_amdc1e_noarat())
			gCpuIdleFunc = amdc1e_noarat_idle;
		else
			gCpuIdleFunc = halt_idle;
	}

	if (x86_check_feature(IA32_FEATURE_MCE, FEATURE_COMMON))
		x86_write_cr4(x86_read_cr4() | IA32_CR4_MCE);

	cpu_ent* cpuEnt = get_cpu_struct();
	if (cpu == 0) {
		bool supportsPAT = x86_check_feature(IA32_FEATURE_PAT, FEATURE_COMMON);

		// Pentium II Errata A52 and Pentium III Errata E27 say the upper four
		// entries of the PAT are not useable as the PAT bit is ignored for 4K
		// PTEs. Pentium 4 Errata N46 says the PAT bit can be assumed 0 in some
		// specific cases. To avoid issues, disable PAT on such CPUs.
		bool brokenPAT = cpuEnt->arch.vendor == VENDOR_INTEL
			&& cpuEnt->arch.extended_family == 0
			&& cpuEnt->arch.extended_model == 0
			&& ((cpuEnt->arch.family == 6 && cpuEnt->arch.model <= 13)
				|| (cpuEnt->arch.family == 15 && cpuEnt->arch.model <= 6));

		sUsePAT = supportsPAT && !brokenPAT
			&& !get_safemode_boolean_early(args, B_SAFEMODE_DISABLE_PAT, false);

		if (sUsePAT) {
			dprintf("using PAT for memory type configuration\n");
		} else {
			dprintf("not using PAT for memory type configuration (%s)\n",
				supportsPAT ? (brokenPAT ? "broken" : "disabled")
					: "unsupported");
		}
	}

	if (sUsePAT)
		init_pat(cpu);

#ifdef __x86_64__
	// if RDTSCP or RDPID are available write cpu number in TSC_AUX
	if (x86_check_feature(IA32_FEATURE_AMD_EXT_RDTSCP, FEATURE_EXT_AMD)
		|| x86_check_feature(IA32_FEATURE_RDPID, FEATURE_7_ECX)) {
		x86_write_msr(IA32_MSR_TSC_AUX, cpu);
	}

	// make LFENCE a dispatch serializing instruction on AMD 64bit
	if (cpuEnt->arch.vendor == VENDOR_AMD) {
		uint32 family = cpuEnt->arch.family + cpuEnt->arch.extended_family;
		if (family >= 0x10 && family != 0x11) {
			uint64 value = x86_read_msr(MSR_F10H_DE_CFG);
			if ((value & DE_CFG_SERIALIZE_LFENCE) == 0)
				x86_write_msr(MSR_F10H_DE_CFG, value | DE_CFG_SERIALIZE_LFENCE);
		}
	}
#endif

	if (x86_check_feature(IA32_FEATURE_APERFMPERF, FEATURE_6_ECX)) {
		gCPU[cpu].arch.mperf_prev = x86_read_msr(IA32_MSR_MPERF);
		gCPU[cpu].arch.aperf_prev = x86_read_msr(IA32_MSR_APERF);
		gCPU[cpu].arch.frequency = 0;
		gCPU[cpu].arch.perf_timestamp = 0;
	}
	return __x86_patch_errata_percpu(cpu);
}


status_t
arch_cpu_init(kernel_args* args)
{
	if (args->ucode_data != NULL
		&& args->ucode_data_size > 0) {
		sUcodeData = args->ucode_data;
		sUcodeDataSize = args->ucode_data_size;
	} else {
		dprintf("CPU: no microcode provided\n");
	}

	// Initialize descriptor tables.
	x86_descriptors_init(args);

	return B_OK;
}


#ifdef __x86_64__
static void
enable_smap(void* dummy, int cpu)
{
	x86_write_cr4(x86_read_cr4() | IA32_CR4_SMAP);
}


static void
enable_smep(void* dummy, int cpu)
{
	x86_write_cr4(x86_read_cr4() | IA32_CR4_SMEP);
}


static void
enable_osxsave(void* dummy, int cpu)
{
	x86_write_cr4(x86_read_cr4() | IA32_CR4_OSXSAVE);
}


static void
enable_xsavemask(void* dummy, int cpu)
{
	xsetbv(0, gXsaveMask);
}
#endif


status_t
arch_cpu_init_post_vm(kernel_args* args)
{
	// allocate the area for the double fault stacks
	area_id stacks = create_area("double fault stacks",
		(void**)&sDoubleFaultStacks, B_EXACT_ADDRESS,
		kDoubleFaultStackSize * smp_get_num_cpus(),
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (stacks < B_OK)
		panic("failed to create double fault stacks area: %" B_PRId32, stacks);

	X86PagingStructures* kernelPagingStructures
		= static_cast<X86VMTranslationMap*>(
			VMAddressSpace::Kernel()->TranslationMap())->PagingStructures();

	// Set active translation map on each CPU.
	for (uint32 i = 0; i < args->num_cpus; i++) {
		gCPU[i].arch.active_paging_structures = kernelPagingStructures;
		kernelPagingStructures->AddReference();
	}

	if (!apic_available())
		x86_init_fpu();
	// else fpu gets set up in smp code

#ifdef __x86_64__
	// if available enable SMEP (Supervisor Memory Execution Protection)
	if (x86_check_feature(IA32_FEATURE_SMEP, FEATURE_7_EBX)) {
		if (!get_safemode_boolean(B_SAFEMODE_DISABLE_SMEP_SMAP, false)) {
			dprintf("enable SMEP\n");
			call_all_cpus_sync(&enable_smep, NULL);
		} else
			dprintf("SMEP disabled per safemode setting\n");
	}

	// if available enable SMAP (Supervisor Memory Access Protection)
	if (x86_check_feature(IA32_FEATURE_SMAP, FEATURE_7_EBX)) {
		if (!get_safemode_boolean(B_SAFEMODE_DISABLE_SMEP_SMAP, false)) {
			dprintf("enable SMAP\n");
			call_all_cpus_sync(&enable_smap, NULL);

			arch_altcodepatch_replace(ALTCODEPATCH_TAG_STAC, &_stac, 3);
			arch_altcodepatch_replace(ALTCODEPATCH_TAG_CLAC, &_clac, 3);
		} else
			dprintf("SMAP disabled per safemode setting\n");
	}

	// if available enable XSAVE (XSAVE and extended states)
	gHasXsave = x86_check_feature(IA32_FEATURE_EXT_XSAVE, FEATURE_EXT);
	if (gHasXsave) {
		gHasXsavec = x86_check_feature(IA32_FEATURE_XSAVEC,
			FEATURE_D_1_EAX);

		call_all_cpus_sync(&enable_osxsave, NULL);
		gXsaveMask = IA32_XCR0_X87 | IA32_XCR0_SSE;
		cpuid_info cpuid;
		get_current_cpuid(&cpuid, 0xd, 0);
		gXsaveMask |= (cpuid.regs.eax & IA32_XCR0_AVX);
		call_all_cpus_sync(&enable_xsavemask, NULL);
		get_current_cpuid(&cpuid, 0xd, 0);
		gFPUSaveLength = cpuid.regs.ebx;
		if (gFPUSaveLength > sizeof(((struct arch_thread *)0)->fpu_state))
			gFPUSaveLength = 832;

		arch_altcodepatch_replace(ALTCODEPATCH_TAG_XSAVE,
			gHasXsavec ? &_xsavec : &_xsave, 4);
		arch_altcodepatch_replace(ALTCODEPATCH_TAG_XRSTOR,
			&_xrstor, 4);

		dprintf("enable %s 0x%" B_PRIx64 " %" B_PRId64 "\n",
			gHasXsavec ? "XSAVEC" : "XSAVE", gXsaveMask, gFPUSaveLength);
	}
#endif

	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args* args)
{
	// initialize CPU module

	void* cookie = open_module_list("cpu");

	while (true) {
		char name[B_FILE_NAME_LENGTH];
		size_t nameLength = sizeof(name);

		if (read_next_module_name(cookie, name, &nameLength) != B_OK
			|| get_module(name, (module_info**)&sCpuModule) == B_OK)
			break;
	}

	close_module_list(cookie);

	// initialize MTRRs if available
	if (x86_count_mtrrs() > 0) {
		sCpuRendezvous = sCpuRendezvous2 = 0;
		call_all_cpus(&init_mtrrs, NULL);
	}

	size_t threadExitLen = (addr_t)x86_end_userspace_thread_exit
		- (addr_t)x86_userspace_thread_exit;
	addr_t threadExitPosition = fill_commpage_entry(
		COMMPAGE_ENTRY_X86_THREAD_EXIT, (const void*)x86_userspace_thread_exit,
		threadExitLen);

	// add the functions to the commpage image
	image_id image = get_commpage_image();

	elf_add_memory_image_symbol(image, "commpage_thread_exit",
		threadExitPosition, threadExitLen, B_SYMBOL_TYPE_TEXT);

	return B_OK;
}


void
arch_cpu_user_TLB_invalidate(void)
{
	x86_write_cr3(x86_read_cr3());
}


void
arch_cpu_global_TLB_invalidate(void)
{
	uint32 flags = x86_read_cr4();

	if (flags & IA32_CR4_GLOBAL_PAGES) {
		// disable and reenable the global pages to flush all TLBs regardless
		// of the global page bit
		x86_write_cr4(flags & ~IA32_CR4_GLOBAL_PAGES);
		x86_write_cr4(flags | IA32_CR4_GLOBAL_PAGES);
	} else {
		cpu_status state = disable_interrupts();
		arch_cpu_user_TLB_invalidate();
		restore_interrupts(state);
	}
}


void
arch_cpu_invalidate_TLB_range(addr_t start, addr_t end)
{
	int32 num_pages = end / B_PAGE_SIZE - start / B_PAGE_SIZE;
	while (num_pages-- >= 0) {
		invalidate_TLB(start);
		start += B_PAGE_SIZE;
	}
}


void
arch_cpu_invalidate_TLB_list(addr_t pages[], int num_pages)
{
	int i;
	for (i = 0; i < num_pages; i++) {
		invalidate_TLB(pages[i]);
	}
}


status_t
arch_cpu_shutdown(bool rebootSystem)
{
	if (acpi_shutdown(rebootSystem) == B_OK)
		return B_OK;

	if (!rebootSystem) {
#ifndef __x86_64__
		return apm_shutdown();
#else
		return B_NOT_SUPPORTED;
#endif
	}

	cpu_status state = disable_interrupts();

	// try to reset the system using the keyboard controller
	out8(0xfe, 0x64);

	// Give some time to the controller to do its job (0.5s)
	snooze(500000);

	// if that didn't help, try it this way
	x86_reboot();

	restore_interrupts(state);
	return B_ERROR;
}


void
arch_cpu_sync_icache(void* address, size_t length)
{
	// instruction cache is always consistent on x86
}

