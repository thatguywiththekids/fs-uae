
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "fpp.h"

void my_trim(TCHAR *s)
{
	int len;
	while (_tcslen(s) > 0 && _tcscspn(s, _T("\t \r\n")) == 0)
		memmove(s, s + 1, (_tcslen(s + 1) + 1) * sizeof(TCHAR));
	len = _tcslen(s);
	while (len > 0 && _tcscspn(s + len - 1, _T("\t \r\n")) == 0)
		s[--len] = '\0';
}

void write_log(const TCHAR *format, ...)
{

}
void f_out(void *f, const TCHAR *format, ...)
{

}

TCHAR *buf_out(TCHAR *buffer, int *bufsize, const TCHAR *format, ...)
{
	int count;
	va_list parms;
	va_start(parms, format);

	if (buffer == NULL)
		return 0;
	count = _vsntprintf(buffer, (*bufsize) - 1, format, parms);
	va_end(parms);
	*bufsize -= _tcslen(buffer);
	return buffer + _tcslen(buffer);
}

void fpux_restore(int *v)
{
}
void fp_init_native(void)
{
	wprintf(_T("fp_init_native called!"));
	exit(0);
}
void fp_init_native_80(void)
{
	wprintf(_T("fp_init_native_80 called!"));
	exit(0);
}
void init_fpucw_x87(void)
{
}
void init_fpucw_x87_80(void)
{
}

int debugmem_get_segment(uaecptr addr, bool *exact, bool *ext, TCHAR *out, TCHAR *name)
{
	return 0;
}
int debugmem_get_symbol(uaecptr addr, TCHAR *out, int maxsize)
{
	return 0;
}
int debugmem_get_sourceline(uaecptr addr, TCHAR *out, int maxsize)
{
	return -1;
}
bool debugger_get_library_symbol(uaecptr base, uaecptr addr, TCHAR *out)
{
	return false;
}

int debug_safe_addr(uaecptr addr, int size)
{
	return 1;
}

void set_cpu_caches(bool flush)
{
}

void flush_icache(int v)
{
}

void mmu_tt_modified(void)
{
}

uae_u16 REGPARAM2 mmu_set_tc(uae_u16 tc)
{
	return 0;
}