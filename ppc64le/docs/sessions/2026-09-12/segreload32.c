/* 32-bit probe: does a segment-register reload change where string
 * instructions write?  On real x86 (flat model) it never does. */
#include <windows.h>
#include <stdio.h>

static volatile ULONG_PTR fault_addr, fault_edi;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
    fault_edi  = ep->ContextRecord->Edi;
    /* skip the faulting rep stos/movs: jump to the label stored in Ebx */
    ep->ContextRecord->Eip = ep->ContextRecord->Ebx;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static unsigned buf[16];

#define REPORT(name) do { \
    if (fault_addr) printf("%-22s FAULT  info[1]=%08lx edi=%08lx  base leak=%08lx\n", name, \
                           (unsigned long)fault_addr, (unsigned long)fault_edi, (unsigned long)(fault_addr - fault_edi)); \
    else            printf("%-22s ok     buf[0]=%08x\n", name, buf[0]); \
    fault_addr = fault_edi = 0; memset(buf, 0, sizeof(buf)); } while (0)

int main(void)
{
    AddVectoredExceptionHandler(1, veh);

    __asm__ volatile("movl $1f, %%ebx\n\t" "rep stosl\n" "1:" : : "D"(buf), "c"(4), "a"(0x41414141) : "ebx", "memory", "cc");
    REPORT("stos, no reload");

    __asm__ volatile("pushl %%es\n\t" "popl %%es\n\t" "movl $1f, %%ebx\n\t" "rep stosl\n" "1:" : : "D"(buf), "c"(4), "a"(0x42424242) : "ebx", "memory", "cc");
    REPORT("stos after pop es");

    __asm__ volatile("pushl %%ds\n\t" "popl %%ds\n\t" "movl $1f, %%ebx\n\t" "rep movsl\n" "1:" : : "D"(buf), "S"(buf + 8), "c"(4) : "ebx", "memory", "cc");
    REPORT("movs after pop ds");

    __asm__ volatile("movw %%ss, %%ax\n\t" "movw %%ax, %%es\n\t" "movl $1f, %%ebx\n\t" "rep stosl\n" "1:" : : "D"(buf), "c"(4) : "eax", "ebx", "memory", "cc");
    REPORT("stos after mov es,ss");
    return 0;
}
