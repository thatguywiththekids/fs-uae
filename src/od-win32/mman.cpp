// Implement mprotect() for Win32
// Copyright (C) 2000, Brian King
// GNU Public License

#include <float.h>

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "uae/memory.h"
#include "uae/mman.h"
#include "uae/vm.h"
#include "autoconf.h"
#include "gfxboard.h"
#include "cpuboard.h"
#include "rommgr.h"
#include "newcpu.h"
#ifdef WINUAE
#include "win32.h"
#endif

#ifdef FSUAE // NL
#include "uae/fs.h"
#endif

#if defined(NATMEM_OFFSET)

uae_u32 max_z3fastmem;

/* BARRIER is used in case Amiga memory is access across memory banks,
 * for example move.l $1fffffff,d0 when $10000000-$1fffffff is mapped and
 * $20000000+ is not mapped.
 * Note: BARRIER will probably effectively be rounded up the host memory
 * page size.
 */
#define BARRIER 32

#define MAXZ3MEM32 0x7F000000
#define MAXZ3MEM64 0xF0000000

static struct uae_shmid_ds shmids[MAX_SHMID];
uae_u8 *natmem_reserved, *natmem_offset, *natmem_offset_end;
uae_u32 natmem_reserved_size;
static uae_u8 *p96mem_offset;
static int p96mem_size;
static uae_u32 p96base_offset;
static SYSTEM_INFO si;
#ifdef FSUAE
static uint32_t maxmem;
/* FIXME: check if signed int is a bit small */
/* FIXME: check where maxmem is set */
#else
int maxmem;
#endif
bool jit_direct_compatible_memory;

static uae_u8 *virtualallocwithlock (LPVOID addr, SIZE_T size, DWORD allocationtype, DWORD protect)
{
	uae_u8 *p = (uae_u8*)VirtualAlloc (addr, size, allocationtype, protect);
	return p;
}
static void virtualfreewithlock (LPVOID addr, SIZE_T size, DWORD freetype)
{
	VirtualFree(addr, size, freetype);
}

static uae_u32 lowmem (void)
{
	uae_u32 change = 0;
#if 0
	struct rtgboardconfig *rbc = &changed_prefs.rtgboards[0];
	struct rtgboardconfig *crbc = &currprefs.rtgboards[0];

	if (currprefs.z3fastmem_size + currprefs.z3fastmem2_size + currprefs.z3chipmem_size >= 8 * 1024 * 1024) {
		if (currprefs.z3fastmem2_size) {
			change = currprefs.z3fastmem2_size;
			currprefs.z3fastmem2_size = 0;
		} else if (currprefs.z3chipmem_size) {
			if (currprefs.z3chipmem_size <= 16 * 1024 * 1024) {
				change = currprefs.z3chipmem_size;
				currprefs.z3chipmem_size = 0;
			} else {
				change = currprefs.z3chipmem_size / 2;
				currprefs.z3chipmem_size /= 2;
			}
		} else {
			change = currprefs.z3fastmem_size - currprefs.z3fastmem_size / 4;
			currprefs.z3fastmem2_size = changed_prefs.z3fastmem2_size = currprefs.z3fastmem_size / 4;
			currprefs.z3fastmem_size /= 2;
			changed_prefs.z3fastmem_size = currprefs.z3fastmem_size;
		}
	} else if (crbc->rtgmem_type == GFXBOARD_UAE_Z3 && crbc->rtgmem_size >= 1 * 1024 * 1024) {
		change = crbc->rtgmem_size - crbc->rtgmem_size / 2;
		crbc->rtgmem_size /= 2;
		rbc->rtgmem_size = crbc->rtgmem_size;
	}
	if (currprefs.z3fastmem2_size < 128 * 1024 * 1024)
		currprefs.z3fastmem2_size = changed_prefs.z3fastmem2_size = 0;
#endif
	return change;
}

#ifdef FSUAE
#else
int mman_GetWriteWatch (PVOID lpBaseAddress, SIZE_T dwRegionSize, PVOID *lpAddresses, PULONG_PTR lpdwCount, PULONG lpdwGranularity)
{
	return GetWriteWatch (WRITE_WATCH_FLAG_RESET, lpBaseAddress, dwRegionSize, lpAddresses, lpdwCount, lpdwGranularity);
}
void mman_ResetWatch (PVOID lpBaseAddress, SIZE_T dwRegionSize)
{
	if (ResetWriteWatch (lpBaseAddress, dwRegionSize))
		write_log (_T("ResetWriteWatch() failed, %d\n"), GetLastError ());
}
#endif

static uae_u64 size64;
#ifdef _WIN32
typedef BOOL (CALLBACK* GLOBALMEMORYSTATUSEX)(LPMEMORYSTATUSEX);
#endif

static void clear_shm (void)
{
	shm_start = NULL;
	for (int i = 0; i < MAX_SHMID; i++) {
		memset (&shmids[i], 0, sizeof(struct uae_shmid_ds));
		shmids[i].key = -1;
	}
}

bool preinit_shm (void)
{
#ifdef FSUAE
	write_log("preinit_shm\n");
#endif
	uae_u64 total64;
	uae_u64 totalphys64;
#ifdef _WIN32
	MEMORYSTATUS memstats;
	GLOBALMEMORYSTATUSEX pGlobalMemoryStatusEx;
	MEMORYSTATUSEX memstatsex;
#endif
	uae_u32 max_allowed_mman;

	if (natmem_reserved)
#ifdef _WIN32
		VirtualFree (natmem_reserved, 0, MEM_RELEASE);
#else
#ifdef FSUAE
		free (natmem_reserved);
#endif
#endif
	natmem_reserved = NULL;
	natmem_offset = NULL;
#if 0
	if (p96mem_offset) {
#ifdef _WIN32
		VirtualFree (p96mem_offset, 0, MEM_RELEASE);
#else
#ifdef FSUAE
		/* Don't free p96mem_offset - it is freed as part of natmem_offset */
		// free (p96mem_offset);
#endif
#endif
	}
	p96mem_offset = NULL;
#endif
	GetSystemInfo (&si);
#ifdef FSUAE
	max_allowed_mman = 2048;
#else
	max_allowed_mman = 512 + 256;
#endif
#if 1
	if (os_64bit) {
//#ifdef WIN64
//		max_allowed_mman = 3072;
//#else
		max_allowed_mman = 2048;
//#endif
	}
#endif
	if (maxmem > max_allowed_mman)
		max_allowed_mman = maxmem;

#ifdef _WIN32
	memstats.dwLength = sizeof(memstats);
	GlobalMemoryStatus(&memstats);
	totalphys64 = memstats.dwTotalPhys;
	total64 = (uae_u64)memstats.dwAvailPageFile + (uae_u64)memstats.dwTotalPhys;
#ifdef FSUAE
	pGlobalMemoryStatusEx = GlobalMemoryStatusEx;
#else
	pGlobalMemoryStatusEx = (GLOBALMEMORYSTATUSEX)GetProcAddress (GetModuleHandle (_T("kernel32.dll")), "GlobalMemoryStatusEx");
#endif
	if (pGlobalMemoryStatusEx) {
		memstatsex.dwLength = sizeof (MEMORYSTATUSEX);
		if (pGlobalMemoryStatusEx(&memstatsex)) {
			totalphys64 = memstatsex.ullTotalPhys;
			total64 = memstatsex.ullAvailPageFile + memstatsex.ullTotalPhys;
		}
	}
#else
#ifdef FSUAE
#ifdef __APPLE__
	int mib[2];
	size_t len;

	mib[0] = CTL_HW;
	// FIXME: check 64-bit compat
	mib[1] = HW_MEMSIZE; /* gives a 64 bit int */
	len = sizeof(totalphys64);
	sysctl(mib, 2, &totalphys64, &len, NULL, 0);
	total64 = (uae_u64) totalphys64;
#else
	totalphys64 = sysconf (_SC_PHYS_PAGES) * (uae_u64)getpagesize();
	total64 = (uae_u64)sysconf (_SC_PHYS_PAGES) * (uae_u64)getpagesize();
#endif
#endif
#endif
	size64 = total64;
	if (os_64bit) {
		if (size64 > MAXZ3MEM64)
			size64 = MAXZ3MEM64;
	} else {
		if (size64 > MAXZ3MEM32)
			size64 = MAXZ3MEM32;
	}
#ifdef FSUAE
	/* FIXME: check */
	if (maxmem == 0) {
#else
	if (maxmem < 0) {
#endif
		size64 = MAXZ3MEM64;
		if (!os_64bit) {
			if (totalphys64 < 1536 * 1024 * 1024)
				max_allowed_mman = 256;
			if (max_allowed_mman < 256)
				max_allowed_mman = 256;
		}
	} else if (maxmem > 0) {
		size64 = maxmem * 1024 * 1024;
	}
	if (size64 < 8 * 1024 * 1024)
		size64 = 8 * 1024 * 1024;
	if (max_allowed_mman * 1024 * 1024 > size64)
		max_allowed_mman = size64 / (1024 * 1024);

	uae_u32 natmem_size = (max_allowed_mman + 1) * 1024 * 1024;
	if (natmem_size < 17 * 1024 * 1024)
		natmem_size = 17 * 1024 * 1024;

	//natmem_size = 257 * 1024 * 1024;

	if (natmem_size > 0x80000000) {
		natmem_size = 0x80000000;
	}

	write_log (_T("NATMEM: Total physical RAM %llu MB, all RAM %llu MB\n"),
				  totalphys64 >> 20, total64 >> 20);
	write_log(_T("NATMEM: Attempting to reserve: %u MB\n"), natmem_size >> 20);

	int vm_flags = UAE_VM_32BIT | UAE_VM_WRITE_WATCH;
#ifdef FSUAE
	write_log("NATMEM: jit compiler %d\n", g_fs_uae_jit_compiler);
	if (!g_fs_uae_jit_compiler) {
		/* Not using the JIT compiler, so we do not need "32-bit memory". */
		vm_flags &= ~UAE_VM_32BIT;
	}
#endif
	natmem_reserved = (uae_u8 *) uae_vm_reserve(natmem_size, vm_flags);

	if (!natmem_reserved) {
		if (natmem_size <= 768 * 1024 * 1024) {
			uae_u32 p = 0x78000000 - natmem_size;
			for (;;) {
#ifdef FSUAE
				natmem_reserved = (uae_u8 *) uae_vm_reserve(natmem_size, vm_flags);
#else
				natmem_reserved = (uae_u8*) VirtualAlloc((void*)(intptr_t)p, natmem_size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
#endif
				if (natmem_reserved)
					break;
				p -= 128 * 1024 * 1024;
				if (p <= 128 * 1024 * 1024)
					break;
			}
		}
	}
	if (!natmem_reserved) {
		DWORD vaflags = MEM_RESERVE | MEM_WRITE_WATCH;
#ifdef _WIN32
#ifdef FSUAE
		OSVERSIONINFO osVersion;
		osVersion.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
		bool os_vista = (osVersion.dwMajorVersion == 6 &&
						 osVersion.dwMinorVersion == 0);
#endif
#ifndef _WIN64
		if (!os_vista)
			vaflags |= MEM_TOP_DOWN;
#endif
#endif
		for (;;) {
#ifdef FSUAE
			natmem_reserved = (uae_u8 *) uae_vm_reserve(natmem_size, vm_flags);
#else
			natmem_reserved = (uae_u8*)VirtualAlloc (NULL, natmem_size, vaflags, PAGE_READWRITE);
#endif
			if (natmem_reserved)
				break;
			natmem_size -= 64 * 1024 * 1024;
			if (!natmem_size) {
				write_log (_T("Can't allocate 257M of virtual address space!?\n"));
				natmem_size = 17 * 1024 * 1024;
#ifdef FSUAE
				natmem_reserved = (uae_u8 *) uae_vm_reserve(natmem_size, vm_flags);
#else
				natmem_reserved = (uae_u8*)VirtualAlloc (NULL, natmem_size, vaflags, PAGE_READWRITE);
#endif
				if (!natmem_size) {
					write_log (_T("Can't allocate 17M of virtual address space!? Something is seriously wrong\n"));
					return false;
				}
				break;
			}
		}
	}
	natmem_reserved_size = natmem_size;
	natmem_offset = natmem_reserved;
	if (natmem_size <= 257 * 1024 * 1024) {
		max_z3fastmem = 0;
	} else {
		max_z3fastmem = natmem_size;
	}
	write_log (_T("NATMEM: Reserved %p-%p (0x%08x %dM)\n"),
			   natmem_reserved, (uae_u8 *) natmem_reserved + natmem_reserved_size,
			   natmem_reserved_size, natmem_reserved_size / (1024 * 1024));

	clear_shm ();

//	write_log (_T("Max Z3FastRAM %dM. Total physical RAM %uM\n"), max_z3fastmem >> 20, totalphys64 >> 20);

	canbang = 1;
	return true;
}

static void resetmem (bool decommit)
{
	int i;

	if (!shm_start)
		return;
	for (i = 0; i < MAX_SHMID; i++) {
		struct uae_shmid_ds *s = &shmids[i];
		int size = s->size;
		uae_u8 *shmaddr;
		uae_u8 *result;

		if (!s->attached)
			continue;
		if (!s->natmembase)
			continue;
		if (s->fake)
			continue;
		if (!decommit && ((uae_u8*)s->attached - (uae_u8*)s->natmembase) >= 0x10000000)
			continue;
		shmaddr = natmem_offset + ((uae_u8*)s->attached - (uae_u8*)s->natmembase);
		if (decommit) {
			VirtualFree (shmaddr, size, MEM_DECOMMIT);
		} else {
			result = virtualallocwithlock (shmaddr, size, decommit ? MEM_DECOMMIT : MEM_COMMIT, PAGE_READWRITE);
			if (result != shmaddr)
				write_log (_T("NATMEM: realloc(%p-%p,%d,%d,%s) failed, err=%d\n"), shmaddr, shmaddr + size, size, s->mode, s->name, GetLastError ());
			else
				write_log (_T("NATMEM: rellocated(%p-%p,%d,%s)\n"), shmaddr, shmaddr + size, size, s->name);
		}
	}
}

static uae_u8 *va (uae_u32 offset, uae_u32 len, DWORD alloc, DWORD protect)
{
	uae_u8 *addr;

	addr = (uae_u8*)VirtualAlloc (natmem_offset + offset, len, alloc, protect);
	if (addr) {
		write_log (_T("VA(%p - %p, %4uM, %s)\n"),
			natmem_offset + offset, natmem_offset + offset + len, len >> 20, (alloc & MEM_WRITE_WATCH) ? _T("WATCH") : _T("RESERVED"));
		return addr;
	}
	write_log (_T("VA(%p - %p, %4uM, %s) failed %d\n"),
		natmem_offset + offset, natmem_offset + offset + len, len >> 20, (alloc & MEM_WRITE_WATCH) ? _T("WATCH") : _T("RESERVED"), GetLastError ());
	return NULL;
}

static int doinit_shm (void)
{
	uae_u32 totalsize;
	uae_u32 startbarrier, align;
	uae_u32 z3rtgmem_size;
	struct rtgboardconfig *rbc = &changed_prefs.rtgboards[0];
	struct rtgboardconfig *crbc = &currprefs.rtgboards[0];

	changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = 0;
	set_expamem_z3_hack_mode(0);
	expansion_scan_autoconfig(&currprefs, true);

	canbang = 1;
	natmem_offset = natmem_reserved;

	align = 16 * 1024 * 1024 - 1;
	totalsize = 0x01000000;
	startbarrier = changed_prefs.mbresmem_high_size >= 128 * 1024 * 1024 ? (changed_prefs.mbresmem_high_size - 128 * 1024 * 1024) + 16 * 1024 * 1024 : 0;

	z3rtgmem_size = gfxboard_get_configtype(rbc) == 3 ? rbc->rtgmem_size : 0;

	if (changed_prefs.cpu_model >= 68020)
		totalsize = 0x10000000;
	totalsize += (changed_prefs.z3chipmem_size + align) & ~align;

	if (changed_prefs.z3_mapping_mode == Z3MAPPING_UAE || cpuboard_memorytype(&changed_prefs) == BOARD_MEMORY_BLIZZARD_12xx ||
		(expamem_z3_pointer_real + 16 * si.dwPageSize >= natmem_reserved_size && changed_prefs.z3_mapping_mode == Z3MAPPING_AUTO)) {
		changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = Z3BASE_UAE;
		if (changed_prefs.z3_mapping_mode == Z3MAPPING_AUTO)
			write_log(_T("MMAN: Selected UAE Z3 mapping mode\n"));
		set_expamem_z3_hack_mode(Z3MAPPING_UAE);
		if (expamem_z3_pointer_uae > totalsize) {
			totalsize = expamem_z3_pointer_uae;
			startbarrier = 0;
		}
	} else {
		changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = Z3BASE_REAL;
		set_expamem_z3_hack_mode(Z3MAPPING_REAL);
		if (expamem_z3_pointer_real > totalsize) {
			totalsize = expamem_z3_pointer_real;
			if (totalsize + 16 * si.dwPageSize >= natmem_reserved_size && expamem_z3_pointer_uae < totalsize) {
				totalsize = expamem_z3_pointer_uae;
			}
			startbarrier = 0;
		}
	}
	if (totalsize < expamem_highmem_pointer)
		totalsize = expamem_highmem_pointer;

	if (totalsize > size64 || totalsize + 16 * si.dwPageSize >= natmem_reserved_size) {
		write_log(_T("NATMEM: Not enough memory!\n"));
		return -1;
	}
#ifdef FSUAE
	write_log("NATMEM: size            0x%08x\n", size);
	write_log("NATMEM: z3size        + 0x%08x\n", z3size);
	write_log("NATMEM: z3rtgmem_size + 0x%08x\n", z3rtgmem_size);
	write_log("NATMEM: othersize     + 0x%08x\n", othersize);
	write_log("NATMEM: totalsize     = 0x%08x\n", totalsize);
#endif

	jit_direct_compatible_memory = true;

	expansion_scan_autoconfig(&currprefs, true);
#if 0
	z3offset = 0;
	if (changed_prefs.z3_mapping_mode != Z3MAPPING_UAE && cpuboard_memorytype(&changed_prefs) != BOARD_MEMORY_BLIZZARD_12xx) {
		if (1 && natmem_reserved_size > 0x40000000 && natmem_reserved_size - 0x40000000 >= (totalsize - 0x10000000 - ((changed_prefs.z3chipmem_size + align) & ~align)) && changed_prefs.z3chipmem_size <= 512 * 1024 * 1024) {
			changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = Z3BASE_REAL;
			z3offset += Z3BASE_REAL - Z3BASE_UAE - ((changed_prefs.z3chipmem_size + align) & ~align);
			z3offset += cpuboards[currprefs.cpuboard_type].subtypes[currprefs.cpuboard_subtype].z3extra;
			set_expamem_z3_hack_override(true);
			startbarrier = 0;
			write_log(_T("Z3 REAL mapping. JIT direct compatible.\n"));
			jit_direct_compatible_memory = true;
		} else if (changed_prefs.z3_mapping_mode == Z3MAPPING_AUTO && currprefs.cachesize) {
			changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = Z3BASE_UAE;
			jit_direct_compatible_memory = true;
			write_log(_T("Z3 UAE mapping (auto).\n"));
		} else {
			changed_prefs.z3autoconfig_start = currprefs.z3autoconfig_start = Z3BASE_REAL;
			write_log(_T("Z3 REAL mapping. Not JIT direct compatible.\n"));
			jit_direct_compatible_memory = false;
		}
	} else {
		currprefs.z3autoconfig_start = changed_prefs.z3autoconfig_start = Z3BASE_UAE;
		jit_direct_compatible_memory = true;
		write_log(_T("Z3 UAE mapping.\n"));
	}
#endif
#ifdef FSUAE
	write_log("NATMEM: JIT direct compatible: %d\n", jit_direct_compatible_memory);
#endif

#if 0
	p96mem_offset = NULL;
	p96mem_size = z3rtgmem_size;
	p96base_offset = 0;
	uae_u32 z3rtgallocsize = 0;
	if (rbc->rtgmem_size && gfxboard_get_configtype(rbc) == 3) {
		z3rtgallocsize = gfxboard_get_autoconfig_size(rbc) < 0 ? rbc->rtgmem_size : gfxboard_get_autoconfig_size(rbc);
		if (changed_prefs.z3autoconfig_start == Z3BASE_UAE)
			p96base_offset = natmemsize + startbarrier + z3offset;
		else
			p96base_offset = expansion_startaddress(natmemsize + startbarrier + z3offset, z3rtgallocsize);
	} else if (rbc->rtgmem_size && gfxboard_get_configtype(rbc) == 2) {
		p96base_offset = getz2rtgaddr (rbc);
	} else if (rbc->rtgmem_size && gfxboard_get_configtype(rbc) == 1) {
		p96base_offset = 0xa80000;
	}
	if (p96base_offset) {
		if (jit_direct_compatible_memory) {
			p96mem_offset = natmem_offset + p96base_offset;
		} else {
			if (changed_prefs.cachesize) {
				crbc->rtgmem_size = rbc->rtgmem_size = 0;
				crbc->rtgmem_type = rbc->rtgmem_type = 0;
				error_log(_T("RTG board is not anymore supported when JIT is enabled and RTG VRAM is located outside of NATMEM (Real Z3 mode under 32-bit Windows)."));
			} else {
				// calculate Z3 alignment (argh, I thought only Z2 needed this..)
				uae_u32 addr = Z3BASE_REAL;
				int z3off = cpuboards[currprefs.cpuboard_type].subtypes[currprefs.cpuboard_subtype].z3extra;
				if (z3off) {
					addr = expansion_startaddress(addr, z3off);
					addr += z3off;
				}
				addr = expansion_startaddress(addr, changed_prefs.z3fastmem_size);
				addr += changed_prefs.z3fastmem_size;
				addr = expansion_startaddress(addr, changed_prefs.z3fastmem2_size);
				addr += changed_prefs.z3fastmem2_size;
				addr = expansion_startaddress(addr, z3rtgallocsize);
				if (gfxboard_get_configtype(rbc) == 3) {
					p96base_offset = addr;
					write_log("NATMEM: p96base_offset = 0x%x\n", p96base_offset);
					// adjust p96mem_offset to beginning of natmem
					// by subtracting start of original p96mem_offset from natmem_offset
					if (p96base_offset >= 0x10000000) {
#ifdef FSUAE
						write_log("NATMEM: natmem_offset = %p - 0x%x\n", natmem_reserved, p96base_offset);
#endif
						natmem_offset = natmem_reserved - p96base_offset;
						p96mem_offset = natmem_offset + p96base_offset;
					}
				}
			}
		}
	}
#endif

	if (!natmem_offset) {
		write_log (_T("NATMEM: No special area could be allocated! err=%d\n"), GetLastError ());
	} else {
		write_log(_T("NATMEM: Our special area: %p-%p (0x%08x %dM)\n"),
			natmem_offset, (uae_u8*)natmem_offset + totalsize,
			totalsize, totalsize / (1024 * 1024));
#if 0
		if (rbc->rtgmem_size)
			write_log (_T("NATMEM: P96 special area: %p-%p (0x%08x %dM)\n"),
				p96mem_offset, (uae_u8*)p96mem_offset + rbc->rtgmem_size,
				rbc->rtgmem_size, rbc->rtgmem_size >> 20);
#endif
		canbang = jit_direct_compatible_memory ? 1 : 0;
		if (p96mem_size)
			natmem_offset_end = p96mem_offset + p96mem_size;
		else
			natmem_offset_end = natmem_offset + totalsize;
	}

	return canbang;
}

static uae_u32 oz3fastmem_size[MAX_RAM_BOARDS];
static uae_u32 oz3chipmem_size;
static uae_u32 ortgmem_size[MAX_RTG_BOARDS];
static int ortgmem_type[MAX_RTG_BOARDS];

bool init_shm (void)
{
#ifdef FSUAE
	write_log("init_shm\n");
#endif
	bool changed = false;

	for (int i = 0; i < MAX_RAM_BOARDS; i++) {
		if (oz3fastmem_size[i] != changed_prefs.z3fastmem[i].size)
			changed = true;
		if (ortgmem_size[i] != changed_prefs.rtgboards[i].rtgmem_size)
			changed = true;
		if (ortgmem_type[i] != changed_prefs.rtgboards[i].rtgmem_type)
			changed = true;
	}
	if (!changed && oz3chipmem_size == changed_prefs.z3chipmem_size)
		return true;

	for (int i = 0; i < MAX_RAM_BOARDS;i++) {
		oz3fastmem_size[i] = changed_prefs.z3fastmem[i].size;
		ortgmem_size[i] = changed_prefs.rtgboards[i].rtgmem_size;
		ortgmem_type[i] = changed_prefs.rtgboards[i].rtgmem_type;
	}
	oz3chipmem_size = changed_prefs.z3chipmem_size;

	if (doinit_shm () < 0)
		return false;

	resetmem (false);
	clear_shm ();

	memory_hardreset (2);
	return true;
}

void free_shm (void)
{
	resetmem (true);
	clear_shm ();
	for (int i = 0; i < MAX_RAM_BOARDS; i++) {
		ortgmem_type[i] = -1;
	}
}

void mapped_free (addrbank *ab)
{
	shmpiece *x = shm_start;
	bool rtgmem = (ab->flags & ABFLAG_RTG) != 0;

	if (ab->baseaddr == NULL)
		return;

	if (ab->flags & ABFLAG_INDIRECT) {
		while(x) {
			if (ab->baseaddr == x->native_address) {
				int shmid = x->id;
				shmids[shmid].key = -1;
				shmids[shmid].name[0] = '\0';
				shmids[shmid].size = 0;
				shmids[shmid].attached = 0;
				shmids[shmid].mode = 0;
				shmids[shmid].natmembase = 0;
				if (!(ab->flags & ABFLAG_NOALLOC)) {
					xfree(ab->baseaddr);
					ab->baseaddr = NULL;
				}
			}
			x = x->next;
		}
		ab->baseaddr = NULL;
		ab->flags &= ~ABFLAG_DIRECTMAP;
		write_log(_T("mapped_free indirect %s\n"), ab->name);
		return;
	}

	if (!(ab->flags & ABFLAG_DIRECTMAP)) {
		if (!(ab->flags & ABFLAG_NOALLOC)) {
			xfree(ab->baseaddr);
		}
		ab->baseaddr = NULL;
		write_log(_T("mapped_free nondirect %s\n"), ab->name);
		return;
	}

	while(x) {
		if(ab->baseaddr == x->native_address)
			uae_shmdt (x->native_address);
		x = x->next;
	}
	x = shm_start;
	while(x) {
		struct uae_shmid_ds blah;
		if (ab->baseaddr == x->native_address) {
			if (uae_shmctl (x->id, UAE_IPC_STAT, &blah) == 0)
				uae_shmctl (x->id, UAE_IPC_RMID, &blah);
		}
		x = x->next;
	}
	ab->baseaddr = NULL;
	write_log(_T("mapped_free direct %s\n"), ab->name);
}

static uae_key_t get_next_shmkey (void)
{
	uae_key_t result = -1;
	int i;
	for (i = 0; i < MAX_SHMID; i++) {
		if (shmids[i].key == -1) {
			shmids[i].key = i;
			result = i;
			break;
		}
	}
	return result;
}

STATIC_INLINE uae_key_t find_shmkey (uae_key_t key)
{
	int result = -1;
	if(shmids[key].key == key) {
		result = key;
	}
	return result;
}

void *uae_shmat (addrbank *ab, int shmid, void *shmaddr, int shmflg)
{
#ifdef FSUAE
	write_log("uae_shmat shmid %d shmaddr %p, shmflg %d natmem_offset = %p\n",
			shmid, shmaddr, shmflg, natmem_offset);
#endif
	void *result = (void *)-1;
	bool got = false, readonly = false, maprom = false;
	int p96special = FALSE;

#ifdef NATMEM_OFFSET
	unsigned int size = shmids[shmid].size;
	unsigned int readonlysize = size;
	struct rtgboardconfig *rbc = &currprefs.rtgboards[0];

	if (shmids[shmid].attached)
		return shmids[shmid].attached;

	if (ab->flags & ABFLAG_INDIRECT) {
		shmids[shmid].attached = ab->baseaddr;
		shmids[shmid].fake = true;
		return shmids[shmid].attached;
	}

	if ((uae_u8*)shmaddr < natmem_offset) {
		if(!_tcscmp (shmids[shmid].name, _T("chip"))) {
			shmaddr=natmem_offset;
			got = true;
			if (!expansion_get_autoconfig_by_address(&currprefs, 0x00200000) || currprefs.chipmem_size < 2 * 1024 * 1024)
				size += BARRIER;
		} else if(!_tcscmp (shmids[shmid].name, _T("kick"))) {
			shmaddr=natmem_offset + 0xf80000;
			got = true;
			size += BARRIER;
			readonly = true;
			maprom = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("rom_a8"))) {
			shmaddr=natmem_offset + 0xa80000;
			got = true;
			readonly = true;
			maprom = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("rom_e0"))) {
			shmaddr=natmem_offset + 0xe00000;
			got = true;
			readonly = true;
			maprom = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("rom_f0"))) {
			shmaddr=natmem_offset + 0xf00000;
			got = true;
			readonly = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("rom_f0_ppc"))) {
			// this is flash and also contains IO
			shmaddr=natmem_offset + 0xf00000;
			got = true;
			readonly = false;
		} else if (!_tcscmp(shmids[shmid].name, _T("rtarea"))) {
			shmaddr = natmem_offset + rtarea_base;
			got = true;
			readonly = true;
			readonlysize = RTAREA_TRAPS;
		} else if(!_tcscmp (shmids[shmid].name, _T("ramsey_low"))) {
			shmaddr=natmem_offset + a3000lmem_bank.start;
			if (!a3000hmem_bank.start)
				size += BARRIER;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("csmk1_maprom"))) {
			shmaddr = natmem_offset + 0x07f80000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("25bitram"))) {
			shmaddr = natmem_offset + 0x01000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("ramsey_high"))) {
			shmaddr = natmem_offset + 0x08000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("dkb"))) {
			shmaddr = natmem_offset + 0x10000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("fusionforty"))) {
			shmaddr = natmem_offset + 0x11000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("blizzard_40"))) {
			shmaddr = natmem_offset + 0x40000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("blizzard_48"))) {
			shmaddr = natmem_offset + 0x48000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("blizzard_68"))) {
			shmaddr = natmem_offset + 0x68000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("blizzard_70"))) {
			shmaddr = natmem_offset + 0x70000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("cyberstorm"))) {
			shmaddr = natmem_offset + 0x0c000000;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("*"))) {
			shmaddr = natmem_offset + ab->start;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("cyberstormmaprom"))) {
			shmaddr = natmem_offset + 0xfff00000;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("bogo"))) {
			shmaddr=natmem_offset+0x00C00000;
			got = true;
			if (currprefs.bogomem_size <= 0x100000)
				size += BARRIER;
		} else if(!_tcscmp (shmids[shmid].name, _T("custmem1"))) {
			shmaddr=natmem_offset + currprefs.custom_memory_addrs[0];
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("custmem2"))) {
			shmaddr=natmem_offset + currprefs.custom_memory_addrs[1];
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("hrtmem"))) {
			shmaddr=natmem_offset + 0x00a10000;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("arhrtmon"))) {
			shmaddr=natmem_offset + 0x00800000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("xpower_e2"))) {
			shmaddr=natmem_offset + 0x00e20000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("xpower_f2"))) {
			shmaddr=natmem_offset + 0x00f20000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("nordic_f0"))) {
			shmaddr=natmem_offset + 0x00f00000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("nordic_f4"))) {
			shmaddr=natmem_offset + 0x00f40000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("nordic_f6"))) {
			shmaddr=natmem_offset + 0x00f60000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp(shmids[shmid].name, _T("superiv_b0"))) {
			shmaddr=natmem_offset + 0x00b00000;
			size += BARRIER;
			got = true;
		} else if(!_tcscmp (shmids[shmid].name, _T("superiv_d0"))) {
			shmaddr=natmem_offset + 0x00d00000;
			size += BARRIER;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("superiv_e0"))) {
			shmaddr = natmem_offset + 0x00e00000;
			size += BARRIER;
			got = true;
		} else if (!_tcscmp(shmids[shmid].name, _T("ram_a8"))) {
			shmaddr = natmem_offset + 0x00a80000;
			size += BARRIER;
			got = true;
		}
	}
#endif

	uintptr_t natmem_end = (uintptr_t) natmem_reserved + natmem_reserved_size;
	if ((uintptr_t) shmaddr + size > natmem_end) {
		/* We cannot add a barrier beyond the end of the reserved memory. */
		assert((uintptr_t) shmaddr + size - natmem_end == BARRIER);
		write_log(_T("NATMEM: Removing barrier (%d bytes) beyond reserved memory\n"), BARRIER);
		size -= BARRIER;
	}

	if (shmids[shmid].key == shmid && shmids[shmid].size) {
		DWORD protect = readonly ? PAGE_READONLY : PAGE_READWRITE;
		shmids[shmid].mode = protect;
		shmids[shmid].rosize = readonlysize;
		shmids[shmid].natmembase = natmem_offset;
		shmids[shmid].maprom = maprom ? 1 : 0;
		if (shmaddr)
			virtualfreewithlock (shmaddr, size, MEM_DECOMMIT);
		result = virtualallocwithlock (shmaddr, size, MEM_COMMIT, PAGE_READWRITE);
		if (result == NULL)
			virtualfreewithlock (shmaddr, 0, MEM_DECOMMIT);
		result = virtualallocwithlock (shmaddr, size, MEM_COMMIT, PAGE_READWRITE);
		if (result == NULL) {
			result = (void*)-1;
			error_log (_T("Memory %s (%s) failed to allocate %p: VA %08X - %08X %x (%dk). Error %d."),
				shmids[shmid].name, ab ? ab->name : _T("?"), shmaddr,
				(uae_u8*)shmaddr - natmem_offset, (uae_u8*)shmaddr - natmem_offset + size,
				size, size >> 10, GetLastError ());
		} else {
			shmids[shmid].attached = result;
			write_log (_T("%p: VA %08lX - %08lX %x (%dk) ok (%p)%s\n"),
				shmaddr, (uae_u8*)shmaddr - natmem_offset, (uae_u8*)shmaddr - natmem_offset + size,
				size, size >> 10, shmaddr, p96special ? _T(" P96") : _T(""));
		}
	}
	return result;
}

void unprotect_maprom (void)
{
	bool protect = false;
	for (int i = 0; i < MAX_SHMID; i++) {
		struct uae_shmid_ds *shm = &shmids[i];
		if (shm->mode != PAGE_READONLY)
			continue;
		if (!shm->attached || !shm->rosize)
			continue;
		if (shm->maprom <= 0)
			continue;
		shm->maprom = -1;
		DWORD old;
		if (!VirtualProtect (shm->attached, shm->rosize, protect ? PAGE_READONLY : PAGE_READWRITE, &old)) {
			write_log (_T("unprotect_maprom VP %08lX - %08lX %x (%dk) failed %d\n"),
				(uae_u8*)shm->attached - natmem_offset, (uae_u8*)shm->attached - natmem_offset + shm->size,
				shm->size, shm->size >> 10, GetLastError ());
		}
	}
}

void protect_roms (bool protect)
{
	if (protect) {
		// protect only if JIT enabled, always allow unprotect
		if (!currprefs.cachesize || currprefs.comptrustbyte || currprefs.comptrustword || currprefs.comptrustlong)
			return;
	}
	for (int i = 0; i < MAX_SHMID; i++) {
		struct uae_shmid_ds *shm = &shmids[i];
		if (shm->mode != PAGE_READONLY)
			continue;
		if (!shm->attached || !shm->rosize)
			continue;
		if (shm->maprom < 0 && protect)
			continue;
		DWORD old;
		if (!VirtualProtect (shm->attached, shm->rosize, protect ? PAGE_READONLY : PAGE_READWRITE, &old)) {
			write_log (_T("protect_roms VP %08lX - %08lX %x (%dk) failed %d\n"),
				(uae_u8*)shm->attached - natmem_offset, (uae_u8*)shm->attached - natmem_offset + shm->rosize,
				shm->rosize, shm->rosize >> 10, GetLastError ());
		} else {
			write_log(_T("ROM VP %08lX - %08lX %x (%dk) %s\n"),
				(uae_u8*)shm->attached - natmem_offset, (uae_u8*)shm->attached - natmem_offset + shm->rosize,
				shm->rosize, shm->rosize >> 10, protect ? _T("WPROT") : _T("UNPROT"));
		}
	}
}

int uae_shmdt (const void *shmaddr)
{
	return 0;
}

int uae_shmget (uae_key_t key, size_t size, int shmflg, const TCHAR *name)
{
	int result = -1;

	if ((key == UAE_IPC_PRIVATE) || ((shmflg & UAE_IPC_CREAT) && (find_shmkey (key) == -1))) {
		write_log (_T("shmget of size %zd (%zdk) for %s\n"), size, size >> 10, name);
		if ((result = get_next_shmkey ()) != -1) {
			shmids[result].size = size;
			_tcscpy (shmids[result].name, name);
		} else {
			result = -1;
		}
	}
	return result;
}

int uae_shmctl (int shmid, int cmd, struct uae_shmid_ds *buf)
{
	int result = -1;

	if ((find_shmkey (shmid) != -1) && buf) {
		switch (cmd)
		{
		case UAE_IPC_STAT:
			*buf = shmids[shmid];
			result = 0;
			break;
		case UAE_IPC_RMID:
			VirtualFree (shmids[shmid].attached, shmids[shmid].size, MEM_DECOMMIT);
			shmids[shmid].key = -1;
			shmids[shmid].name[0] = '\0';
			shmids[shmid].size = 0;
			shmids[shmid].attached = 0;
			shmids[shmid].mode = 0;
			result = 0;
			break;
		}
	}
	return result;
}

#endif

#ifdef FSUAE
/* The function isinf is provided by libc. */
/* FIXME: Replace with HAVE_ISINF? */
#else
int isinf (double x)
{
	const int nClass = _fpclass (x);
	int result;
	if (nClass == _FPCLASS_NINF || nClass == _FPCLASS_PINF)
		result = 1;
	else
		result = 0;
	return result;
}
#endif
