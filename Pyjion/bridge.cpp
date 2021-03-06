/* This file serves as a bridge between two portions of Pyjion - the CoreCLR portion
 * and the normal C++ portion.  CoreCLR defines a PAL which replaces the entire C/C++
 * standard library with one which is Windows like and compatible with MSVC++.  Any
 * files in this part of the world don't get built with the standard header files, and
 * therefore can't use any standard functionality.
 *
 * When we need to route around that we implement our own wrappers over here and make 
 * them available to the CoreCLR part of the world via bridge.h.  bridge.h doesn't
 * bring in any header files, and just exports the implementation defines here.
 */
#ifdef PLATFORM_UNIX
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstdarg>
#include <cstdio>

#if !PLATFORM_UNIX
#include <windows.h>
#endif

#ifdef PLATFORM_UNIX
extern "C" int g_executableMmapProt = PROT_READ | PROT_WRITE | PROT_EXEC;
extern "C" int g_privateAnonMmapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif

extern "C" size_t pyjit_pagesize() {
#ifdef PLATFORM_UNIX
	return sysconf(_SC_PAGESIZE);
#else
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#endif
}

int pyjit_log(const char *__restrict format, ...) {
	va_list arglist;

	va_start(arglist, format);
	int res = vprintf(format, arglist);
	va_end(arglist);
	return res;
}


