/*
 * sao_thp_probe -- can one VMA be both PROT_SAO (hardware TSO for the
 * emulator) and transparent-huge-page backed on this kernel?
 *
 * mmap 64 MB anonymous, madvise(MADV_HUGEPAGE), touch every page, then
 * mprotect(PROT_READ|PROT_WRITE|PROT_SAO) and read the region back out of
 * /proc/self/smaps: we want the `ar` VmFlag AND a non-zero AnonHugePages on
 * the SAME mapping.  If AnonHugePages drops to 0 after the mprotect, the
 * kernel split the huge pages to apply SAO and THP and HWTSO are exclusive
 * for that memory.  Exit 0 = both, 1 = SAO only, 2 = THP only, 3 = neither,
 * 4 = SAO refused (EINVAL: radix MMU or kernel without PROT_SAO).
 *
 *   gcc -O2 -o sao_thp_probe sao_thp_probe.c && ./sao_thp_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#ifndef PROT_SAO
#define PROT_SAO 0x10
#endif

static void report( const char *when, void *base, size_t size, long *huge, int *sao )
{
    char line[512], want[64];
    FILE *f = fopen( "/proc/self/smaps", "r" );
    int in = 0;
    *huge = -1; *sao = 0;
    snprintf( want, sizeof(want), "%lx-", (unsigned long)base );
    while (f && fgets( line, sizeof(line), f ))
    {
        /* mapping headers start with a lowercase hex address; field lines start with a capital */
        if (line[0] >= '0' && line[0] <= 'f' && !(line[0] >= 'A' && line[0] <= 'Z')) in = !strncmp( line, want, strlen(want) );
        if (!in) continue;
        if (!strncmp( line, "AnonHugePages:", 14 )) *huge = atol( line + 14 );
        if (!strncmp( line, "VmFlags:", 8 )) *sao = strstr( line, " ar" ) != NULL;
    }
    if (f) fclose( f );
    printf( "%-22s AnonHugePages=%ld kB  sao(ar)=%d\n", when, *huge, *sao );
}

int main( void )
{
    size_t size = 64u << 20;
    long huge0, huge1; int sao0, sao1;
    /* align to 16 MB so a hash-64K PMD can cover it */
    char *raw = mmap( NULL, size + (16u << 20), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if (raw == MAP_FAILED) { perror( "mmap" ); return 3; }
    char *base = (char *)(((unsigned long)raw + (16u << 20) - 1) & ~((16ul << 20) - 1));
    if (madvise( base, size, MADV_HUGEPAGE )) perror( "madvise(MADV_HUGEPAGE)" );
    for (size_t i = 0; i < size; i += 4096) base[i] = 1;
    report( "after touch:", base, size, &huge0, &sao0 );
    if (mprotect( base, size, PROT_READ | PROT_WRITE | PROT_SAO ))
    {
        printf( "mprotect(PROT_SAO): %s -- SAO unavailable here\n", strerror( errno ) );
        return 4;
    }
    for (size_t i = 0; i < size; i += 4096) base[i]++;
    report( "after PROT_SAO+touch:", base, size, &huge1, &sao1 );
    if (sao1 && huge1 > 0) { puts( "RESULT: SAO and THP coexist on one VMA" ); return 0; }
    if (sao1)             { puts( "RESULT: SAO only -- huge pages were split or never formed" ); return 1; }
    if (huge1 > 0)        { puts( "RESULT: THP only -- SAO flag not shown" ); return 2; }
    puts( "RESULT: neither" ); return 3;
}
