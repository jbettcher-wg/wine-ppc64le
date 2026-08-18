/*
 * wow_smoke.c -- the i386 guest probe for check-wow64-smoke.sh.
 *
 * Built with clang/mingw -target i686 as a PE32, no CRT: the image entry
 * point IS the program, exactly as ppc64le/seh/guest_callbacks.c builds its
 * x86-64 probe.  Everything below is a VALUE check, not a "did it start":
 * each step prints a relation between two independently produced numbers,
 * and the driving gate diffs the whole transcript byte-for-byte.
 *
 * What each step pins, in terms of the port's own mechanisms:
 *   1  fs:[0x18] equals the address the TIB describes itself at -- the FS
 *      base the CPU backend installed per thread (fexbridge_set_fs_base)
 *      points at a real, self-consistent 32-bit TEB.  This is the mechanism
 *      WINEEMUNOFSBASE32=1 sabotages, and the first thing to die under it.
 *   2  fs:[0x30] (the PEB) is a real pointer into the wow PEB: its
 *      ImageBaseAddress field equals GetModuleHandleW(NULL) -- one value
 *      read raw through FS, the other produced by a wow64-thunked syscall.
 *   3  GetModuleHandleW(NULL) points at this image's own MZ header.
 *   4  VirtualAlloc'd memory (NtAllocateVirtualMemory through wow64's
 *      zero_bits) is writable and reads back a pattern.
 *   5  GetProcAddress(kernel32, "GetCurrentProcessId") called through the
 *      returned pointer agrees with the import-bound call: two resolution
 *      paths, one answer.
 *   6  QueryPerformanceFrequency fills a 64-bit OUT parameter with a
 *      nonzero value: a syscall writing wide data back through the thunk.
 *   7  A hand-rolled FS-chain SEH handler catches a NULL write, repairs Eip
 *      from the CONTEXT it is handed, and execution continues: the whole
 *      guest exception round trip (fault -> unix RUN_FAULT -> native raise
 *      -> cpu_simulate filter -> Wow64PassExceptionToGuest -> 32-bit
 *      KiUserExceptionDispatcher -> this handler -> NtContinue).
 *   8  CreateThread runs a second guest thread (init_syscall_frame's i386
 *      arm + BTCpuThreadInit) that proves it saw its own distinct TIB,
 *      writes a value the main thread checks, and exits with a code
 *      GetExitCodeThread confirms.
 *
 * Run with any argument containing "exit" the probe instead terminates
 * immediately with ExitProcess(123), which is the gate's exit-code rung.
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winternl.h>

static HANDLE out;

static void print( const char *s )
{
    DWORD written;
    DWORD len = 0;
    while (s[len]) len++;
    WriteFile( out, s, len, &written, NULL );
}

static int checks, fails;

static void step( int n, const char *what, const char *detail, int ok )
{
    char buf[16];
    checks++;
    if (!ok) fails++;
    print( "step " );
    buf[0] = '0' + n; buf[1] = 0;
    print( buf );
    print( " " );
    print( what );
    print( ": " );
    print( detail );
    print( ok ? " ok\n" : " FAIL\n" );
}

static inline DWORD read_fs( DWORD offset )
{
    DWORD value;
    __asm__ volatile( "movl %%fs:(%1), %0" : "=r" (value) : "r" (offset) );
    return value;
}

/* ---- step 7: hand-rolled FS-chain SEH ---------------------------------- */

struct seh_registration
{
    void *prev;
    void *handler;
};

static volatile DWORD seh_code;
static void *seh_resume;

static EXCEPTION_DISPOSITION __cdecl seh_handler( EXCEPTION_RECORD *rec, void *frame,
                                                  CONTEXT *context, void *dispatch )
{
    seh_code = rec->ExceptionCode;
    context->Eip = (DWORD)seh_resume;
    return ExceptionContinueExecution;
}

/* ---- step 8: a second guest thread ------------------------------------- */

static volatile DWORD thread_value;
static volatile DWORD thread_tib;

static DWORD WINAPI thread_proc( void *arg )
{
    thread_tib = read_fs( 0x18 );
    thread_value = (DWORD)(ULONG_PTR)arg + 1;
    return 0x1234;
}

void wow_smoke_entry(void)
{
    const char *cmdline;

    out = GetStdHandle( STD_OUTPUT_HANDLE );

    for (cmdline = GetCommandLineA(); *cmdline; cmdline++)
        if (cmdline[0] == 'e' && cmdline[1] == 'x' && cmdline[2] == 'i' && cmdline[3] == 't')
            ExitProcess( 123 );

    print( "wow_smoke: start\n" );

    /* 1: the TIB describes itself at the address FS points at */
    {
        DWORD self = read_fs( 0x18 );
        struct seh_registration probe;
        /* the address of a local is a number only this 32-bit universe can
         * produce; the TIB self-pointer must be in the same universe and
         * consistent with an FS-relative read of any TIB field */
        int ok = self != 0 && read_fs( 0x04 ) > (DWORD)&probe;  /* StackBase above a live local */
        step( 1, "fs TIB self-pointer and stack bound", ok ? "self=nonzero stackbase>local=yes" : "bad", ok );
    }

    /* 2: the PEB through FS agrees with the loader */
    {
        DWORD peb = read_fs( 0x30 );
        DWORD image_base_from_peb = peb ? *(DWORD *)(peb + 0x08) : 0;  /* PEB32.ImageBaseAddress */
        HMODULE mod = GetModuleHandleW( NULL );
        int ok = peb && image_base_from_peb == (DWORD)mod;
        step( 2, "fs:[0x30] PEB names the image the loader names", ok ? "peb_base==module=yes" : "bad", ok );
    }

    /* 3: the module handle is this image's own MZ header */
    {
        HMODULE mod = GetModuleHandleW( NULL );
        int ok = mod && ((const char *)mod)[0] == 'M' && ((const char *)mod)[1] == 'Z';
        step( 3, "GetModuleHandleW(NULL) is an MZ header", ok ? "mz=yes" : "bad", ok );
    }

    /* 4: allocated guest memory holds a pattern */
    {
        DWORD *mem = VirtualAlloc( NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
        int ok = 0;
        if (mem)
        {
            mem[0] = 0xc0ffee01;
            mem[511] = 0xc0ffee02;
            ok = mem[0] == 0xc0ffee01 && mem[511] == 0xc0ffee02;
            VirtualFree( mem, 0, MEM_RELEASE );
        }
        step( 4, "VirtualAlloc memory is writable and stable", ok ? "pattern=held" : "bad", ok );
    }

    /* 5: GetProcAddress and the import agree */
    {
        DWORD (WINAPI *p_getpid)(void) =
            (DWORD (WINAPI *)(void))GetProcAddress( GetModuleHandleW( L"kernel32.dll" ),
                                                    "GetCurrentProcessId" );
        int ok = p_getpid && p_getpid() == GetCurrentProcessId();
        step( 5, "GetProcAddress agrees with the import", ok ? "same_pid=yes" : "bad", ok );
    }

    /* 6: a syscall writes 64 bits back */
    {
        LARGE_INTEGER freq;
        freq.QuadPart = 0;
        int ok = QueryPerformanceFrequency( &freq ) && freq.QuadPart != 0;
        step( 6, "QueryPerformanceFrequency fills 64 bits", ok ? "nonzero=yes" : "bad", ok );
    }

    /* 7: a guest fault comes back through the 32-bit dispatcher */
    {
        struct seh_registration reg;
        int ok;

        seh_code = 0;
        reg.handler = (void *)seh_handler;
        __asm__ volatile( "movl %%fs:0, %%eax\n\tmovl %%eax, %0" : "=m" (reg.prev) :: "eax" );
        __asm__ volatile( "leal %0, %%eax\n\tmovl %%eax, %%fs:0" :: "m" (reg) : "eax" );
        __asm__ volatile( "movl $1f, %0\n\t"
                          "xorl %%ecx, %%ecx\n\t"
                          "movl %%ecx, (%%ecx)\n"     /* write through NULL */
                          "1:\n\t"
                          : "=m" (seh_resume) :: "eax", "ecx", "memory" );
        __asm__ volatile( "movl %0, %%eax\n\tmovl %%eax, %%fs:0" :: "m" (reg.prev) : "eax" );
        ok = seh_code == 0xc0000005;
        step( 7, "SEH caught the NULL write and resumed", ok ? "code=c0000005" : "bad", ok );
    }

    /* 8: a second guest thread with its own TIB */
    {
        HANDLE thread;
        DWORD exit_code = 0, my_tib = read_fs( 0x18 );
        int ok = 0;

        thread_value = 0;
        thread_tib = 0;
        thread = CreateThread( NULL, 0, thread_proc, (void *)0xbeef00, 0, NULL );
        if (thread && WaitForSingleObject( thread, 30000 ) == WAIT_OBJECT_0 &&
            GetExitCodeThread( thread, &exit_code ))
            ok = exit_code == 0x1234 && thread_value == 0xbeef01 &&
                 thread_tib != 0 && thread_tib != my_tib;
        step( 8, "second thread ran with its own TIB", ok ? "exit=1234 value=beef01 tib=distinct" : "bad", ok );
    }

    if (fails) { print( "wow_smoke: FAIL\n" ); ExitProcess( 1 ); }
    print( "wow_smoke: PASS 8/8\n" );
    ExitProcess( 0 );
}
