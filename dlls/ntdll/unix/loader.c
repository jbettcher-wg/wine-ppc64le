/*
 * Unix interface for loader functions
 *
 * Copyright (C) 2020 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#ifdef HAVE_PWD_H
# include <pwd.h>
#endif
#ifdef HAVE_ELF_H
# include <elf.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_SYS_AUXV_H
# include <sys/auxv.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#include <limits.h>
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif
#ifdef __APPLE__
# include <CoreFoundation/CoreFoundation.h>
# define LoadResource MacLoadResource
# define GetCurrentThread MacGetCurrentThread
# include <CoreServices/CoreServices.h>
# undef LoadResource
# undef GetCurrentThread
# include <pthread.h>
# include <mach/mach.h>
# include <mach/mach_error.h>
# include <mach-o/getsect.h>
# include <crt_externs.h>
# ifndef _POSIX_SPAWN_DISABLE_ASLR
#  define _POSIX_SPAWN_DISABLE_ASLR 0x0100
# endif
# define environ (*_NSGetEnviron())
#else
  extern char **environ;
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winbase.h"
#include "winnls.h"
#include "winioctl.h"
#include "winternl.h"
#include "unix_private.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#if defined __i386__ || (defined __x86_64__ && !defined __APPLE__) || defined __powerpc64__
#define SO_DLLS_SUPPORTED
#endif

void *pDbgUiRemoteBreakin = NULL;
void *pKiRaiseUserExceptionDispatcher = NULL;
void *pKiUserExceptionDispatcher = NULL;
void *pKiUserApcDispatcher = NULL;
void *pKiUserCallbackDispatcher = NULL;
void *pKiUserEmulationDispatcher = NULL;
void *pLdrInitializeThunk = NULL;
void *pRtlUserThreadStart = NULL;
void *p__wine_ctrl_routine = NULL;
#ifdef __powerpc64__
void *p__wine_init_teb = NULL;
#endif
SYSTEM_DLL_INIT_BLOCK *pLdrSystemDllInitBlock = NULL;

#ifdef __GNUC__
static void fatal_error( const char *err, ... ) __attribute__((noreturn, format(printf,1,2)));
#endif

static const char *bin_dir;
static const char *dll_dir;
static const char *ntdll_dir;
static const char *alt_build_dir;
static SIZE_T dll_path_maxlen;

const char *home_dir = NULL;
const char *data_dir = NULL;
const char *build_dir = NULL;
const char *config_dir = NULL;
const char *wineloader = NULL;
const char **dll_paths = NULL;
const char **system_dll_paths = NULL;
const char *user_name = NULL;
SECTION_IMAGE_INFORMATION main_image_info = { NULL };

/* die on a fatal error; use only during initialization */
static void fatal_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    va_end( args );
    exit(1);
}

static void set_max_limit( int limit )
{
    struct rlimit rlimit;

    if (!getrlimit( limit, &rlimit ))
    {
        rlimit.rlim_cur = rlimit.rlim_max;
        if (!setrlimit( limit, &rlimit )) return;
#ifdef __APPLE__
        if (limit == RLIMIT_NOFILE)
        {
            /* macOS before Big Sur fails if rlim_max is larger than maxfilesperproc */
            unsigned int nlimit = 0;
            size_t size = sizeof(nlimit);
            sysctlbyname("kern.maxfilesperproc", &nlimit, &size, NULL, 0);
            rlimit.rlim_cur = max( nlimit, OPEN_MAX );
            if (!setrlimit( RLIMIT_NOFILE, &rlimit )) return;
        }
#endif
        WARN("Failed to raise limit %d\n", limit);
    }
}

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
    char *p, *fullpath = realpath( name, NULL );

    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    if (len)
    {
        memcpy( ret, dir, len );
        if (ret[len - 1] != '/') ret[len++] = '/';
        if (name[0] == '/') name++;
    }
    strcpy( ret + len, name );
    return ret;
}

/* build a path with the relative dir from 'from' to 'dest' appended to base */
static char *build_relative_path( const char *base, const char *from, const char *dest )
{
    const char *start;
    char *ret;
    unsigned int dotdots = 0;

    for (;;)
    {
        while (*from == '/') from++;
        while (*dest == '/') dest++;
        start = dest;  /* save start of next path element */
        if (!*from) break;

        while (*from && *from != '/' && *from == *dest) { from++; dest++; }
        if ((!*from || *from == '/') && (!*dest || *dest == '/')) continue;

        do  /* count remaining elements in 'from' */
        {
            dotdots++;
            while (*from && *from != '/') from++;
            while (*from == '/') from++;
        }
        while (*from);
        break;
    }

    ret = malloc( strlen(base) + 3 * dotdots + strlen(start) + 2 );
    strcpy( ret, base );
    while (dotdots--) strcat( ret, "/.." );

    if (!start[0]) return ret;
    strcat( ret, "/" );
    strcat( ret, start );
    return ret;
}

/* build a path to a binary and exec it */
static int build_path_and_exec( pid_t *pid, const char *dir, const char *name, char **argv )
{
    int ret;

    argv[0] = build_path( dir, name );
    ret = posix_spawn( pid, argv[0], NULL, NULL, argv, environ );
    free( argv[0] );
    return ret;
}


static const char *get_so_dir( WORD machine )
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:  return "/i386-unix";
    case IMAGE_FILE_MACHINE_AMD64: return "/x86_64-unix";
    case IMAGE_FILE_MACHINE_ARMNT: return "/arm-unix";
    case IMAGE_FILE_MACHINE_ARM64: return "/aarch64-unix";
    case IMAGE_FILE_MACHINE_POWERPC64: return "/ppc64-unix";
    default: return "";
    }
}

static const char *get_pe_dir( WORD machine )
{
    switch(machine)
    {
    case IMAGE_FILE_MACHINE_I386:  return "/i386-windows";
    case IMAGE_FILE_MACHINE_AMD64: return "/x86_64-windows";
    case IMAGE_FILE_MACHINE_ARMNT: return "/arm-windows";
    case IMAGE_FILE_MACHINE_ARM64: return "/aarch64-windows";
    case IMAGE_FILE_MACHINE_POWERPC64: return "/ppc64-windows";
    default: return "";
    }
}

static WORD get_alt_machine( WORD machine )
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:  return IMAGE_FILE_MACHINE_AMD64;
    case IMAGE_FILE_MACHINE_AMD64: return IMAGE_FILE_MACHINE_I386;
    case IMAGE_FILE_MACHINE_ARMNT: return IMAGE_FILE_MACHINE_ARM64;
    case IMAGE_FILE_MACHINE_ARM64: return IMAGE_FILE_MACHINE_ARMNT;
    default: return machine;
    }
}

static void set_dll_path(void)
{
    char *p, *path = getenv( "WINEDLLPATH" );
    int i, count = 0;

    if (path) for (p = path, count = 1; *p; p++) if (*p == ':') count++;

    dll_paths = malloc( (count + 2) * sizeof(*dll_paths) );
    count = 0;

    if (!build_dir) dll_paths[count++] = dll_dir;

    if (path)
    {
        path = strdup(path);
        for (p = strtok( path, ":" ); p; p = strtok( NULL, ":" )) dll_paths[count++] = strdup( p );
        free( path );
    }

    for (i = 0; i < count; i++) dll_path_maxlen = max( dll_path_maxlen, strlen(dll_paths[i]) );
    dll_paths[count] = NULL;
}


static void set_system_dll_path(void)
{
    const char *p, *path = SYSTEMDLLPATH;
    int count = 0;

    if (path && *path) for (p = path, count = 1; *p; p++) if (*p == ':') count++;

    system_dll_paths = malloc( (count + 1) * sizeof(*system_dll_paths) );
    count = 0;

    if (path && *path)
    {
        char *path_copy = strdup(path);
        for (p = strtok( path_copy, ":" ); p; p = strtok( NULL, ":" ))
            system_dll_paths[count++] = strdup( p );
        free( path_copy );
    }
    system_dll_paths[count] = NULL;
}


static void set_home_dir(void)
{
    const char *home = getenv( "HOME" );
    const char *name = getenv( "USER" );
    const char *p;

    if (!home || !name)
    {
        struct passwd *pwd = getpwuid( getuid() );
        if (pwd)
        {
            if (!home) home = pwd->pw_dir;
            if (!name) name = pwd->pw_name;
        }
        if (!name) name = "wine";
    }
    if ((p = strrchr( name, '/' ))) name = p + 1;
    if ((p = strrchr( name, '\\' ))) name = p + 1;
    home_dir = strdup( home );
    user_name = strdup( name );
}


static void set_config_dir(void)
{
    char *p, *dir;
    const char *prefix = getenv( "WINEPREFIX" );

    if (prefix)
    {
        if (prefix[0] != '/')
            fatal_error( "invalid directory %s in WINEPREFIX: not an absolute path\n", prefix );
        config_dir = dir = strdup( prefix );
        for (p = dir + strlen(dir) - 1; p > dir && *p == '/'; p--) *p = 0;
    }
    else
    {
        if (!home_dir) fatal_error( "could not determine your home directory\n" );
        if (home_dir[0] != '/') fatal_error( "the home directory %s is not an absolute path\n", home_dir );
        config_dir = build_path( home_dir, ".wine" );
    }
}

static void init_paths(void)
{
    Dl_info info;

    if (!dladdr( init_paths, &info ) || !(ntdll_dir = realpath_dirname( info.dli_fname )))
        fatal_error( "cannot get path to ntdll.so\n" );

    if ((build_dir = remove_tail( ntdll_dir, "/dlls/ntdll" )))
    {
        wineloader = build_path( build_dir, "loader/wine" );
        alt_build_dir = realpath_dirname( build_path( build_dir, "loader-wow64" ));
    }
    else
    {
        if (!(dll_dir = remove_tail( ntdll_dir, get_so_dir(current_machine) ))) dll_dir = ntdll_dir;
        bin_dir = build_relative_path( dll_dir, LIBDIR "/wine", BINDIR );
        data_dir = build_relative_path( dll_dir, LIBDIR "/wine", DATADIR "/wine" );
        wineloader = build_path( ntdll_dir, "wine" );
    }

    set_dll_path();
    set_system_dll_path();
    set_home_dir();
    set_config_dir();
}


/***********************************************************************
 *           get_alternate_wineloader
 */
char *get_alternate_wineloader( WORD machine )
{
    const char *arch;
    BOOL force_wow64 = (arch = getenv( "WINEARCH" )) && !strcmp( arch, "wow64" );
    char *ret = NULL;

    if (is_win64)
    {
        if (force_wow64) return NULL;
        if (machine != get_alt_machine( current_machine )) return NULL;
    }
    else
    {
        if (!force_wow64 && machine == current_machine) return NULL;
        machine = get_alt_machine( current_machine );
    }

    if (!build_dir)
        asprintf( &ret, "%s%s/wine", dll_dir, get_so_dir( machine ));
    else if (alt_build_dir)
        asprintf( &ret, "%s/loader/wine", alt_build_dir );

    return ret;
}


static void preloader_exec( char **argv )
{
#ifdef HAVE_WINE_PRELOADER
    asprintf( &argv[0], "%s-preloader", argv[1] );
#ifdef __APPLE__
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init( &attr );
        posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETEXEC | _POSIX_SPAWN_DISABLE_ASLR );
        posix_spawn( NULL, argv[0], NULL, &attr, argv, *_NSGetEnviron() );
        posix_spawnattr_destroy( &attr );
    }
#endif
    execv( argv[0], argv );
    free( argv[0] );
#endif
    execv( argv[1], argv + 1 );
}

/* exec the appropriate wine loader for the specified machine */
static NTSTATUS loader_exec( char **argv, WORD machine )
{
    static char noexec[] = "WINELOADERNOEXEC=1";

    putenv( noexec );

    if (((argv[1] = get_alternate_wineloader( machine )))) preloader_exec( argv );

    argv[1] = strdup( wineloader );
    preloader_exec( argv );
    return STATUS_INVALID_IMAGE_FORMAT;
}


/***********************************************************************
 *           exec_wineloader
 *
 * argv[0] and argv[1] must be reserved for the preloader and loader respectively.
 */
NTSTATUS exec_wineloader( char **argv, int socketfd, const struct pe_image_info *pe_info )
{
    WORD machine = pe_info->machine;
    ULONGLONG res_start = pe_info->base;
    ULONGLONG res_end = pe_info->base + pe_info->map_size;
    char preloader_reserve[64], socket_env[64];

    if (pe_info->wine_fakedll) res_start = res_end = 0;
    if (pe_info->image_flags & IMAGE_FLAGS_ComPlusNativeReady)
        machine = is_machine_64bit( native_machine ) ? IMAGE_FILE_MACHINE_AMD64 : native_machine;

    signal( SIGPIPE, SIG_DFL );

    snprintf( socket_env, sizeof(socket_env), "WINESERVERSOCKET=%u", socketfd );
    snprintf( preloader_reserve, sizeof(preloader_reserve), "WINEPRELOADRESERVE=%x%08x-%x%08x",
             (UINT)(res_start >> 32), (UINT)res_start, (UINT)(res_end >> 32), (UINT)res_end );

    putenv( preloader_reserve );
    putenv( socket_env );

    return loader_exec( argv, machine );
}


/***********************************************************************
 *           exec_wineserver
 *
 * Exec a new wine server.
 */
static int exec_wineserver( pid_t *pid, char **argv )
{
    char *path;

    if (!is_win64 && alt_build_dir)  /* look for 64-bit server */
        return build_path_and_exec( pid, alt_build_dir, "server/wineserver", argv );

    if (build_dir)
        return build_path_and_exec( pid, build_dir, "server/wineserver", argv );

    if (!build_path_and_exec( pid, bin_dir, "wineserver", argv )) return 0;
    if ((path = getenv( "WINESERVER" )) && !build_path_and_exec( pid, "", path, argv )) return 0;

    if ((path = getenv( "PATH" )))
    {
        for (path = strtok( strdup( path ), ":" ); path; path = strtok( NULL, ":" ))
            if (!build_path_and_exec( pid, path, "wineserver", argv )) return 0;
    }
    return build_path_and_exec( pid, BINDIR, "wineserver", argv );
}


/***********************************************************************
 *           start_server
 *
 * Start a new wine server.
 */
void start_server( BOOL debug )
{
    static BOOL started;  /* we only try once */
    char *argv[3];
    static char debug_flag[] = "-d";

    if (!started)
    {
        int status;
        pid_t pid;

        argv[1] = debug ? debug_flag : NULL;
        argv[2] = NULL;
        if (exec_wineserver( &pid, argv )) fatal_error( "could not exec wineserver\n" );
        waitpid( pid, &status, 0 );
        status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (status == 2) return;  /* server lock held by someone else, will retry later */
        if (status) exit(status);  /* server failed */
        started = TRUE;
    }
}


#ifdef SO_DLLS_SUPPORTED

/* adjust an array of pointers to make them into RVAs */
static inline void fixup_rva_ptrs( void *array, BYTE *base, unsigned int count )
{
    BYTE **src = array;
    DWORD *dst = array;

    for ( ; count; count--, src++, dst++) *dst = *src ? *src - base : 0;
}

/* fixup an array of RVAs by adding the specified delta */
static inline void fixup_rva_dwords( DWORD *ptr, int delta, unsigned int count )
{
    for ( ; count; count--, ptr++) if (*ptr) *ptr += delta;
}


/* fixup an array of name/ordinal RVAs by adding the specified delta */
static inline void fixup_rva_names( UINT_PTR *ptr, int delta )
{
    for ( ; *ptr; ptr++) if (!(*ptr & IMAGE_ORDINAL_FLAG)) *ptr += delta;
}


/* fixup RVAs in the resource directory */
static void fixup_so_resources( IMAGE_RESOURCE_DIRECTORY *dir, BYTE *root, int delta )
{
    IMAGE_RESOURCE_DIRECTORY_ENTRY *entry = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    unsigned int i;

    for (i = 0; i < dir->NumberOfNamedEntries + dir->NumberOfIdEntries; i++, entry++)
    {
        void *ptr = root + entry->OffsetToDirectory;
        if (entry->DataIsDirectory) fixup_so_resources( ptr, root, delta );
        else fixup_rva_dwords( &((IMAGE_RESOURCE_DATA_ENTRY *)ptr)->OffsetToData, delta, 1 );
    }
}

/***********************************************************************
 *           fill_builtin_image_info
 */
static void fill_builtin_image_info( void *module, struct pe_image_info *info )
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
    const IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((const BYTE *)dos + dos->e_lfanew);

    memset( info, 0, sizeof(*info) );
    info->base            = nt->OptionalHeader.ImageBase;
    info->entry_point     = nt->OptionalHeader.AddressOfEntryPoint;
    info->map_size        = nt->OptionalHeader.SizeOfImage;
    info->stack_size      = nt->OptionalHeader.SizeOfStackReserve;
    info->stack_commit    = nt->OptionalHeader.SizeOfStackCommit;
    info->subsystem       = nt->OptionalHeader.Subsystem;
    info->subsystem_minor = nt->OptionalHeader.MinorSubsystemVersion;
    info->subsystem_major = nt->OptionalHeader.MajorSubsystemVersion;
    info->osversion_major = nt->OptionalHeader.MajorOperatingSystemVersion;
    info->osversion_minor = nt->OptionalHeader.MinorOperatingSystemVersion;
    info->image_charact   = nt->FileHeader.Characteristics;
    info->dll_charact     = nt->OptionalHeader.DllCharacteristics;
    info->machine         = nt->FileHeader.Machine;
    info->contains_code   = TRUE;
    info->wine_builtin    = TRUE;
    info->header_size     = nt->OptionalHeader.SizeOfHeaders;
    info->file_size       = nt->OptionalHeader.SizeOfImage;
    info->checksum        = nt->OptionalHeader.CheckSum;
}

/*************************************************************************
 *		map_so_dll
 *
 * Map a builtin dll in memory and fixup RVAs.
 */
static NTSTATUS map_so_dll( const IMAGE_NT_HEADERS *nt_descr, HMODULE module )
{
    static const char builtin_signature[32] = "Wine builtin DLL";
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    BYTE *addr = (BYTE *)module;
    DWORD code_start, code_end, data_start, data_end;
    DWORD align_mask = nt_descr->OptionalHeader.SectionAlignment - 1;
    int delta, nb_sections = 2;  /* code + data */
    unsigned int i;

    code_start = (sizeof(IMAGE_DOS_HEADER)
                  + sizeof(builtin_signature)
                  + sizeof(IMAGE_NT_HEADERS)
                  + nb_sections * sizeof(IMAGE_SECTION_HEADER)
                  + align_mask) & ~align_mask;

    if (anon_mmap_fixed( addr, code_start, PROT_READ | PROT_WRITE, 0 ) != addr) return STATUS_NO_MEMORY;

    dos = (IMAGE_DOS_HEADER *)addr;
    nt  = (IMAGE_NT_HEADERS *)((BYTE *)(dos + 1) + sizeof(builtin_signature));
    sec = (IMAGE_SECTION_HEADER *)(nt + 1);

    /* build the DOS and NT headers */

    dos->e_magic    = IMAGE_DOS_SIGNATURE;
    dos->e_cblp     = 0x90;
    dos->e_cp       = 3;
    dos->e_cparhdr  = (sizeof(*dos) + 0xf) / 0x10;
    dos->e_minalloc = 0;
    dos->e_maxalloc = 0xffff;
    dos->e_ss       = 0x0000;
    dos->e_sp       = 0x00b8;
    dos->e_lfanew   = sizeof(*dos) + sizeof(builtin_signature);
    memcpy( dos + 1, builtin_signature, sizeof(builtin_signature) );

    *nt = *nt_descr;

    delta      = (const BYTE *)nt_descr - addr;
    data_start = delta & ~align_mask;
#ifdef __APPLE__
    {
        Dl_info dli;
        unsigned long data_size;
        /* need the mach_header, not the PE header, to give to getsegmentdata(3) */
        dladdr(addr, &dli);
        code_end   = getsegmentdata(dli.dli_fbase, "__DATA", &data_size) - addr;
        data_end   = (code_end + data_size + align_mask) & ~align_mask;
    }
#else
    code_end   = data_start;
    data_end   = (nt->OptionalHeader.SizeOfImage + delta + align_mask) & ~align_mask;
#endif

    fixup_rva_ptrs( &nt->OptionalHeader.AddressOfEntryPoint, addr, 1 );

    nt->FileHeader.NumberOfSections                = nb_sections;
    nt->OptionalHeader.BaseOfCode                  = code_start;
#ifndef _WIN64
    nt->OptionalHeader.BaseOfData                  = data_start;
#endif
    nt->OptionalHeader.SizeOfCode                  = code_end - code_start;
    nt->OptionalHeader.SizeOfInitializedData       = data_end - data_start;
    nt->OptionalHeader.SizeOfUninitializedData     = 0;
    nt->OptionalHeader.SizeOfImage                 = data_end;
    nt->OptionalHeader.ImageBase                   = (ULONG_PTR)addr;

    /* build the code section */

    memcpy( sec->Name, ".text", sizeof(".text") );
    sec->SizeOfRawData = code_end - code_start;
    sec->Misc.VirtualSize = sec->SizeOfRawData;
    sec->VirtualAddress   = code_start;
    sec->PointerToRawData = code_start;
    sec->Characteristics  = (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    sec++;

    /* build the data section */

    memcpy( sec->Name, ".data", sizeof(".data") );
    sec->SizeOfRawData = data_end - data_start;
    sec->Misc.VirtualSize = sec->SizeOfRawData;
    sec->VirtualAddress   = data_start;
    sec->PointerToRawData = data_start;
    sec->Characteristics  = (IMAGE_SCN_CNT_INITIALIZED_DATA |
                             IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_READ);
    sec++;

    for (i = 0; i < nt->OptionalHeader.NumberOfRvaAndSizes; i++)
        fixup_rva_dwords( &nt->OptionalHeader.DataDirectory[i].VirtualAddress, delta, 1 );

    /* build the import directory */

    dir = &nt->OptionalHeader.DataDirectory[IMAGE_FILE_IMPORT_DIRECTORY];
    if (dir->Size)
    {
        IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)(addr + dir->VirtualAddress);

        while (imports->Name)
        {
            fixup_rva_dwords( &imports->OriginalFirstThunk, delta, 1 );
            fixup_rva_dwords( &imports->Name, delta, 1 );
            fixup_rva_dwords( &imports->FirstThunk, delta, 1 );
            if (imports->OriginalFirstThunk)
                fixup_rva_names( (UINT_PTR *)(addr + imports->OriginalFirstThunk), delta );
            if (imports->FirstThunk)
                fixup_rva_names( (UINT_PTR *)(addr + imports->FirstThunk), delta );
            imports++;
        }
    }

    /* build the resource directory */

    dir = &nt->OptionalHeader.DataDirectory[IMAGE_FILE_RESOURCE_DIRECTORY];
    if (dir->Size)
    {
        void *ptr = addr + dir->VirtualAddress;
        fixup_so_resources( ptr, ptr, delta );
    }

    /* build the export directory */

    dir = &nt->OptionalHeader.DataDirectory[IMAGE_FILE_EXPORT_DIRECTORY];
    if (dir->Size)
    {
        IMAGE_EXPORT_DIRECTORY *exports = (IMAGE_EXPORT_DIRECTORY *)(addr + dir->VirtualAddress);

        fixup_rva_dwords( &exports->Name, delta, 1 );
        fixup_rva_dwords( &exports->AddressOfFunctions, delta, 1 );
        fixup_rva_dwords( &exports->AddressOfNames, delta, 1 );
        fixup_rva_dwords( &exports->AddressOfNameOrdinals, delta, 1 );
        fixup_rva_dwords( (DWORD *)(addr + exports->AddressOfNames), delta, exports->NumberOfNames );
        fixup_rva_ptrs( addr + exports->AddressOfFunctions, addr, exports->NumberOfFunctions );
    }

    /* build the delay import directory */

    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dir->Size)
    {
        IMAGE_DELAYLOAD_DESCRIPTOR *imports = (IMAGE_DELAYLOAD_DESCRIPTOR *)(addr + dir->VirtualAddress);

        while (imports->DllNameRVA)
        {
            fixup_rva_dwords( &imports->DllNameRVA, delta, 1 );
            fixup_rva_dwords( &imports->ModuleHandleRVA, delta, 1 );
            fixup_rva_dwords( &imports->ImportAddressTableRVA, delta, 1 );
            fixup_rva_dwords( &imports->ImportNameTableRVA, delta, 1 );
            fixup_rva_dwords( &imports->BoundImportAddressTableRVA, delta, 1 );
            fixup_rva_dwords( &imports->UnloadInformationTableRVA, delta, 1 );
            fixup_rva_names( (UINT_PTR *)(addr + imports->ImportNameTableRVA), delta );
            imports++;
        }
    }

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           dlopen_dll
 */
static NTSTATUS dlopen_dll( const char *so_name, UNICODE_STRING *nt_name, void **ret_module,
                            struct pe_image_info *image_info, BOOL prefer_native )
{
    void *module, *handle;
    const IMAGE_NT_HEADERS *nt;

    handle = dlopen( so_name, RTLD_NOW );
    if (!handle)
    {
        WARN( "failed to load .so lib %s: %s\n", debugstr_a(so_name), dlerror() );
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    if (!(nt = dlsym( handle, "__wine_spec_nt_header" )))
    {
        ERR( "invalid .so library %s, too old?\n", debugstr_a(so_name));
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    module = (HMODULE)((nt->OptionalHeader.ImageBase + 0xffff) & ~0xffff);
    if (get_builtin_so_handle( module ))  /* already loaded */
    {
        fill_builtin_image_info( module, image_info );
        *ret_module = module;
        dlclose( handle );
        return STATUS_SUCCESS;
    }

    if (map_so_dll( nt, module ))
    {
        dlclose( handle );
        return STATUS_NO_MEMORY;
    }

    fill_builtin_image_info( module, image_info );
    if (prefer_native && (image_info->dll_charact & IMAGE_DLLCHARACTERISTICS_PREFER_NATIVE))
    {
        TRACE( "%s has prefer-native flag, ignoring builtin\n", debugstr_a(so_name) );
        dlclose( handle );
        return STATUS_IMAGE_ALREADY_LOADED;
    }

    if (virtual_create_builtin_view( module, nt_name, image_info, handle ))
    {
        dlclose( handle );
        return STATUS_NO_MEMORY;
    }
#ifdef __x86_64__
    signal_disable_syscall_dispatch();
#endif
    *ret_module = module;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           load_so_dll
 */
static NTSTATUS load_so_dll( void *args )
{
    static const WCHAR soW[] = {'.','s','o',0};
    struct load_so_dll_params *params = args;
    UNICODE_STRING *nt_name = &params->nt_name;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING true_nt_name;
    struct pe_image_info info;
    char *unix_name;
    NTSTATUS status;
    DWORD len;

    if (get_load_order( nt_name, FALSE, NULL ) == LO_DISABLED) return STATUS_DLL_NOT_FOUND;
    InitializeObjectAttributes( &attr, nt_name, OBJ_CASE_INSENSITIVE, 0, 0 );
    if (!get_nt_and_unix_names( &attr, &true_nt_name, &unix_name, FILE_OPEN, FALSE ))
    {
        /* remove .so extension from Windows name */
        len = nt_name->Length / sizeof(WCHAR);
        if (len > 3 && !wcsicmp( nt_name->Buffer + len - 3, soW )) nt_name->Length -= 3 * sizeof(WCHAR);

        status = dlopen_dll( unix_name, nt_name, params->module, &info, FALSE );
    }
    else status = STATUS_DLL_NOT_FOUND;

    free( unix_name );
    free( true_nt_name.Buffer );
    return status;
}

/* check if the library is the correct architecture */
/* only returns false for a valid library of the wrong arch */
static int check_library_arch( int fd )
{
#ifdef __APPLE__
    struct  /* Mach-O header */
    {
        unsigned int magic;
        unsigned int cputype;
    } header;

    if (read( fd, &header, sizeof(header) ) != sizeof(header)) return 1;
    if (header.magic != 0xfeedface) return 1;
    if (sizeof(void *) == sizeof(int)) return !(header.cputype >> 24);
    else return (header.cputype >> 24) == 1; /* CPU_ARCH_ABI64 */
#else
    struct  /* ELF header */
    {
        unsigned char magic[4];
        unsigned char class;
        unsigned char data;
        unsigned char version;
    } header;

    if (read( fd, &header, sizeof(header) ) != sizeof(header)) return 1;
    if (memcmp( header.magic, "\177ELF", 4 )) return 1;
    if (header.version != 1 /* EV_CURRENT */) return 1;
#ifdef WORDS_BIGENDIAN
    if (header.data != 2 /* ELFDATA2MSB */) return 1;
#else
    if (header.data != 1 /* ELFDATA2LSB */) return 1;
#endif
    if (sizeof(void *) == sizeof(int)) return header.class == 1; /* ELFCLASS32 */
    else return header.class == 2; /* ELFCLASS64 */
#endif
}

/***********************************************************************
 *           open_builtin_so_file
 */
static NTSTATUS open_builtin_so_file( char *name, OBJECT_ATTRIBUTES *attr, void **module,
                                      SECTION_IMAGE_INFORMATION *image_info, USHORT search_machine,
                                      USHORT load_machine, BOOL prefer_native )
{
    NTSTATUS status = STATUS_DLL_NOT_FOUND;
    int fd;
    char *end = name + strlen( name );

    if (search_machine != current_machine) return status;
    if (load_machine && load_machine != current_machine) return status;

    *module = NULL;
    strcpy( end, ".so" );
    if ((fd = open( name, O_RDONLY )) == -1) goto done;

    if (check_library_arch( fd ))
    {
        struct pe_image_info info;

        status = dlopen_dll( name, attr->ObjectName, module, &info, prefer_native );
        if (!status) virtual_fill_image_information( &info, image_info );
        else if (status != STATUS_IMAGE_ALREADY_LOADED)
        {
            ERR( "failed to load .so lib %s\n", debugstr_a(name) );
            status = STATUS_PROCEDURE_NOT_FOUND;
        }
    }
    else status = STATUS_NOT_SUPPORTED;

    close( fd );
 done:
    *end = 0;
    return status;
}

/***********************************************************************
 *           open_main_image_so_file
 */
static NTSTATUS open_main_image_so_file( const char *name, UNICODE_STRING *nt_name, void **module,
                                         SECTION_IMAGE_INFORMATION *image_info )
{
    struct pe_image_info pe_info;
    NTSTATUS status;

    /* remove .so extension from Windows name */
    if (nt_name->Length > 3 * sizeof(WCHAR))
    {
        static const WCHAR soW[] = {'.','s','o',0};
        WCHAR *p = nt_name->Buffer + nt_name->Length / sizeof(WCHAR);
        if (!wcsicmp( p - 3, soW ))
        {
            p[-3] = 0;
            nt_name->Length -= 3 * sizeof(WCHAR);
        }
    }
    status = dlopen_dll( name, nt_name, module, &pe_info, FALSE );
    if (!status) virtual_fill_image_information( &pe_info, image_info );
    return status;
}

extern NTSTATUS unwind_builtin_dll( void *args );

#else /* SO_DLLS_SUPPORTED */

static NTSTATUS open_builtin_so_file( char *name, OBJECT_ATTRIBUTES *attr, void **module,
                                      SECTION_IMAGE_INFORMATION *image_info, USHORT search_machine,
                                      USHORT load_machine, BOOL prefer_native )
{
    return STATUS_DLL_NOT_FOUND;
}

static NTSTATUS open_main_image_so_file( const char *name, UNICODE_STRING *nt_name, void **module,
                                         SECTION_IMAGE_INFORMATION *image_info )
{
    return STATUS_INVALID_IMAGE_FORMAT;
}

static NTSTATUS load_so_dll( void *args )
{
    return STATUS_INVALID_IMAGE_FORMAT;
}

static NTSTATUS unwind_builtin_dll( void *args )
{
    return STATUS_UNSUCCESSFUL;
}

#endif /* SO_DLLS_SUPPORTED */


/***********************************************************************
 *           emu_trap_thunk
 *
 * The emulator's trap callback.  Runs on the kernel stack, as all unix code
 * does, so it does no work itself: it hands the trap straight to the PE-side
 * dispatcher on the Win32 stack.  See call_emu_trap_dispatcher().
 *
 * The bridge's handler is process-global, so the PE entry point is kept here
 * rather than in the per-run parameters; it is the same function for every
 * thread and every run.
 */
static void (*p_emu_trap_dispatcher)( ULONG id, void *args, ULONG len );

/* The EC leaf path's PE entry (emu_trap_leaf, signal_ppc64.c; the class is
 * thunk_leaf_exports there).  Installed with the trap dispatcher, only
 * when EC arms, and NULL under WINE_PPC64LE_NO_EC_LEAF=1 -- the kill
 * switch, which puts every transitioned call back on the callback frame.
 * emu_trap_dispatch_common calls it before building any frame. */
static NTSTATUS (*p_emu_trap_leaf)( AMD64_CONTEXT *ctx, void *cookie );

/* WINE_PPC64LE_EC_LEAF_SABOTAGE=1, sampled at arming: a served leaf call's
 * RAX is flipped on the way back, so every leaf-served export answers
 * garbage.  The negative control that proves the leaf path is live AND
 * load-bearing: with it armed a value gate (check-ec-leaf.sh's pid and
 * last-error round trips) must go visibly wrong, and NO_EC_LEAF must
 * restore it -- which proves the kill switch reroutes rather than
 * decorates. */
static int emu_ec_leaf_sabotage;

static int  (*p_fexbridge_run_entry)( void *entry, void *arg,
                                     ULONGLONG *rax, /* same width as the
                                     bridge's unsigned long long on LP64 */
                                     char *err, unsigned int errlen );
static void (*p_fexbridge_set_trap_handler)( int (*cb)( void *thread, void *ctx, void *user ),
                                             void *user );
static int  (*p_fexbridge_process_init)(void);
static int  (*p_fexbridge_process_init32)( ULONGLONG exit_page );
static int  (*p_fexbridge_thread_init)( void **thread_out );
static void (*p_fexbridge_thread_term)( void *thread );
static void *(*p_fexbridge_current_thread)(void);
static int  (*p_fexbridge_set_gs_base)( void *thread, ULONGLONG base );
static int  (*p_fexbridge_set_fs_base)( void *thread, ULONGLONG base );
static int  (*p_fexbridge_fault_is_jit)( const void *host_ucontext );
static int  (*p_fexbridge_fault_unwind)( void *host_ucontext );
static int  (*p_fexbridge_run)( void *thread, void *ctx );
static void (*p_fexbridge_invalidate_code_range)( ULONGLONG start, ULONGLONG length );
static UINT (*p_fexbridge_hwtso_prot)(void);
static UINT (*p_fexbridge_hwtso_refused)( ULONGLONG start, ULONGLONG length );
/* ABI 5: lazy trap contexts.  Optional like the TSO pair -- an older bridge
 * simply stays eager, and emu_ctx_lazy_mask stays 0. */
static UINT (*p_fexbridge_declare_trap_ctx)( UINT lazy_mask );
static int  (*p_fexbridge_ctx_materialize)( void *thread, void *ctx, UINT flags );
/* ABI 6, optional the same way: the zero-copy trap view.  Absent means every
 * trap marshals a CONTEXT through the bridge, which is exactly the ABI 5
 * contract.  view_pull/view_push are the cold-path CONTEXT bridge for view
 * callbacks (fexbridge.h has the group semantics). */
static void (*p_fexbridge_set_trap_view_handler)( int (*cb)( void *thread, void *view, void *user ),
                                                  void *user );
static int  (*p_fexbridge_view_pull)( void *thread, void *amd64_ctx, UINT flags );
static int  (*p_fexbridge_view_push)( void *thread, const void *amd64_ctx, UINT flags );
/* ABI 7, optional: EC targets.  A registered stub RIP compiles to a direct
 * host call to the handler instead of decoding the stub's bytes; the bytes
 * stay in guest memory as the always-correct fallback.  See the 6->7
 * changelog in fexbridge.h -- especially that the transition fires BEFORE
 * the stub's `mov r10,rcx`, so arg0 is in RCX and the view's rip is the
 * stub BASE, not the trap site. */
static int  (*p_fexbridge_register_ec_target)( ULONGLONG rip,
                                               int (*handler)( void *thread, void *view, void *cookie ),
                                               void *cookie );
static int  (*p_fexbridge_register_ec_targets)( const ULONGLONG *rips, UINT count,
                                                int (*handler)( void *thread, void *view, void *cookie ),
                                                void *cookie );
static int  (*p_fexbridge_register_ec_targets2)( const ULONGLONG *rips, const void * const *cookies,
                                                 UINT count,
                                                 int (*handler)( void *thread, void *view, void *cookie ) );
static int  (*p_fexbridge_unregister_ec_range)( ULONGLONG start, ULONGLONG length );

/* fexbridge_run results and CONTEXT flags, mirrored from fexbridge.h (the
 * header is not vendored into the tree; the values are ABI). */
#define EMU_RUN_EXITED  0
#define EMU_RUN_HLT     1
#define EMU_RUN_FAULT   2
#define EMU_CTX_AMD64   0x00100000u
#define EMU_CTX_CONTROL (EMU_CTX_AMD64 | 0x1u)
#define EMU_CTX_INTEGER (EMU_CTX_AMD64 | 0x2u)
#define EMU_CTX_FLOATING_POINT (EMU_CTX_AMD64 | 0x8u)
/* ABI 5 lazy-trap markers: set in a trap CONTEXT's ContextFlags when the
 * group's bytes were NOT stored; fexbridge_ctx_materialize fills the group
 * and clears its marker.  Outside every winnt AMD64 ContextFlags bit. */
#define EMU_CTX_LAZY_EFLAGS (EMU_CTX_AMD64 | 0x100u)
#define EMU_CTX_LAZY_FLOAT  (EMU_CTX_AMD64 | 0x200u)

/* the mask fexbridge_declare_trap_ctx() actually granted; 0 = fully eager */
static UINT emu_ctx_lazy_mask;

static pthread_once_t emu_bridge_once = PTHREAD_ONCE_INIT;
static NTSTATUS emu_bridge_status;

/***********************************************************************
 *           emu_load_bridge
 *
 * Load the emulator bridge and resolve its ABI.  Runs once per process under
 * pthread_once: every guest thread needs these pointers, and resolving them
 * again per call raced with threads already using them.
 */
#define FEXBRIDGE_SONAME  "libfexbridge.so"
#define FEXBRIDGE_MIN_ABI 2

static void emu_load_bridge(void)
{
    const char *name = getenv( "WINEFEXBRIDGE" );
    char path[1024];
    const char *loaded = NULL;
    UINT (*p_abi_version)(void);
    Dl_info info;
    void *so = NULL;

    if (name && name[0])
    {
        /* An explicit override is obeyed or fails -- never silently replaced
         * by a fallback, which would run a different bridge than the one the
         * caller named. */
        if (!(so = dlopen( name, RTLD_NOW | RTLD_LOCAL )))
        {
            ERR( "cannot load emulator bridge %s (WINEFEXBRIDGE): %s\n", name, dlerror() );
            emu_bridge_status = STATUS_DLL_NOT_FOUND;
            return;
        }
        loaded = name;
    }
    /* The build installs the bridge next to this unixlib (see
     * fexbridge/build-fexbridge.sh in the port tree).  Resolve our own path
     * first, exactly as the d3d12 unixlib does for vkd3d-proton: glibc
     * expands $ORIGIN-style lookups from the load path rather than the real
     * path, so a symlinked ntdll.so must be resolved before the dirname is
     * reused.  A defaulted /tmp build artifact is what this replaces -- a
     * stale bridge there was silently wrong instead of loudly missing. */
    if (!so && dladdr( (void *)emu_load_bridge, &info ) && info.dli_fname)
    {
        const char *slash = strrchr( info.dli_fname, '/' );
        if (slash && (size_t)(slash - info.dli_fname) + sizeof(FEXBRIDGE_SONAME) + 1 < sizeof(path))
        {
            char *real;
            snprintf( path, sizeof(path), "%.*s/%s",
                      (int)(slash - info.dli_fname), info.dli_fname, FEXBRIDGE_SONAME );
            if ((real = realpath( path, NULL )))
            {
                if ((so = dlopen( real, RTLD_NOW | RTLD_LOCAL )))
                {
                    snprintf( path, sizeof(path), "%s", real );
                    loaded = path;
                }
                else WARN( "cannot load %s: %s\n", real, dlerror() );
                free( real );
            }
            else TRACE( "no %s beside ntdll.so, trying the linker path\n", FEXBRIDGE_SONAME );
        }
    }
    if (!so)
    {
        if (!(so = dlopen( FEXBRIDGE_SONAME, RTLD_NOW | RTLD_LOCAL )))
        {
            ERR( "cannot load emulator bridge %s: %s -- build and install it with "
                 "fexbridge/build-fexbridge.sh (WINEFEXBRIDGE overrides the search)\n",
                 FEXBRIDGE_SONAME, dlerror() );
            emu_bridge_status = STATUS_DLL_NOT_FOUND;
            return;
        }
        loaded = FEXBRIDGE_SONAME;
    }

    /* Refuse a wrong-ABI bridge by name, not by symptom.  An ABI-1 bridge has
     * no GS-base accessor, and under one of those every real guest dies at
     * entry with c0000005 on its first TEB read through %gs.  That symptom
     * must never again be how a stale file announces itself. */
    p_abi_version = (UINT (*)(void))dlsym( so, "fexbridge_abi_version" );
    if (!p_abi_version || p_abi_version() < FEXBRIDGE_MIN_ABI ||
        !(p_fexbridge_set_gs_base = dlsym( so, "fexbridge_set_gs_base" )))
    {
        ERR( "emulator bridge %s has ABI %u, this ntdll needs >= %u (no GS base accessor "
             "means every guest faults at entry) -- stale build?\n",
             loaded, p_abi_version ? p_abi_version() : 0, FEXBRIDGE_MIN_ABI );
        dlclose( so );
        emu_bridge_status = STATUS_DLL_INIT_FAILED;
        return;
    }
    if (!(p_fexbridge_run_entry = dlsym( so, "fexbridge_run_entry" )))
    {
        ERR( "%s has no fexbridge_run_entry: %s\n", loaded, dlerror() );
        dlclose( so );
        emu_bridge_status = STATUS_ENTRYPOINT_NOT_FOUND;
        return;
    }
    /* Guaranteed present at ABI 2, resolved individually all the same. */
    p_fexbridge_set_trap_handler = dlsym( so, "fexbridge_set_trap_handler" );
    p_fexbridge_process_init   = dlsym( so, "fexbridge_process_init" );
    p_fexbridge_thread_init    = dlsym( so, "fexbridge_thread_init" );
    p_fexbridge_thread_term    = dlsym( so, "fexbridge_thread_term" );
    p_fexbridge_current_thread = dlsym( so, "fexbridge_current_thread" );
    p_fexbridge_fault_is_jit   = dlsym( so, "fexbridge_fault_is_jit" );
    p_fexbridge_fault_unwind   = dlsym( so, "fexbridge_fault_unwind" );
    p_fexbridge_run            = dlsym( so, "fexbridge_run" );
    p_fexbridge_invalidate_code_range = dlsym( so, "fexbridge_invalidate_code_range" );
    /* optional (no ABI bump): an older bridge simply has no hardware TSO */
    p_fexbridge_hwtso_prot    = dlsym( so, "fexbridge_hwtso_prot" );
    p_fexbridge_hwtso_refused = dlsym( so, "fexbridge_hwtso_refused" );
    /* ABI 5, optional the same way: absent means every trap CONTEXT stays
     * eagerly complete, which is exactly the old contract */
    p_fexbridge_declare_trap_ctx = dlsym( so, "fexbridge_declare_trap_ctx" );
    p_fexbridge_ctx_materialize  = dlsym( so, "fexbridge_ctx_materialize" );
    /* ABI 6, optional: an older bridge simply has no view protocol and every
     * trap keeps marshalling a CONTEXT.  The minimum-ABI check above is a
     * floor, not a pin, so an ABI 6 bridge passes it unchanged. */
    p_fexbridge_set_trap_view_handler = dlsym( so, "fexbridge_set_trap_view_handler" );
    p_fexbridge_view_pull             = dlsym( so, "fexbridge_view_pull" );
    p_fexbridge_view_push             = dlsym( so, "fexbridge_view_push" );
    /* ABI 7, optional the same way: without these every stub keeps trapping. */
    p_fexbridge_register_ec_target    = dlsym( so, "fexbridge_register_ec_target" );
    p_fexbridge_register_ec_targets   = dlsym( so, "fexbridge_register_ec_targets" );
    p_fexbridge_register_ec_targets2  = dlsym( so, "fexbridge_register_ec_targets2" );
    p_fexbridge_unregister_ec_range   = dlsym( so, "fexbridge_unregister_ec_range" );
    /* ABI 4: the 32-bit (WoW64) lane.  Resolved unconditionally like the
     * rest; their absence is diagnosed where the 32-bit lane starts, not
     * here, so a 64-bit-only bridge keeps serving the AMD64 lane exactly as
     * before. */
    p_fexbridge_process_init32 = dlsym( so, "fexbridge_process_init32" );
    p_fexbridge_set_fs_base    = dlsym( so, "fexbridge_set_fs_base" );
    TRACE( "loaded emulator bridge %s, ABI %u\n", loaded, p_abi_version() );
}


/* Every per-thread variable on the trap path is initial-exec, and the choice
 * is measured, not stylistic: the default global-dynamic model costs one
 * __tls_get_addr_opt call per access, and emu_trap_thunk() touches several of
 * these on EVERY crossing -- __tls_get_addr_opt was 3.6% of the GameThread in
 * the 2026-08-27 profile.  Space-wise the model is free: this module's PT_TLS
 * is already static-allocated at dlopen because ppc64_unix_teb is initial-exec
 * (one IE variable commits the whole block), so the attribute changes only the
 * access sequence.  See thread_data_cache in unix_private.h for the same
 * reasoning and the static-TLS-surplus caveat that DOES bound the block's
 * size. */
#define EMU_THREAD_VAR __thread __attribute__((tls_model("initial-exec")))

/* set when the PE dispatcher ended the run because the guest called
 * ExitThread, so that a deliberate exit is not reported as an emulator
 * failure.  Per thread: it describes this thread's run, nothing else. */
static EMU_THREAD_VAR BOOL emu_thread_exit_requested;

/* set when the PE dispatcher ended the run because a guest exception went
 * unhandled at guest level (RaiseException with no consuming handler); the
 * record itself waits PE-side.  Same per-thread protocol as the exit flag. */
static EMU_THREAD_VAR BOOL emu_thread_exc_pending;

/* WINEEMUNOFAULTSTASH=1: skip the fault-record stash, the S2 negative
 * control -- the dispatched record must visibly degrade to code-only,
 * proving the stash is load-bearing.  Read once outside signal context. */
static int emu_no_fault_stash = -1;

/* WINEEMUNOSTACKSIZE=1: size every guest stack from the image and ignore what
 * the thread itself asked for -- the negative control for the per-thread guest
 * stack size, and exactly what this file did before it had one.  Read once,
 * outside signal context, like the flag above. */
static int emu_no_stack_size = -1;

/* the innermost run's guest stack bounds on this thread (base > limit, the
 * stack grows down); guest TEB frames are validated against these */
static EMU_THREAD_VAR void *emu_guest_stack_base;
static EMU_THREAD_VAR void *emu_guest_stack_limit;

/* Non-zero for exactly the extent of the p_fexbridge_run() calls below: a
 * fault landing while it is set happened while the bridge was executing guest
 * code, even if the fault's own host program counter cannot itself be
 * recognized as JIT-owned (see the NULL-guest-call comment on
 * emu_handle_fault).  Per-thread, like every other run_loop flag here.
 *
 * A DEPTH, not a flag, and that is the whole of what this variable has to get
 * right.  Runs NEST: a guest trap dispatches into native code, that native
 * code calls a guest callback, and call_guest_function comes back through
 * unix_emu_run_entry into a second emu_run_loop on the same thread -- which
 * the surrounding code already knows, since it stacks the guest stack bounds
 * and the TEB stack description around exactly that.  A boolean cleared on
 * the inner run's exit would say "no guest code is running" while the OUTER
 * run is still executing, and the null-call recovery below would go quietly
 * missing for the rest of that outer run: the process would die natively
 * again, and only for programs that use callbacks -- the ones most likely to
 * have a __try around the call in the first place. */
static EMU_THREAD_VAR UINT emu_run_depth;

/* Windows reserves the low 64K of every process's address space precisely so
 * that a dereference of a null-derived pointer faults rather than reading
 * something; a branch to any address inside it is the null call this port has
 * to turn back into an exception the guest can handle. */
#define EMU_NULL_REGION_SIZE  0x10000


/***********************************************************************
 *           the guest register file, for a debugger
 *
 * A debugger looking at this process sees native ppc64 threads whose CONTEXT
 * is the emulator's, and the guest register file it wants is the emulator's
 * private property.  Every piece of it this port needs, it already
 * reconstructs -- the run loop's own AMD64_CONTEXT below, the one the bridge
 * fills at every trap, and the one it fills at every fault -- but all three
 * live in a stack frame that is gone by the time anybody outside asks.  So
 * the last one is COPIED here, where it outlives the run, and served through
 * NtQueryInformationThread(ThreadWow64Context) like any other machine's
 * context (see get_thread_wow64_context() in unix/signal_ppc64.c).
 *
 * THE STATE MATTERS AS MUCH AS THE REGISTERS.  A snapshot is only worth
 * anything if the reader can tell whether it is where the guest IS or where
 * the guest last WAS:
 *
 *   EMU_GUEST_TRAP   the guest called out (an import thunk, a COM slot) and
 *                    native code is running on its behalf.  The bridge handed
 *                    us the register file at the trapping instruction; it is
 *                    exact, and it stays exact for as long as the native call
 *                    takes -- which for a thread blocked in a Wine API is the
 *                    whole time a debugger is likely to look.
 *   EMU_GUEST_FAULT  the guest faulted.  Exact, at the faulting instruction,
 *                    and this is the state a crash is read from.
 *   EMU_GUEST_ENDED  the run returned or exited.  Exact, at the HLT.
 *   EMU_GUEST_RUNNING  the bridge is executing guest code right now.  The
 *                    registers are inside the JIT and the copy below is where
 *                    this run last resumed from, which is NOT where the guest
 *                    is.  Reported as "no context" rather than as registers,
 *                    because a debugger printing a stale RIP as the current
 *                    one is a wrong number, and a wrong number is worse than
 *                    a refusal -- the same rule the FP marshalling gate is
 *                    built on.  There is no way to do better without a bridge
 *                    entry point that reads the register file out of a
 *                    running thread; fexbridge_fault_unwind() reconstructs it
 *                    but ENDS the run doing so, which is not something a mere
 *                    suspend may do.
 *
 * Nested runs (a guest callback entered from inside a trap) save and restore
 * this the way they save every other per-run variable here, so what is
 * published is always the innermost live run.  The one exception is a run
 * that ended on an unhandled guest exception: that state is deliberately left
 * standing, because the thread is on its way to reporting it and the report
 * is the only reason anybody is looking.
 */
enum emu_guest_state
{
    EMU_GUEST_NONE = 0,
    EMU_GUEST_RUNNING,
    EMU_GUEST_TRAP,
    EMU_GUEST_FAULT,
    EMU_GUEST_ENDED,
};

/* WINEEMUNODBGCTX=1: publish nothing, which is exactly the state this file was
 * in before a debugger could see a guest at all.  The negative control for
 * ppc64le/winedbg/check-guest-debug.sh; read once, outside signal context,
 * like every other lever here. */
static int emu_no_dbg_ctx = -1;

/* WINEEMUNODBGSTACK=1: free the guest stack of a thread that died on an
 * unhandled guest exception even when a debugger is attached, which is what
 * this file did before.  The debugger then has a valid guest RSP pointing at
 * unmapped memory -- registers without a stack.  The second negative control
 * for ppc64le/winedbg/check-guest-debug.sh. */
static int emu_no_dbg_stack = -1;

/* The state alone -- the registers already published are still this thread's,
 * only the claim about how exact they are has changed.
 *
 * emu_no_dbg_ctx still being -1 means no context has ever been published on
 * this thread, so there are no registers for a state to describe and writing
 * one would say "the guest is stopped here" about a zeroed block.  Every
 * caller publishes a context first and this is unreachable; it is written down
 * because the alternative to being unreachable is being wrong. */
static void emu_publish_guest_state( int state )
{
    struct thread_data *data = get_thread_data();

    if (emu_no_dbg_ctx != 0 || !data) return;
    data->emu_guest_ctx_state = state;
}

static void __attribute__((noinline)) emu_no_dbg_ctx_init(void)
{
    const char *str = getenv( "WINEEMUNODBGCTX" );
    emu_no_dbg_ctx = (str && *str == '1');
    if (emu_no_dbg_ctx)
        ERR( "WINEEMUNODBGCTX: the guest register file will not be published; "
             "a debugger attaching to this process sees the emulator's registers only\n" );
}

/* the hot path is two stores; the once-per-process env check and its ERR
 * live out of line so this inlines into the crossing */
static FORCEINLINE void emu_publish_guest_context( const AMD64_CONTEXT *ctx, int state )
{
    struct thread_data *data = get_thread_data();

    if (__builtin_expect( emu_no_dbg_ctx == -1, 0 )) emu_no_dbg_ctx_init();
    if (emu_no_dbg_ctx || !data) return;
    if (state == EMU_GUEST_TRAP)
    {
        /* The hot path: one crossing per guest API call, ~1.3M/s in the
         * Cyberpunk flythrough.  The frame `ctx` names is the bridge's and is
         * live for exactly as long as this state stands -- emu_trap_thunk()
         * publishes RUNNING before its frame can die -- so the reader
         * (emu_get_guest_context, same thread, from a signal) copies from the
         * pointer instead of this path copying 1232 bytes per trap.
         *
         * Store order is the correctness: the pointer must be in place before
         * the state that licenses reading it, and the fence stops the compiler
         * from reordering the two stores across a signal's arrival point.
         * Same-thread signal delivery makes CPU order a non-issue. */
        data->emu_guest_ctx_ptr = ctx;
        __atomic_signal_fence( __ATOMIC_SEQ_CST );
        data->emu_guest_ctx_state = state;
        return;
    }
    /* The cold states copy, and in copy-then-state order for the same signal
     * window: a reader that interrupts the memcpy still sees the OLD state
     * describing the OLD (complete) content, never a fresh label on a
     * half-written block. */
    if (ctx) data->emu_guest_ctx = *ctx;
    __atomic_signal_fence( __ATOMIC_SEQ_CST );
    data->emu_guest_ctx_state = state;
}

/***********************************************************************
 *           emu_ctx_materialize_full
 *
 * Fill a lazy trap CONTEXT's skipped EFLAGS/FP groups from the live guest
 * state (bridge ABI 5).  One call, both groups: every caller is a cold path
 * (a debugger read, an exception, a fiber, an FP-typed thunk) where the cost
 * that matters is the crossing already being paid, not the group split.
 *
 * Idempotent and cheap when there is nothing to do -- the bridge checks the
 * markers -- so callers invoke it unconditionally.  The thread handle is the
 * calling thread's own: a trap CONTEXT only ever surfaces on the thread that
 * trapped, and nested traps publish the innermost frame, whose spill is the
 * one the bridge reads.
 */
/* WINEEMUNOCTXMAT=1: every materialize call becomes a no-op, which is the
 * consumer side FORGETTING the contract on purpose.  With the bridge's
 * FEXBRIDGE_CTX_POISON=1 armed, an FP-typed call then computes with the
 * poison pattern and a value-checking gate goes red -- the pair is the
 * negative control that proves the consumer audit is load-bearing.  Read
 * once, outside signal context, like every other lever here. */
static int emu_no_ctx_mat = -1;

static void emu_ctx_materialize_full( AMD64_CONTEXT *ctx )
{
    void *thread;

    if (!emu_ctx_lazy_mask || !ctx) return;
    if (emu_no_ctx_mat == -1)
    {
        const char *str = getenv( "WINEEMUNOCTXMAT" );
        emu_no_ctx_mat = (str && *str == '1');
        if (emu_no_ctx_mat)
            ERR( "WINEEMUNOCTXMAT: lazy trap contexts will NOT be materialized; "
                 "every EFLAGS/FP consumer reads unfilled state\n" );
    }
    if (emu_no_ctx_mat) return;
    if (!p_fexbridge_current_thread || !(thread = p_fexbridge_current_thread())) return;
    p_fexbridge_ctx_materialize( thread, ctx, EMU_CTX_CONTROL | EMU_CTX_FLOATING_POINT );
}

static NTSTATUS unixcall_emu_ctx_materialize( void *args )
{
    emu_ctx_materialize_full( args );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           emu_get_guest_context
 *
 * The calling thread's guest register file, if it has one that is exact.
 *
 * Called from context_to_server() on the thread's own behalf -- always the
 * thread itself, never a peer, because the server's context machinery works
 * by asking a thread for its own registers (a SIGUSR1 into wait_suspend).
 * That is what makes this safe to read without a lock: the only writer is
 * this thread, and the only reader is this thread.
 */
BOOL emu_get_guest_context( AMD64_CONTEXT *ctx )
{
    struct thread_data *data = get_thread_data();

    if (!data) return FALSE;
    switch (data->emu_guest_ctx_state)
    {
    case EMU_GUEST_TRAP:
        /* published as a pointer to the bridge's live trap frame; see
         * emu_publish_guest_context() for why the frame is guaranteed live
         * for as long as this state stands.  Under the lazy declaration the
         * frame may still be missing its EFLAGS/FP bytes -- a debugger wants
         * all of them, so materialize first.  The cast un-consts the live
         * frame for exactly that: filling skipped groups in place is the
         * bridge's documented mechanism, not a mutation of guest state. */
        if (!data->emu_guest_ctx_ptr) return FALSE;
        emu_ctx_materialize_full( (AMD64_CONTEXT *)data->emu_guest_ctx_ptr );
        *ctx = *data->emu_guest_ctx_ptr;
        goto synth_segments;
    case EMU_GUEST_FAULT:
    case EMU_GUEST_ENDED:
        *ctx = data->emu_guest_ctx;
    synth_segments:
        /* The segment selectors, SYNTHESISED rather than reported, and that
         * distinction is the point: the bridge's CONTEXT does not carry them
         * because guest code never changes them, so there is nothing to copy
         * and these are the fixed values every user-mode x86-64 Windows thread
         * has.  (The GS *base* is real and is this thread's TEB -- the run
         * installs it; it is the selector that is a constant, because on x86-64
         * the base comes from an MSR and not from the descriptor.)
         *
         * CS is load-bearing beyond being right.  A context block this port
         * never filled arrives as all zeros, and CS 0 is a value no user-mode
         * x86-64 thread can have -- so CS is how a reader tells "no guest
         * context" from "a guest context that happens to be zero", which is
         * the difference between a refusal and sixteen invented registers. */
        ctx->SegCs = 0x33;
        ctx->SegSs = 0x2b;
        ctx->SegDs = 0x2b;
        ctx->SegEs = 0x2b;
        ctx->SegFs = 0x53;
        ctx->SegGs = 0x2b;
        return TRUE;
    default:
        return FALSE;
    }
}


/***********************************************************************
 *           the TEB's stack description across the machine boundary
 *
 * A thread that runs guest code has TWO stacks: the native ppc64 one Wine
 * gave it at creation, and the guest stack emu_run_loop() allocates below.
 * There is one TEB and it is shared -- the emulator's GS base is this
 * thread's own TEB, see unixcall_emu_run_entry() -- so NtTib.StackBase and
 * NtTib.StackLimit can only describe one stack at a time, and both readers
 * are real code that this port has to be right for:
 *
 *   GUEST code reads gs:[0x08] and gs:[0x10] directly.  Every function MSVC
 *   compiles with a frame larger than a page begins with a call to __chkstk,
 *   which loads gs:[0x10] and, if the new RSP is below it, touches one byte
 *   per page from there downwards.  With the NATIVE bounds in the TEB and a
 *   guest stack that happens to lie below them, the very first byte __chkstk
 *   touches is the native stack's guard page.  Measured, not deduced: DOOM
 *   (2016) died in steam_api64.dll's CRT startup with c00000fd whose
 *   ExceptionInformation[1] was exactly the native StackLimit minus one page,
 *   raised from the `movb $0, (%r11)` at the top of that module's __chkstk.
 *   It had been read as Steam DRM declining to run without a Steam client.
 *
 *   the HOST's fault handler reads them through is_inside_thread_stack() to
 *   decide whether a guard-page hit is a stack that should grow.  With the
 *   native bounds in the TEB the guest stack cannot grow past its initial
 *   commit at all: its guard page is not inside "the" thread stack, so it
 *   becomes STATUS_GUARD_PAGE_VIOLATION and the guest dies at one megabyte.
 *
 * So the TEB describes whichever machine is currently executing: the guest
 * stack strictly inside fexbridge_run(), the native stack everywhere else --
 * including inside a trap, where native Wine code runs on the native stack
 * and Wine's own SEH validates its frames against these same two fields.
 *
 * The switch reads the outgoing values back rather than assuming them,
 * because grow_thread_stack() moves NtTib.StackLimit down as a stack grows:
 * what has to be reinstalled on the next run is the grown limit, not the one
 * the stack was allocated with.
 *
 * One thing is deliberately NOT restored to the pre-port behaviour: while a
 * run is active, a guard-page hit on the NATIVE stack is now classified as a
 * guard-page violation rather than as a stack overflow, because the TEB no
 * longer describes it.  That case is the emulator's own JIT overflowing
 * Wine's stack, it is fatal either way, and both spellings reach the same
 * emu_handle_fault() arm -- so it costs a status code on a path that is a
 * bug in the port, in exchange for the guest's stack behaving like a
 * Windows thread stack on every path that is not.
 */
struct emu_teb_stack
{
    void *base;      /* NtTib.StackBase   -- highest address, exclusive */
    void *limit;     /* NtTib.StackLimit  -- lowest committed address */
    void *dealloc;   /* DeallocationStack -- lowest reserved address */
};

/* the native stack of this thread.  Captured once: init_thread_stack() sets
 * it at thread creation and nothing moves it afterwards. */
static EMU_THREAD_VAR struct emu_teb_stack emu_native_teb_stack;

/* the innermost run's guest stack as the TEB last knew it */
static EMU_THREAD_VAR struct emu_teb_stack emu_guest_teb_stack;

static void emu_capture_native_teb_stack(void)
{
    TEB *teb = NtCurrentTeb();

    if (emu_native_teb_stack.base) return;
    emu_native_teb_stack.base    = teb->Tib.StackBase;
    emu_native_teb_stack.limit   = teb->Tib.StackLimit;
    emu_native_teb_stack.dealloc = teb->DeallocationStack;
}

/* The install half, ONE spelling shared by the hot per-crossing sites (which
 * already hold the TEB through thread_data) and the cold run-entry switch
 * below.  Inline because the out-of-line pair sat at 5.4% of the bench
 * crossing [MEASURED 2026-09-01]: two calls per trap, each re-deriving
 * NtCurrentTeb() to do three loads or stores. */
static inline void emu_teb_stack_install( TEB *teb, const struct emu_teb_stack *in )
{
    if (!in->base)
    {
        /* Only reachable if a trap fired on a thread that never entered
         * unixcall_emu_run_entry(), which is the one place a guest run can
         * start.  Refuse rather than write zeroes into the TEB's stack
         * description, which would make every later fault unclassifiable. */
        ERR( "no stack description to install in the TEB; leaving it alone\n" );
        return;
    }
    teb->Tib.StackBase     = in->base;
    teb->Tib.StackLimit    = in->limit;
    teb->DeallocationStack = in->dealloc;
}

/* install `in`, reading what was there into `out` (which may be NULL) */
static void emu_teb_stack_switch( const struct emu_teb_stack *in, struct emu_teb_stack *out )
{
    TEB *teb = NtCurrentTeb();

    if (out)
    {
        out->base    = teb->Tib.StackBase;
        out->limit   = teb->Tib.StackLimit;
        out->dealloc = teb->DeallocationStack;
    }
    emu_teb_stack_install( teb, in );
}

/* FORCEINLINE, and emu_view_dispatch below too: these are two of the four C
 * frames between the bridge's trampoline and call_user_mode_callback, each
 * paying a prologue, an epilogue and an argument shuffle per crossing.
 * Inlined into the two bridge-facing thunks they cost nothing but code
 * size.  [MEASURED] op4k 2026-09-04: 8.8% + 11.6% of a crossing's samples
 * sat in the two of them as separate symbols. */
static FORCEINLINE NTSTATUS emu_trap_dispatch_common( AMD64_CONTEXT *ctx, void *cookie, BOOL ec )
{
    struct thread_data *data = get_thread_data();
    NTSTATUS status;

    /* Crossing log: this is the guest->native direction, pushed BEFORE
     * call_emu_trap_dispatcher's own kernel-stack check runs, so the stack's
     * innermost entry is always the crossing about to be checked --
     * including the one that fails it.  Popped as soon as the dispatcher
     * returns, a few lines down, because that is exactly when this crossing
     * is no longer open.  See emu_crossing_push() in unix_private.h. */
    emu_crossing_push( data, EMU_CROSSING_TRAP, ctx->Rip );

    /* The register file in `ctx` IS the guest's, at the trapping instruction
     * -- the whole marshalling layer is built on that.  Under the CONTEXT
     * protocol it is the bridge's own trap frame; under the view protocol it
     * is emu_trap_view_thunk's shell, filled from the live file moments ago.
     * Both are live for exactly as long as the TRAP state stands.  Publish it
     * for the duration of the native call, so a debugger looking at a thread
     * that is inside a Wine API sees where the guest is rather than where the
     * emulator is.  Nothing is saved here: a nested run started from inside
     * this trap saves and restores it for itself, and the moment this returns
     * the guest is executing again -- which is what the second publication
     * below says, because a TRAP state left standing over running guest code
     * would name the last trap's RIP as the current one. */
    emu_publish_guest_context( ctx, EMU_GUEST_TRAP );

    /* The EC leaf path, tried FIRST and before any frame exists: a
     * transitioned call whose cell names a leaf export (thunk_leaf_exports,
     * signal_ppc64.c) is served by an ordinary call into PE code on this
     * very stack -- no callback frame, no 48-register save, no Win32-stack
     * switch, no PE dispatcher frame, no lean return.  The crossing log and
     * the TRAP publication above still cover it: a mid-call suspend reads
     * the same shell the full path would publish, and a fault inside the
     * callee (a bug, by the class's definition) is reported against this
     * crossing.  A decline touches nothing, so the full path below runs
     * exactly as before -- and a non-leaf row declines HERE, on the
     * cell's state word (EMU_EC_CELL_LEAF), before any call into PE code:
     * the call-and-decline was +14 ns on every COM slot [MEASURED, the
     * unixlib.h note].  Only the EC thunk has a cookie, so the CONTEXT and
     * view trap protocols never get here with ec set. */
    if (ec && cookie && p_emu_trap_leaf && *(const volatile LONG *)cookie == EMU_EC_CELL_LEAF &&
        (status = p_emu_trap_leaf( ctx, cookie )) != EMU_LEAF_DECLINED)
    {
        if (__builtin_expect( emu_ec_leaf_sabotage, 0 )) ctx->Rax = ~ctx->Rax;
        emu_crossing_pop( data );
        goto served;
    }

    /* everything below this line is native code on the native stack */
    emu_teb_stack_install( data->teb, &emu_native_teb_stack );
    status = call_emu_trap_dispatcher_inline( data, p_emu_trap_dispatcher, ctx, cookie );
    emu_crossing_pop( data );
    /* ...and back to the GUEST stack the run is on NOW, which is not always
     * the one this trap arrived on: a guest fiber switch happens inside the
     * trap and moves the run to another stack (unixcall_emu_fiber_stack).
     * Restoring the snapshot taken above would put the TEB back on the
     * stack the switch just left, and the resumed fiber would run on its own
     * stack while being told it is on the other one -- silent until its
     * first deep frame or its first exception.  emu_guest_teb_stack is the
     * run's own record and a switch updates it, so it is the one to install;
     * with no switch it holds exactly what the snapshot does. */
    emu_teb_stack_install( data->teb, &emu_guest_teb_stack );

served:
    emu_publish_guest_state( EMU_GUEST_RUNNING );

    if (status == STATUS_THREAD_IS_TERMINATING) emu_thread_exit_requested = TRUE;
    else if (status == STATUS_EMU_GUEST_EXCEPTION) emu_thread_exc_pending = TRUE;
    else if (status)
    {
        /* A status the run loop has no flag for.  It ends the run all the
         * same, and the run loop's "no consuming handler" message cannot say
         * why because by then the status is gone -- so it is said HERE, where
         * it is still in hand.  The one that reaches this in practice is
         * STATUS_STACK_OVERFLOW out of call_emu_trap_dispatcher, which is
         * crossing depth; see the comment there. */
        ERR( "the guest trap dispatcher could not be entered, status %08x; "
             "the run will end with no handler having consumed the trap\n",
             (UINT)status );
    }
    return status;
}

static int emu_trap_thunk( void *thread, void *ctx, void *user )
{
    /* FEXBRIDGE_TRAP_CONTINUE / _EXIT */
    return emu_trap_dispatch_common( ctx, NULL, FALSE ) ? 1 : 0;
}

/* mirror of FEXBRIDGE_TRAP_VIEW (fexbridge.h, bridge ABI 6; the header is
 * not vendored, the layout is ABI): pointers into the live guest register
 * file, valid only until the callback returns.  gregs holds the 16 GPRs in
 * x86 encoding order -- RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15. */
struct emu_trap_view
{
    UINT64 *gregs;
    UINT64 *rip;
    UINT    reserved[4];
};

/* The shell copies in emu_trap_view_thunk depend on the AMD64 CONTEXT's GPR
 * block being the very array shape the view exposes: Rax..R15 contiguous in
 * x86 encoding order, Rip directly after R15.  Pinned so a header change
 * breaks the build, not the guest. */
C_ASSERT( offsetof(AMD64_CONTEXT, Rax) == 0x78 );
C_ASSERT( offsetof(AMD64_CONTEXT, Rcx) == 0x80 );
C_ASSERT( offsetof(AMD64_CONTEXT, Rdx) == 0x88 );
C_ASSERT( offsetof(AMD64_CONTEXT, Rsp) == 0x78 + 4 * 8 );
C_ASSERT( offsetof(AMD64_CONTEXT, R8)  == 0x78 + 8 * 8 );
C_ASSERT( offsetof(AMD64_CONTEXT, R15) == 0x78 + 15 * 8 );
C_ASSERT( offsetof(AMD64_CONTEXT, Rip) == 0xf8 );

/* FEXBRIDGE_CTX_POISON=1, sampled once at view registration (outside signal
 * context): the bridge cannot poison a CONTEXT it never builds, so the view
 * thunk carries the negative control itself -- the same patterns the bridge
 * writes, so the check-lazy-ctx sabotage stays byte-exact on this protocol. */
static int emu_trap_view_poison;

/* WINE_PPC64LE_EC_SABOTAGE=1, sampled at arming (outside signal context):
 * the EC handler stops simulating the stub's `mov r10,rcx`, so every
 * transitioned call reads garbage for argument 0.  The negative control
 * that proves the simulation is load-bearing: with it armed, a value gate
 * (winepath's output) must go visibly wrong, and lifting the lever must
 * restore it. */
static int emu_ec_sabotage;

/* One body for both the ABI 6 view trap and the ABI 7 EC transition.  The
 * only difference is WHERE the guest stopped: a trap fired at stub+3 with
 * the stub's `mov r10,rcx` already executed; an EC transition fires at the
 * stub BASE with nothing executed.  `ec` makes this body perform the two
 * instructions the guest never ran -- the rescue and the advance to the
 * trap site -- so everything downstream (the dispatcher's whole RIP-keyed
 * world, the stats rows, the publish) sees the trap it has always seen.
 * The +3 is safe because registration is byte-verified: only stubs whose
 * five bytes are exactly `49 89 ca 0f 05` are ever registered. */
/* The gregs copies are 128 bytes each way on EVERY crossing.  Spelled as
 * memcpy they are a PLT call into libc under this tree's -fno-builtin
 * ([MEASURED] 9.4% of a crossing on op4k, 2026-09-03, perf callchain: all
 * of memcpy's samples came from this function); spelled out they are
 * sixteen doubleword moves the compiler schedules inline. */
static inline void copy_gregs( UINT64 *dst, const UINT64 *src )
{
    unsigned int i;
#pragma GCC unroll 16
    for (i = 0; i < 16; i++) dst[i] = src[i];
}

static FORCEINLINE int emu_view_dispatch( void *thread, struct emu_trap_view *view, BOOL ec, void *cookie )
{
    AMD64_CONTEXT shell;    /* deliberately NOT zeroed: only the groups named
                             * by ContextFlags carry values, which is exactly
                             * the claim an ABI 5 lazy trap CONTEXT makes --
                             * its EFLAGS/FP bytes are unfilled too, and the
                             * poison lever below is what catches a consumer
                             * that reads them without materializing. */
    UINT push_flags = 0;
    NTSTATUS status;

    /* One shape, one copy: the C_ASSERT chain above pins &shell.Rax as a
     * UINT64[16] in view->gregs order. */
    copy_gregs( &shell.Rax, view->gregs );
    shell.Rip = *view->rip;
    if (ec)
    {
        if (!emu_ec_sabotage) shell.R10 = shell.Rcx;  /* the mov r10,rcx */
        shell.Rip += 3;                               /* the trap site   */
    }
    /* Dr0-Dr7 are zeroed even though no lazy group names them: the bridge's
     * CONTEXT protocol handed out a zero-initialized frame, so a debugger --
     * or a DRM reading CONTEXT_DEBUG_REGISTERS to look for hardware
     * breakpoints -- has always seen zeros here.  Stack garbage in Dr7 reads
     * as armed breakpoints; six stores keep the observable contract. */
    shell.Dr0 = shell.Dr1 = shell.Dr2 = shell.Dr3 = shell.Dr6 = shell.Dr7 = 0;
    shell.ContextFlags = EMU_CTX_CONTROL | EMU_CTX_INTEGER |
                         EMU_CTX_LAZY_EFLAGS | EMU_CTX_LAZY_FLOAT;
    shell.P2Home = 0;  /* the bridge's materialize parks its EFLAGS baseline
                        * here; unused on this path, kept deterministic */
    if (emu_trap_view_poison)
    {
        shell.EFlags = 0xDEADF1A6u;
        shell.MxCsr  = 0xDEADF1A6u;
        memset( &shell.FltSave, 0xDD, sizeof(shell.FltSave) );
    }

    status = emu_trap_dispatch_common( &shell, cookie, ec );

    /* Write-back, unconditional and on EVERY status: under the CONTEXT
     * protocol the bridge's after-trap load applies the callback's CONTEXT
     * whether the trap continued or ended the run, and this is that load's
     * equivalent.  It is also what keeps NESTED runs correct with no new
     * contract: a guest callback entered from inside this trap re-enters
     * fexbridge_run, whose Nested block restores the raw flag forms and the
     * whole FP file but NOT the GPRs/RIP the nested guest clobbered in
     * CPUState -- under the CONTEXT protocol the outer values sat in the
     * bridge's trap frame and its resume reinstated them; here they sit in
     * this shell, and these stores are that reinstatement.  (The nested run
     * itself went through unixcall_emu_run_entry, which saves and restores
     * the published-context state around it; the shell is untouched by the
     * nested run by construction -- it lives on this thunk's stack.) */
    copy_gregs( view->gregs, &shell.Rax );
    *view->rip = shell.Rip;

    /* EFLAGS/FP write-back only when the group was materialized -- the ABI 5
     * rule ("writes to an unmaterialized group are IGNORED at resume")
     * restated for the view.  A cleared lazy marker is the materialize
     * receipt: fexbridge_ctx_materialize (and view_pull) clear 0x100/0x200
     * as they fill.  Unmaterialized groups were never copied anywhere, so
     * the live state is already the truth. */
    if (!(shell.ContextFlags & 0x100u)) push_flags |= EMU_CTX_CONTROL;
    if (!(shell.ContextFlags & 0x200u)) push_flags |= EMU_CTX_FLOATING_POINT;
    if (push_flags) p_fexbridge_view_push( thread, &shell, push_flags );

    /* FEXBRIDGE_TRAP_CONTINUE / _EXIT */
    return status ? 1 : 0;
}

static int emu_trap_view_thunk( void *thread, void *view_ptr, void *user )
{
    return emu_view_dispatch( thread, view_ptr, FALSE, NULL );
}

/* The ABI 7 EC transition handler.  The gregs write-back in the shared body
 * covers R10 too, which is exactly what the real stub would have left
 * behind; RCX keeps the argument it held, which the real path also permits
 * (SYSCALL architecturally clobbers RCX, so no MS-x64 caller reads it). */
static int emu_ec_thunk( void *thread, void *view_ptr, void *cookie )
{
    /* the cookie is this stub's own PE-side row cell (registered per rip by
     * unixcall_emu_register_ec below); the PE dispatcher serves the row from
     * it without re-resolving the RIP.  NULL when the module armed without
     * cells, which only costs the lookup it always cost. */
    return emu_view_dispatch( thread, view_ptr, TRUE, cookie );
}


/***********************************************************************
 *           emu_handle_fault
 *
 * A fault taken inside the emulator's JIT belongs to the guest, not to us.
 *
 * Nothing else can tell the difference.  The JIT is unix code and runs on the
 * kernel stack, so a guest fault looks exactly like a fault taken inside a
 * syscall, and handle_syscall_fault swallows it into a bare c0000005 with no
 * guest state at all -- which is why a guest crash used to report nothing
 * useful and had to be found by disassembling the guest by hand.
 *
 * The bridge reconstructs the guest register file from the host fault context
 * and unwinds to the innermost fexbridge_run, which returns RUN_FAULT with
 * Rip at the faulting guest instruction.  fexbridge_fault_unwind does not
 * return when it succeeds.  Called from a signal handler: it touches only
 * function pointers resolved once at bridge load.
 *
 * One case fexbridge_fault_is_jit necessarily gets wrong: a guest CALL
 * through a NULL (or otherwise garbage, low) function pointer lands the
 * host program counter itself at that address, so the fault's NIP reads 0
 * (or near it) rather than anywhere inside the JIT's owned code range --
 * fault_is_jit can only answer "is NIP in JIT code", and the honest answer
 * to that question is no, even though the branch that produced this fault
 * was JIT-emitted guest code a moment earlier and the GPR file the ucontext
 * carries is still the guest's.  Left alone, this used to fall through to
 * handle_syscall_fault and then a native, unattributed process death: the
 * guest never saw an EXCEPTION_RECORD, so a __try/__except (or a vectored
 * handler) around the call could not do what it would on real Windows.
 * Recognized narrowly -- this thread is inside an active p_fexbridge_run()
 * call (emu_run_in_progress) and the fault's address is the null page --
 * and unwound anyway: fexbridge_fault_unwind reconstructs from the GPR
 * file, not from NIP, so it does not need fault_is_jit's blessing to work
 * here. */
BOOL emu_handle_fault( void *sigcontext, EXCEPTION_RECORD *rec )
{
    struct thread_data *data = get_thread_data();
    BOOL is_jit;

    if (!p_fexbridge_fault_is_jit || !p_fexbridge_fault_unwind) return FALSE;
    is_jit = p_fexbridge_fault_is_jit( sigcontext );
    if (!is_jit)
    {
        if (!emu_run_depth || !rec || rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
            (ULONG_PTR)rec->ExceptionAddress >= EMU_NULL_REGION_SIZE)
            return FALSE;

        /* THE WHOLE NULL REGION, not the single address zero.  rec's
         * ExceptionAddress is the faulting NIP (segv_handler builds it from
         * NIP_sig), so this test says "the host program counter itself landed
         * in the null region" -- which native ppc64 code never does, and which
         * only a branch through a bad pointer produces.  Exactly zero is the
         * least common form of it: what a guest actually does is call through
         * a null-derived pointer WITH AN OFFSET -- `object->vtbl->Method()`
         * with a null vtbl lands at the method's slot offset, `p->fn()` with a
         * null p lands at the member's offset -- so the PC is a small number
         * rather than 0.  Windows reserves the whole first 64K for this reason
         * and reports every one of them as the same access violation; testing
         * only for 0 recovered the rarest case and let the common one kill the
         * process, which is the opposite of the intent this code was written
         * with.
         *
         * A call through a null pointer faults FETCHING the instruction, not
         * reading data: on real Windows (DEP) that is an ACCESS_VIOLATION
         * whose first slot says execute, and the seed gate's sabotage leg
         * documents exactly that shape -- the address is the target itself,
         * already sitting in rec->ExceptionAddress. */
        rec->NumberParameters = 2;
        rec->ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
        rec->ExceptionInformation[1] = (ULONG_PTR)rec->ExceptionAddress;  /* the bad target */
    }

    /* Stash the record: fexbridge_fault_unwind longjmps the fault away, so
     * this copy is the only thing that survives into the RUN_FAULT arm of
     * the run loop.  A write, not a reorder -- fault legibility unchanged.
     * (A guest jump to unfetchable memory never gets here at all: the bridge
     * turns it into RUN_FAULT internally without any host signal, and the
     * loop synthesizes the execute-fault record for it.) */
    if (rec && data && emu_no_fault_stash != 1)
    {
        data->emu_fault_rec = *rec;
        data->emu_fault_rec_valid = TRUE;
    }

    return p_fexbridge_fault_unwind( sigcontext ) != 0;
}


/***********************************************************************
 *           emu_alloc_hlt_page
 *
 * One page of x86-64 HLT instructions, preloaded as the return address of
 * every guest entry frame: the guest returning normally executes HLT and
 * fexbridge_run comes back with RUN_HLT and the result in RAX.  One page per
 * process, published to the emulator once.
 */
static void *emu_hlt_page;
static pthread_once_t emu_hlt_once = PTHREAD_ONCE_INIT;

static void emu_alloc_hlt_page(void)
{
    void *mem = NULL;
    SIZE_T size = page_size;

    if (NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0, &size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ))
    {
        ERR( "cannot allocate the guest HLT page\n" );
        return;
    }
    memset( mem, 0xf4 /* hlt */, size );
    if (p_fexbridge_invalidate_code_range)
        p_fexbridge_invalidate_code_range( (ULONG_PTR)mem, size );
    emu_hlt_page = mem;
}


/***********************************************************************
 *           emu_invalidate_code_range
 *
 * The 64-bit lane's equivalent of the WoW64 BTCpuNotify* forwards
 * (wow64cpu_ppc64.c): the emulator detects the GUEST's own stores itself,
 * but code that appears through NATIVE syscalls -- a view mapped over a
 * reused address, a protection change, an explicit NtFlushInstructionCache
 * -- is invisible to it, and fexbridge.h names reporting those the caller's
 * job.  The cost of missing one is not a wrong result but a STICKY wrong
 * result: the bridge's fetchability probe answers for the page as it was,
 * CompileCode caches a NoExecOp block for that rip, and every later call
 * lands on the cached refusal even after the page holds real code
 * (measured: Cyberpunk 2077's amd_ags_x64.dll and its crash handler, both
 * readable at report time, both permanently "NoExec" to the emulator).
 * Over-invalidation is always safe, so virtual.c forwards every range a
 * native syscall could have changed.  No-op when no bridge is loaded.
 */
void emu_invalidate_code_range( const void *addr, SIZE_T size )
{
    if (p_fexbridge_invalidate_code_range)
        p_fexbridge_invalidate_code_range( (ULONG_PTR)addr, size );
}


/***********************************************************************
 *           EC targets (bridge ABI 7, ppc64le/docs/ppc64ec.md step B)
 *
 * Armed at view registration (unixcall_emu_run_entry): the EC handler
 * consumes the view contract, so no view means no EC.  The PE side asks for
 * registration through unix_emu_register_ec the first time a trap resolves
 * into a thunk module (the qpc_arm_module seam) -- so the FIRST call of a
 * module's export traps, and every later call to any of its verified stubs
 * transitions.  Registration is BYTE-VERIFIED per stub: only the exact
 * 5-byte trap body `49 89 ca 0f 05` (mov r10,rcx; syscall) is registered,
 * which excludes PE forwarders, FAST_PATH_EXPORTS bodies, --body=direct
 * stubs and RAW_BODY exports by construction, and is what makes the EC
 * handler's fixed +3 trap-site simulation self-verifying. */
static BOOL emu_ec_armed;
static int  emu_ec_banner_done;

static const UCHAR emu_ec_stub_body[5] = { 0x49, 0x89, 0xca, 0x0f, 0x05 };

static NTSTATUS unixcall_emu_register_ec( void *args )
{
    struct emu_register_ec_params *params = args;
    ULONG_PTR stub = (ULONG_PTR)params->first_stub;
    ULONGLONG *rips;
    const void **cookies = NULL;
    UINT i, verified = 0;

    params->armed = emu_ec_armed ? 1 : 0;
    params->registered = params->skipped = 0;
    /* count == 0 is the PE side's arming PROBE: it asks whether cells are
     * worth allocating before it allocates any (see emu_register_ec_params). */
    if (!emu_ec_armed || !params->count) return STATUS_SUCCESS;

    /* Verify first, register in ONE batch: the bridge's per-target form
     * takes the code-invalidation locks per rip, and a large module's array
     * (ntdll: ~2400) registered that way cost a measured ~1.1 ms inside
     * whatever the guest was timing when the module armed -- the QPC
     * interval gate caught it.  The batch form pays one invalidation and
     * one thread walk for the whole array.
     *
     * The cookie for slot i is that slot's PE-side row cell -- cells is
     * INDEX-aligned with the stub array, so the cell address is computed
     * from the slot index before byte-verification can shift anything. */
    if (!(rips = malloc( params->count * sizeof(*rips) ))) return STATUS_NO_MEMORY;
    if (params->cells && params->cell_size &&
        !(cookies = malloc( params->count * sizeof(*cookies) )))
    {
        free( rips );
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < params->count; i++, stub += params->stride)
    {
        if (memcmp( (const void *)stub, emu_ec_stub_body, sizeof(emu_ec_stub_body) ))
        {
            params->skipped++;
            continue;
        }
        if (cookies)
            cookies[verified] = (const void *)(ULONG_PTR)(params->cells + (ULONGLONG)i * params->cell_size);
        rips[verified++] = stub;
    }
    if (verified)
    {
        int registered;

        if (cookies && p_fexbridge_register_ec_targets2)
            registered = p_fexbridge_register_ec_targets2( rips, cookies, verified, emu_ec_thunk );
        else if (p_fexbridge_register_ec_targets)
            /* a bridge without the _targets2 form: register WITHOUT cookies
             * rather than per-target WITH them -- the per-target loop's
             * registration stall (~1.1 ms, caught by the QPC interval gate)
             * is a measured bug and the cells are only an optimization;
             * cookie NULL just keeps every call on the find path it always
             * had.  The allocated cells go unused, which is the cheap arm
             * of that trade. */
            registered = p_fexbridge_register_ec_targets( rips, verified, emu_ec_thunk, NULL );
        else
        {
            registered = 0;
            for (i = 0; i < verified; i++)
                if (!p_fexbridge_register_ec_target( rips[i], emu_ec_thunk,
                                                     cookies ? (void *)cookies[i] : NULL )) registered++;
        }
        if (registered < 0) registered = 0;
        params->registered = registered;
        params->skipped += verified - registered;
    }
    free( rips );
    free( cookies );
    if (params->registered && !emu_ec_banner_done)
    {
        emu_ec_banner_done = 1;
        /* unconditional stderr, bridge-banner style: the line
         * check-ec-transition asserts on. */
        fprintf( stderr, "wine-emu: ec targets live: direct transitions (bridge ABI 7)\n" );
    }
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           emu_unregister_ec_range
 *
 * Called from the UNMAP seam only (NtUnmapViewOfSection in virtual.c), not
 * from the protect/flush funnel: invalidation is routinely over-broad and
 * harmless, but dropping a live registration downgrades its stubs to traps
 * for the life of the mapping (the PE side's once-per-module arming never
 * re-runs), so this fires only when the addresses really go away.  The
 * hazard it closes is address REUSE: a later mapping over this range must
 * not transition into a handler keyed to the old module's stubs.  Guest
 * thunk modules are rarely unloaded in practice, but FreeLibrary of one is
 * legal and games do unload d3d-family modules. */
void emu_unregister_ec_range( const void *addr, SIZE_T size )
{
    if (emu_ec_armed && p_fexbridge_unregister_ec_range)
        p_fexbridge_unregister_ec_range( (ULONG_PTR)addr, size );
}


/***********************************************************************
 *           emu_hwtso_enable / emu_hwtso_refused
 *
 * The PROT_SAO half of FEX_HWTSO on the native lane.  The bridge decides
 * (probe + config) during fexbridge_process_init*; from then on the JIT
 * emits no TSO barriers and every guest-reachable page must carry the SAO
 * bit.  virtual.c's get_unix_prot() is the funnel that applies it forward;
 * virtual_enable_hwtso() retro-applies it to what was mapped before the
 * lazy bridge init; and a kernel refusal comes back through
 * emu_hwtso_refused, where the bridge either aborts (FEX_HWTSO_STRICT) or
 * revokes the whole process to emitted barriers -- after which the answer
 * here is 0 and virtual.c stops adding the bit.
 */
static void emu_hwtso_enable(void)
{
    UINT prot;

    if (!p_fexbridge_hwtso_prot) return;
    if (!(prot = p_fexbridge_hwtso_prot())) return;
    TRACE( "hardware TSO live, carrying prot bit %#x on guest pages\n", prot );
    virtual_enable_hwtso( prot );
}

unsigned int emu_hwtso_refused( const void *addr, SIZE_T size )
{
    if (!p_fexbridge_hwtso_refused) return 0;
    return p_fexbridge_hwtso_refused( (ULONG_PTR)addr, size );
}


/***********************************************************************
 *           emu_run_loop
 *
 * The Wine-owned replacement for the bridge's one-shot fexbridge_run_entry:
 * same initial frame contract (fexbridge.h:276), but the guest stack is
 * allocated through Wine's own virtual memory (real guard page, visible to
 * NtQueryVirtualMemory, bounds known for guest TEB-frame validity) and the
 * run is driven in a loop -- which is the structural point: RUN_FAULT no
 * longer destroys the run, so a guest exception survives long enough to be
 * dispatched (docs/guest-seh.md section 5.1/5.2).
 */
static NTSTATUS emu_run_loop( struct emu_run_entry_params *params, void *thread )
{
    struct thread_data *data = get_thread_data();
    struct emu_teb_stack prev_teb_stack, native_saved;
    AMD64_CONTEXT ctx = { 0 }, prev_guest_ctx;
    const AMD64_CONTEXT *prev_guest_ptr;
    EXCEPTION_RECORD rec;
    INITIAL_TEB stack;
    NTSTATUS status;
    void *prev_base, *prev_limit, *stack_addr;
    SIZE_T free_size = 0, reserve_size;
    int prev_guest_state;
    ULONG_PTR rsp;
    int r;

    pthread_once( &emu_hlt_once, emu_alloc_hlt_page );
    if (!emu_hlt_page) return STATUS_NO_MEMORY;

    /* Crossing log: this is the native->guest direction -- every entry into
     * guest code, the initial thread start included, goes through here (see
     * ppc64le/cpu/CROSSINGS.md, "the native->guest direction is complete
     * rather than sampled").  Pushed before the guest stack is even
     * allocated, so a failure in that allocation still leaves the attempt on
     * the stack (and is popped on that path too, below); popped for real at
     * every other return from this function, since the whole point of a
     * push/pop stack over a plain log is that it holds only what is still
     * open -- see the comment above struct thread_data::crossing_log. */
    emu_crossing_push( data, EMU_CROSSING_REVERSE, (ULONG_PTR)params->entry );

    /* The guest stack is sized by what THIS THREAD asked for, and the request
     * has already been recorded in a place this side can read: the thread's own
     * native stack.
     *
     * A thread that runs guest code has two stacks, and only one of them is
     * created by anything the application called.  A guest CreateThread( ...,
     * dwStackSize, ... ) is an ordinary thunk call into native kernel32, so
     * kernelbase applies STACK_SIZE_PARAM_IS_A_RESERVATION (dwStackSize is the
     * RESERVE with the flag, the COMMIT without it -- dlls/kernelbase/thread.c),
     * NtCreateThreadEx carries both to init_thread_stack(), and
     * virtual_alloc_thread_stack() resolves them into one mapping of
     * max(reserve, commit) with the image's own values substituted for whatever
     * was left zero.  By the time the new thread reaches here, that whole
     * computation is already expressed as a pair of addresses in its TEB.  So
     * the guest stack asks for the same size, and every rule the request had to
     * obey -- the flag, the image defaults, the one-megabyte floor, the
     * granularity rounding -- was obeyed once, by Wine, in the one place that
     * owns those rules.  A second copy of that arithmetic here would be a
     * second place to get it wrong.
     *
     * A zero reserve, which is what this asked for before, means "the image's
     * SizeOfStackReserve" -- right for the initial thread and wrong for every
     * worker: DOOM (2016) asks for 8 MiB workers ("Starting stack size in KB:
     * 8388608" in its own startup log) against a PE that reserves 2 MiB, so its
     * threads got a quarter of the stack they asked for while gs:[0x10] told
     * them so.  Nothing crashes at that; the guest simply runs out of stack
     * somewhere its author had proved it could not.
     *
     * The initial thread is unchanged by construction: its native stack IS the
     * image default, so mirroring it computes the same number the zero did.
     *
     * WINEEMUNOSTACKSIZE=1 is the negative control, same shape as
     * WINEEMUNOGSTHREADS: size the guest stack from the image and ignore the
     * thread, which is exactly the old behaviour, so a gate that asserts a
     * requested size has something to go red against. */
    if (emu_no_stack_size == -1)
    {
        const char *str = getenv( "WINEEMUNOSTACKSIZE" );
        emu_no_stack_size = (str && *str == '1');
        if (emu_no_stack_size)
            ERR( "WINEEMUNOSTACKSIZE: guest stacks will be sized from the image, "
                 "ignoring what each thread asked for\n" );
    }
    reserve_size = 0;
    if (!emu_no_stack_size && emu_native_teb_stack.base && emu_native_teb_stack.dealloc)
        reserve_size = (char *)emu_native_teb_stack.base - (char *)emu_native_teb_stack.dealloc;

    if ((status = virtual_alloc_thread_stack( &stack, 0, 0, reserve_size, 0, TRUE )))
    {
        ERR( "cannot allocate a guest stack of %lu bytes, status %08x\n",
             (unsigned long)reserve_size, (UINT)status );
        emu_crossing_pop( data );
        return status;
    }
    /* Traced because it is otherwise unobservable from outside the guest: the
     * bounds are published only through the TEB, which the guest reads
     * directly, so "what stack did this run get" has no other answer. */
    TRACE( "guest stack for %p: base %p committed %p reserved %p (%lu bytes, from %s)\n",
           params->entry, stack.StackBase, stack.StackLimit, stack.DeallocationStack,
           (unsigned long)((char *)stack.StackBase - (char *)stack.DeallocationStack),
           reserve_size ? "this thread's own stack" : "the image" );

    /* MS-x64 entry frame, exactly the one-shot's documented shape: the
     * caller reserved 32 bytes of shadow space and CALL pushed a return
     * address -- the HLT page -- so RSP % 16 == 8 at entry, RCX carries the
     * one argument. */
    rsp = ((ULONG_PTR)stack.StackBase & ~(ULONG_PTR)15) - 0x28;
    *(ULONG_PTR *)rsp = (ULONG_PTR)emu_hlt_page;

    ctx.ContextFlags = EMU_CTX_CONTROL | EMU_CTX_INTEGER;
    ctx.Rip    = (ULONG_PTR)params->entry;
    ctx.Rsp    = rsp;
    ctx.Rcx    = (ULONG_PTR)params->arg;
    ctx.EFlags = 0x202;

    /* nested runs (guest callbacks under a trap) stack these */
    prev_base  = emu_guest_stack_base;
    prev_limit = emu_guest_stack_limit;
    emu_guest_stack_base  = stack.StackBase;
    emu_guest_stack_limit = stack.DeallocationStack;

    /* and this, which is the same stack described the way the TEB describes
     * a stack: committed limit rather than reserved base, because it is what
     * guest __chkstk probes against and what grow_thread_stack() moves */
    prev_teb_stack = emu_guest_teb_stack;
    emu_guest_teb_stack.base    = stack.StackBase;
    emu_guest_teb_stack.limit   = stack.StackLimit;
    emu_guest_teb_stack.dealloc = stack.DeallocationStack;

    /* ...and the guest register file a debugger reads, for the same reason.
     * A TRAP state is published as a pointer into the outer trap's frame,
     * which is still live below us for the whole nested run, so the pointer
     * is what gets saved and restored; the deep buffer only matters for the
     * states that publish through it. */
    prev_guest_state = data ? data->emu_guest_ctx_state : EMU_GUEST_NONE;
    prev_guest_ptr = data ? data->emu_guest_ctx_ptr : NULL;
    if (data) prev_guest_ctx = data->emu_guest_ctx;
    else memset( &prev_guest_ctx, 0, sizeof(prev_guest_ctx) );

    for (;;)
    {
        /* only a stash written during THIS run may be consumed below; a
         * record left by some earlier native fault must never be replayed
         * as a guest exception */
        if (data) data->emu_fault_rec_valid = FALSE;

        /* guest code is about to execute: the TEB describes its stack until
         * it stops, whether it stops by trapping, faulting or returning */
        emu_teb_stack_switch( &emu_guest_teb_stack, &native_saved );
        emu_publish_guest_context( &ctx, EMU_GUEST_RUNNING );
        emu_run_depth++;
        r = p_fexbridge_run( thread, &ctx );
        emu_run_depth--;
        emu_teb_stack_switch( &native_saved, &emu_guest_teb_stack );
        /* the bridge filled ctx on the way out, whichever way it came out */
        emu_publish_guest_context( &ctx, r == EMU_RUN_FAULT ? EMU_GUEST_FAULT : EMU_GUEST_ENDED );

        if (r == EMU_RUN_HLT)
        {
            params->retval = ctx.Rax;
            status = STATUS_SUCCESS;
            break;
        }
        if (r == EMU_RUN_EXITED)
        {
            /* the trap dispatcher ended the run: deliberate exit, pending
             * guest exception, or a dispatch failure -- in that order */
            if (emu_thread_exit_requested) { status = STATUS_SUCCESS; break; }
            if (emu_thread_exc_pending)
            {
                emu_thread_exc_pending = FALSE;
                status = STATUS_EMU_GUEST_EXCEPTION;
                break;
            }
            ERR( "guest trap at rip=%p ended the run with no consuming handler\n",
                 (void *)(ULONG_PTR)ctx.Rip );
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        if (r == EMU_RUN_FAULT)
        {
            struct emu_exception_params exc;

            if (data && data->emu_fault_rec_valid)
            {
                rec = data->emu_fault_rec;
                data->emu_fault_rec_valid = FALSE;
            }
            else
            {
                /* No stash: either the bridge classified a jump to
                 * unfetchable memory (no host signal fires for those, and
                 * execute-at-Rip is exactly the right record), or the stash
                 * was skipped (WINEEMUNOFAULTSTASH) -- in which case this
                 * synthesis is visibly WRONG for a data fault, which is what
                 * makes the control falsifiable. */
                memset( &rec, 0, sizeof(rec) );
                rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
                rec.NumberParameters = 2;
                rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
                rec.ExceptionInformation[1] = (ULONG_PTR)ctx.Rip;
            }
            /* the host NIP in the record points into JIT code and means
             * nothing to any handler; the guest Rip is the address */
            rec.ExceptionAddress = (void *)(ULONG_PTR)ctx.Rip;

            if (!params->exception_dispatcher)
            {
                ERR( "guest fault at rip=%p with no exception dispatcher\n",
                     (void *)(ULONG_PTR)ctx.Rip );
                status = STATUS_UNSUCCESSFUL;
                break;
            }
            exc.ctx = &ctx;
            exc.rec = &rec;
            exc.stack_base  = emu_guest_stack_base;
            exc.stack_limit = emu_guest_stack_limit;
            /* Crossing log: also guest->native, same call_emu_trap_dispatcher,
             * same kernel-stack cost as the trap path above -- a guest
             * exception dispatch nesting into a recursion looks identical to
             * the guard and must look identical here. Popped right after the
             * call returns, on every one of its outcomes, same as the trap
             * path in emu_trap_thunk(). */
            emu_crossing_push( data, EMU_CROSSING_TRAP, ctx.Rip );
            status = call_emu_trap_dispatcher( data, params->exception_dispatcher, &exc, NULL );
            emu_crossing_pop( data );
            if (status == STATUS_SUCCESS) continue;   /* handled: resume the edited CONTEXT */
            if (status != STATUS_EMU_GUEST_EXCEPTION)
                ERR( "guest exception dispatch failed, status %08x\n", (UINT)status );
            break;
        }
        ERR( "emulator run error %d at rip=%p\n", r, (void *)(ULONG_PTR)ctx.Rip );
        status = STATUS_UNSUCCESSFUL;
        break;
    }

    emu_guest_stack_base  = prev_base;
    emu_guest_stack_limit = prev_limit;
    emu_guest_teb_stack   = prev_teb_stack;

    /* A run that ended on an unhandled guest exception keeps its published
     * state: the thread is on its way up to raise_pending_guest_exception(),
     * where the record is reported natively and a debugger stops on it, and
     * the guest RIP and registers of the fault are the whole content of that
     * report.  Restoring the caller's state here would hand the debugger the
     * state of whatever ran BEFORE the fault, which is a wrong answer wearing
     * a right answer's clothes.  Every other ending restores. */
    if (status != STATUS_EMU_GUEST_EXCEPTION)
    {
        if (prev_guest_state == EMU_GUEST_TRAP)
            emu_publish_guest_context( prev_guest_ptr, prev_guest_state );
        else
            emu_publish_guest_context( &prev_guest_ctx, prev_guest_state );
    }

    stack_addr = stack.DeallocationStack;
    /* A NESTED run that ended on STATUS_EMU_GUEST_EXCEPTION keeps its STACK,
     * handed to the PE side unfreed.  Both flavors of that ending need it to
     * outlive the run: an unwind request's EXCEPTION_RECORD may carry
     * POINTERS into this stack -- MSVC's FH4 passes the catch establisher
     * frame as a pointer to a personality-run local -- and the next nested
     * run reuses these pages, so freeing here is how GfnRuntimeSdk's catch
     * funclet came to be entered with a null frame [MEASURED].  The PE side
     * owns the free now, at the point it knows nothing needs the stack any
     * more; see call_guest_function() in signal_ppc64.c.
     *
     * The OUTERMOST run keeps the old rule verbatim: the thread is on its
     * way up to report, the stack is kept mapped only while a debugger is
     * attached (registers alone answer "where it died", the stack answers
     * "how it got there"), and WINEEMUNODBGSTACK=1 is the negative control
     * that forces the free (ppc64le/winedbg/check-guest-debug.sh). */
    if (status == STATUS_EMU_GUEST_EXCEPTION && prev_base)
        params->kept_stack = stack_addr;
    else
    {
        if (emu_no_dbg_stack == -1)
        {
            const char *str = getenv( "WINEEMUNODBGSTACK" );
            emu_no_dbg_stack = (str && *str == '1');
        }
        if (status == STATUS_EMU_GUEST_EXCEPTION && !emu_no_dbg_stack &&
            NtCurrentTeb()->Peb->BeingDebugged)
            TRACE( "guest stack %p-%p kept mapped for the debugger; the run ended on an "
                   "unhandled guest exception at rip=%p\n",
                   stack.DeallocationStack, stack.StackBase, (void *)(ULONG_PTR)ctx.Rip );
        else
            NtFreeVirtualMemory( NtCurrentProcess(), &stack_addr, &free_size, MEM_RELEASE );
    }
    emu_crossing_pop( data );
    return status;
}


/***********************************************************************
 *           unixcall_emu_run_entry
 *
 * Run an x86-64 guest entry point through the embedded emulator bridge.
 * The bridge is a native ELF shared object (default libfexbridge.so,
 * overridable through WINEFEXBRIDGE) embedding FEXCore without its Linux
 * frontend; see fexbridge/ in the port tree.  Loaded lazily so processes
 * with a native main image never pay for it, and RTLD_LOCAL so the
 * emulator's internal allocator and C++ runtime symbols never interpose
 * on the rest of the process.
 */
static NTSTATUS unixcall_emu_run_entry( void *args )
{
    struct emu_run_entry_params *params = args;
    static TEB *emu_initial_teb;
    static int no_gs_threads = -1;
    static int use_oneshot = -1;

    void *thread = NULL;
    BOOL own_thread = FALSE;
    char err[256] = "";
    NTSTATUS status;
    int ret;

    pthread_once( &emu_bridge_once, emu_load_bridge );
    if (emu_bridge_status) return emu_bridge_status;

    /* Before anything can trap: this is the only entry into guest execution,
     * and on the way in the TEB still describes the native stack.  True of a
     * nested run too -- emu_trap_thunk() has already put the native stack
     * back by the time a trap dispatcher gets here. */
    emu_capture_native_teb_stack();

    /* The dispatcher and the bridge's trap handler are process-global and were
     * being rewritten on every call.  With one guest thread that was merely
     * redundant; with two it repoints a handler another thread may be trapping
     * through right now.  There is one PE ntdll per process, so the value is
     * the same every time -- freeze it, and treat a different one as a bug
     * rather than silently swapping it under a running thread. */
    if (params->trap_dispatcher)
    {
        if (!p_fexbridge_set_trap_handler)
        {
            ERR( "emulator bridge has no fexbridge_set_trap_handler, guest imports cannot work\n" );
            return STATUS_ENTRYPOINT_NOT_FOUND;
        }
        if (!p_emu_trap_dispatcher)
        {
            p_emu_trap_dispatcher = params->trap_dispatcher;
            p_fexbridge_set_trap_handler( emu_trap_thunk, NULL );
            /* The consumer declaration (bridge ABI 5): this side promises to
             * call fexbridge_ctx_materialize before reading OR writing a trap
             * CONTEXT's EFLAGS or FP group, and the bridge stops building
             * them on every crossing -- ~4-5% of the GameThread was that
             * reconstruction for hops that read neither [2026-08-27 profile].
             * Every consumer is audited and routed through
             * emu_ctx_materialize_full(); the promise is tested by the gate's
             * FEXBRIDGE_CTX_POISON lever, and WINEEMUNOLAZYCTX=1 is this
             * side's refusal -- the negative control that puts the eager
             * world back without touching the bridge. */
            if (p_fexbridge_declare_trap_ctx && p_fexbridge_ctx_materialize)
            {
                const char *str = getenv( "WINEEMUNOLAZYCTX" );
                if (str && *str == '1')
                    ERR( "WINEEMUNOLAZYCTX: trap contexts stay eager; the lazy "
                         "EFLAGS/FP path is off\n" );
                else
                    emu_ctx_lazy_mask = p_fexbridge_declare_trap_ctx( EMU_CTX_LAZY_EFLAGS |
                                                                      EMU_CTX_LAZY_FLOAT );
            }
            /* Bridge ABI 6: the zero-copy trap view.  Registered ALONGSIDE
             * the CONTEXT handler above, never instead of it -- the CONTEXT
             * handler is the landing spot for both kill switches (this
             * side's WINE_PPC64LE_NO_TRAP_VIEW=1 and the bridge's
             * FEXBRIDGE_EAGER_CTX=1 veto) and for any bridge without the
             * protocol.  Gated on the lazy grant: the view is definitionally
             * lazy (no CONTEXT is built to be eager IN), so an eager world
             * -- WINEEMUNOLAZYCTX=1, FEXBRIDGE_EAGER_CTX=1, or an ABI 5
             * bridge -- keeps the CONTEXT protocol wholesale.  64-bit lane
             * only by construction: this registration happens in the 64-bit
             * run entry, which a 32-bit-guest process never reaches (its
             * bounded emu32 runs register no trap handler at all). */
            if (emu_ctx_lazy_mask && p_fexbridge_set_trap_view_handler &&
                p_fexbridge_view_pull && p_fexbridge_view_push)
            {
                const char *str = getenv( "WINE_PPC64LE_NO_TRAP_VIEW" );
                if (str && *str == '1')
                    ERR( "WINE_PPC64LE_NO_TRAP_VIEW: traps stay on the CONTEXT "
                         "protocol; the zero-copy view is off\n" );
                else
                {
                    const char *poison = getenv( "FEXBRIDGE_CTX_POISON" );
                    emu_trap_view_poison = (poison && *poison == '1');
                    p_fexbridge_set_trap_view_handler( emu_trap_view_thunk, NULL );
                    /* unconditional stderr, bridge-banner style ("lazy trap
                     * contexts live"): once per process, and the line the
                     * check-lazy-ctx view leg asserts on. */
                    fprintf( stderr, "wine-emu: trap view live: zero-copy crossings (bridge ABI 6)\n" );

                    /* EC targets (ABI 7) arm only on top of a live view --
                     * the handler consumes the view contract.  Registration
                     * itself is asked for by the PE side per thunk module;
                     * this only opens the door. */
                    if (p_fexbridge_register_ec_target && p_fexbridge_unregister_ec_range)
                    {
                        str = getenv( "WINE_PPC64LE_NO_EC" );
                        if (str && *str == '1')
                            ERR( "WINE_PPC64LE_NO_EC: stub RIPs stay on the trap "
                                 "protocol; no ec targets will be registered\n" );
                        else
                        {
                            str = getenv( "WINE_PPC64LE_EC_SABOTAGE" );
                            emu_ec_sabotage = (str && *str == '1');
                            if (emu_ec_sabotage)
                                ERR( "SABOTAGE: ec transitions will not simulate the "
                                     "stub's mov r10,rcx; every transitioned call reads "
                                     "garbage for argument 0, which is what the gate's "
                                     "negative control requires\n" );
                            emu_ec_armed = TRUE;

                            /* The leaf path rides on EC (it needs the per-rip
                             * cookie) and on the PE side offering it. */
                            str = getenv( "WINE_PPC64LE_NO_EC_LEAF" );
                            if (str && *str == '1')
                                ERR( "WINE_PPC64LE_NO_EC_LEAF: every transitioned call "
                                     "takes the callback frame; the ec leaf path is off\n" );
                            else if (params->leaf_dispatcher)
                            {
                                p_emu_trap_leaf = params->leaf_dispatcher;
                                str = getenv( "WINE_PPC64LE_EC_LEAF_SABOTAGE" );
                                emu_ec_leaf_sabotage = (str && *str == '1');
                                if (emu_ec_leaf_sabotage)
                                    ERR( "SABOTAGE: every leaf-served call answers ~RAX, "
                                         "which is what the gate's negative control "
                                         "requires\n" );
                                /* bridge-banner style, once per process; the
                                 * line check-ec-leaf.sh asserts on */
                                fprintf( stderr, "wine-emu: ec leaf path live: leaf exports "
                                         "skip the callback frame\n" );
                            }
                        }
                    }
                }
            }
        }
        else if (p_emu_trap_dispatcher != params->trap_dispatcher)
        {
            ERR( "trap dispatcher changed from %p to %p; refusing to repoint it\n",
                 p_emu_trap_dispatcher, params->trap_dispatcher );
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* Give the guest a TEB.  Every real x86-64 Windows image reaches it
     * through GS -- the CRT startup reads gs:[0x30] before main to find its
     * own PE header, and SEH, stack probes and TLS all do the same -- so
     * without this a real binary faults on its first TEB access.  Wine's
     * 64-bit TEB is the NT layout the guest expects, so this thread's own TEB
     * serves directly.
     *
     * The bridge sets the base per guest thread, and run_entry adopts an
     * existing one rather than making a transient one, so create it here
     * first.  Anything missing means an ABI-1 bridge: the guest still runs,
     * but only until it touches GS. */
    if (p_fexbridge_set_gs_base && p_fexbridge_thread_init && p_fexbridge_process_init)
    {
        /* process_init before thread_init: run_entry would do it for us, but we
         * need the thread to exist before it runs so that the base is set
         * before the first guest instruction.  Both are idempotent. */
        if (p_fexbridge_process_init()) WARN( "emulator process init failed\n" );
        else emu_hwtso_enable();
        if (p_fexbridge_current_thread) thread = p_fexbridge_current_thread();
        if (!thread && !p_fexbridge_thread_init( &thread )) own_thread = TRUE;

        /* The GS base is per guest thread and is NOT inherited, so every thread
         * that runs guest code must set its own or it reads another thread's
         * TEB -- a bug that looks like it works.  WINEEMUNOGSTHREADS=1 skips it
         * on every thread but the first, which is exactly that bug, so the test
         * that asserts distinct TEBs has something to go red against.  Same
         * shape as WINEEMUKERNELSTACK in unix/signal_ppc64.c. */
        if (!emu_initial_teb) emu_initial_teb = NtCurrentTeb();
        if (no_gs_threads == -1)
        {
            const char *str = getenv( "WINEEMUNOGSTHREADS" );
            no_gs_threads = (str && *str == '1');
        }
        if (thread)
        {
            if (no_gs_threads && NtCurrentTeb() != emu_initial_teb)
                ERR( "WINEEMUNOGSTHREADS: deliberately leaving GS unset on this thread\n" );
            else
                p_fexbridge_set_gs_base( thread, (ULONG_PTR)NtCurrentTeb() );
        }
        else WARN( "no guest thread, GS will be unset\n" );
    }
    else WARN( "emulator bridge cannot set a guest GS base; TEB access will fault\n" );

    /* WINEEMUONESHOT=1 keeps the bridge's one-shot entry callable: the A/B
     * lever for the run loop, and the negative control for everything the
     * Wine-owned guest stack provides (RUN_FAULT survival, bookkeeping
     * visibility, guard semantics).  Same shape as WINEEMUKERNELSTACK. */
    if (use_oneshot == -1)
    {
        const char *str = getenv( "WINEEMUONESHOT" );
        use_oneshot = (str && *str == '1');
        if (use_oneshot) ERR( "WINEEMUONESHOT: using the bridge one-shot entry, guest faults are fatal\n" );
        if (!p_fexbridge_run && !use_oneshot)
        {
            ERR( "emulator bridge has no fexbridge_run; falling back to the one-shot entry\n" );
            use_oneshot = 1;
        }
    }
    if (emu_no_fault_stash == -1)
    {
        const char *str = getenv( "WINEEMUNOFAULTSTASH" );
        emu_no_fault_stash = (str && *str == '1');
        if (emu_no_fault_stash) ERR( "WINEEMUNOFAULTSTASH: guest fault records will be code-only\n" );
    }

    if (!use_oneshot && thread)
    {
        ret = 0;
        status = emu_run_loop( params, thread );
    }
    else
    {
        ret = p_fexbridge_run_entry( params->entry, params->arg, &params->retval,
                                     err, sizeof(err) );
        status = ret ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
    }
    if (own_thread && p_fexbridge_thread_term) p_fexbridge_thread_term( thread );

    /* A guest ExitThread ends the run on purpose.  Check it before the error
     * path: the bridge reports the same "run exited" either way, and only we
     * know it was asked for.  The bridge handle is already gone above, so the
     * thread is an ordinary native one before any teardown runs. */
    if (emu_thread_exit_requested)
    {
        emu_thread_exit_requested = FALSE;
        params->exit_requested = TRUE;
        return STATUS_SUCCESS;
    }
    if (ret) ERR( "emulator bridge failed (%d): %s\n", ret, err );
    return status;
}


/***********************************************************************
 *           unixcall_emu_guest_stack
 *
 * The innermost active run's guest stack bounds on this thread; zeros when
 * no run is active.  For the raise-path guest exception dispatch, which is
 * entered from a trap rather than from the run loop and so has no
 * emu_exception_params in hand.
 */
static NTSTATUS unixcall_emu_guest_stack( void *args )
{
    struct emu_guest_stack_params *params = args;

    params->base  = emu_guest_stack_base;
    params->limit = emu_guest_stack_limit;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           unixcall_emu_fiber_stack
 *
 * The running run's guest stack bounds, read or written, and the HLT page.
 *
 * A guest fiber switch replaces the context the run resumes from, which is
 * PE-side work -- but the bounds that context runs against are kept here,
 * because they are the run's rather than the thread's: emu_teb_stack_switch
 * sets the guest's TEB from them every time guest code starts executing, and
 * dispatch_guest_frames validates every SEH frame against them.  A switch
 * that moved the context and not these would leave a fiber running on its own
 * stack while being told it is on the one before it.
 *
 * Only the innermost run's are touched, which is the only run a guest fiber
 * switch can be made from: the switch arrives as a trap out of that run.
 */
static NTSTATUS unixcall_emu_fiber_stack( void *args )
{
    struct emu_fiber_params *params = args;

    if (params->op == EMU_FIBER_SET_STACK)
    {
        if (!params->base || !params->limit) return STATUS_INVALID_PARAMETER;
        emu_guest_stack_base    = params->base;
        emu_guest_stack_limit   = params->dealloc ? params->dealloc : params->limit;
        emu_guest_teb_stack.base    = params->base;
        emu_guest_teb_stack.limit   = params->limit;
        emu_guest_teb_stack.dealloc = params->dealloc ? params->dealloc : params->limit;
        return STATUS_SUCCESS;
    }
    params->base    = emu_guest_teb_stack.base;
    params->limit   = emu_guest_teb_stack.limit;
    params->dealloc = emu_guest_teb_stack.dealloc;
    params->hlt     = (ULONG_PTR)emu_hlt_page;
    return STATUS_SUCCESS;
}


/***********************************************************************
 * The 32-bit (WoW64) lane.
 *
 * Everything below serves dlls/ntdll/wow64cpu_ppc64.c, the BTCpu* backend
 * wow64.dll drives on this host.  It shares this file's bridge handle and
 * the fault path (emu_handle_fault classifies a JIT fault identically in
 * either mode), and deliberately does NOT share the AMD64 lane's run shape:
 * every emu32 run is BOUNDED -- no trap handler is registered, so a guest
 * bop ends fexbridge_run and the PE side dispatches from its own loop on
 * the Win32 stack.  Why that matters is a stack-cutting story told in
 * ppc64le/wow64/DESIGN.md and at the top of wow64cpu_ppc64.c.
 *
 * The two trap sites are int 0x80 (CD 80): the one 32-bit instruction this
 * FEXCore build routes into the same OS_GENERIC syscall sink the 64-bit
 * lane's 0F 05 uses, with RIP likewise left at the trapping instruction.
 * Both sites live on one page below 4 GiB -- their addresses are stored
 * into 32-bit cells (Wow64Transition, WOW32Reserved) -- and the rest of
 * the page is int3 so a stray jump into it dies legibly.  The exit page is
 * a page of hlt the bridge routes every cooperative run exit through; it
 * must be guest-executable, hence guest-legal, hence allocated HERE with
 * wow64-shaped zero_bits rather than mmapped by the bridge (the bridge
 * cannot place low pages without racing Wine's own reservations).
 */
#define EMU_CTX_FLOATING_POINT (EMU_CTX_AMD64 | 0x8u)

static ULONG_PTR emu32_bop_page;
static ULONG_PTR emu32_exit_page;
static NTSTATUS  emu32_status = STATUS_PENDING;

static NTSTATUS emu32_alloc_low_page( ULONG_PTR *addr_ret, BYTE fill,
                                      const BYTE *bytes, SIZE_T len )
{
    void *mem = NULL;
    SIZE_T size = page_size;
    ULONG old_prot;
    NTSTATUS status;

    /* below 2 GiB: guest-legal whatever the image's large-address-awareness,
     * and representable in the 32-bit cells wow64.dll stores bops into */
    if ((status = NtAllocateVirtualMemory( NtCurrentProcess(), &mem, 0x7fffffff, &size,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE )))
    {
        ERR( "cannot allocate a guest-legal low page, status %08x\n", (UINT)status );
        return status;
    }
    memset( mem, fill, size );
    if (bytes) memcpy( mem, bytes, len );
    NtProtectVirtualMemory( NtCurrentProcess(), &mem, &size, PAGE_EXECUTE_READ, &old_prot );
    /* NOT published to the emulator here: this runs before
     * fexbridge_process_init32, and invalidation against a bridge with no
     * context yet dereferences null inside the emulator (measured: the fault
     * is then swallowed into a bare c0000005 by the syscall fault handler).
     * The caller publishes the bop page after the bridge is up; the exit
     * page is published by process_init32 itself. */
    *addr_ret = (ULONG_PTR)mem;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           unixcall_emu32_init
 *
 * Reached exactly once, from wow64.dll's RtlRunOnceExecuteOnce'd
 * process_init via BTCpuProcessInit, before any second guest thread can
 * exist -- so the statics need no locking.
 */
static NTSTATUS unixcall_emu32_init( void *args )
{
    /* CD 80 at +0 (the syscall bop) and +16 (the unix-call bop); int3 blocks
     * between and after so that only the two published addresses mean
     * anything */
    static const BYTE bops[] = { 0xcd, 0x80, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                 0xcd, 0x80 };
    struct emu32_init_params *params = args;
    const char *str;
    NTSTATUS status;
    int rc;

    pthread_once( &emu_bridge_once, emu_load_bridge );
    if (emu_bridge_status) return emu_bridge_status;

    if (emu32_status == STATUS_PENDING)
    {
        emu32_status = STATUS_UNSUCCESSFUL;

        /* The negative control for the whole lane: with the mechanism
         * disabled the refusal must be prompt and must name its lever, so
         * the gate has a red state to prove the green one means something. */
        if ((str = getenv( "WINEEMUNOWOW32" )) && *str == '1')
        {
            ERR( "WINEEMUNOWOW32: refusing to start the 32-bit emulator lane\n" );
            emu32_status = STATUS_NOT_SUPPORTED;
            return emu32_status;
        }
        if (!p_fexbridge_process_init32 || !p_fexbridge_set_fs_base ||
            !p_fexbridge_run || !p_fexbridge_thread_init || !p_fexbridge_current_thread)
        {
            ERR( "the emulator bridge has no 32-bit guest support; 32-bit "
                 "Windows programs need a bridge with fexbridge_process_init32 (ABI 4)\n" );
            emu32_status = STATUS_ENTRYPOINT_NOT_FOUND;
            return emu32_status;
        }

        if ((status = emu32_alloc_low_page( &emu32_exit_page, 0xf4 /* hlt */, NULL, 0 ))) return status;
        if ((status = emu32_alloc_low_page( &emu32_bop_page, 0xcc /* int3 */,
                                            bops, sizeof(bops) ))) return status;

        if ((rc = p_fexbridge_process_init32( emu32_exit_page )))
        {
            ERR( "32-bit emulator process init failed (%d)\n", rc );
            return emu32_status;
        }
        emu_hwtso_enable();
        p_fexbridge_invalidate_code_range( emu32_bop_page, page_size );
        emu32_status = STATUS_SUCCESS;
        TRACE( "32-bit lane up: bops at %p/+16, exit page %p\n",
               (void *)emu32_bop_page, (void *)emu32_exit_page );
    }
    if (emu32_status) return emu32_status;

    params->bop_syscall  = emu32_bop_page;
    params->bop_unixcall = emu32_bop_page + 16;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           unixcall_emu32_thread
 */
static NTSTATUS unixcall_emu32_thread( void *args )
{
    struct emu32_thread_params *params = args;
    static int no_fs_base = -1;
    void *thread;

    if (emu32_status) return emu32_status;

    if (params->term)
    {
        if ((thread = p_fexbridge_current_thread()) && p_fexbridge_thread_term)
            p_fexbridge_thread_term( thread );
        return STATUS_SUCCESS;
    }

    if (!(thread = p_fexbridge_current_thread()) && p_fexbridge_thread_init( &thread ))
    {
        ERR( "cannot create the emulator thread state\n" );
        return STATUS_NO_MEMORY;
    }

    /* The FS base is what makes the 32-bit TIB reachable: every fs: access
     * in the guest -- SEH registration, TLS, stack bounds, the CRT's very
     * first moves -- adds it.  WINEEMUNOFSBASE32=1 leaves it unset, which is
     * this mechanism's own negative control: the guest then dereferences its
     * TIB offsets against base 0 and dies before it can claim anything
     * works.  Same shape as WINEEMUNOGSTHREADS above. */
    if (no_fs_base == -1)
    {
        const char *str = getenv( "WINEEMUNOFSBASE32" );
        no_fs_base = (str && *str == '1');
    }
    if (no_fs_base)
        ERR( "WINEEMUNOFSBASE32: deliberately leaving the FS base unset on this thread\n" );
    else if (p_fexbridge_set_fs_base( thread, params->teb32 ))
    {
        ERR( "cannot set the FS base to the 32-bit TEB %p\n", (void *)params->teb32 );
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           emu32 context conversion
 *
 * The bridge speaks flat AMD64_CONTEXT in both modes; the TEB cpu area
 * speaks I386_CONTEXT.  ExtendedRegisters IS the FXSAVE image and is the
 * authoritative FP state in a WoW64 context -- FloatSave is the legacy
 * FSAVE view, derived on the way out for readers that only know it, ignored
 * on the way in.
 */
static void emu32_context_to_bridge( const I386_CONTEXT *wow, AMD64_CONTEXT *ctx )
{
    memset( ctx, 0, sizeof(*ctx) );
    ctx->ContextFlags = EMU_CTX_CONTROL | EMU_CTX_INTEGER | EMU_CTX_FLOATING_POINT;
    ctx->Rax = wow->Eax;
    ctx->Rbx = wow->Ebx;
    ctx->Rcx = wow->Ecx;
    ctx->Rdx = wow->Edx;
    ctx->Rsi = wow->Esi;
    ctx->Rdi = wow->Edi;
    ctx->Rbp = wow->Ebp;
    ctx->Rsp = wow->Esp;
    ctx->Rip = wow->Eip;
    ctx->EFlags = wow->EFlags;
    memcpy( &ctx->FltSave, wow->ExtendedRegisters, sizeof(ctx->FltSave) );
    ctx->MxCsr = ctx->FltSave.MxCsr;
}

static void emu32_context_from_bridge( const AMD64_CONTEXT *ctx, I386_CONTEXT *wow )
{
    wow->ContextFlags = CONTEXT_I386_ALL;
    wow->Eax = (ULONG)ctx->Rax;
    wow->Ebx = (ULONG)ctx->Rbx;
    wow->Ecx = (ULONG)ctx->Rcx;
    wow->Edx = (ULONG)ctx->Rdx;
    wow->Esi = (ULONG)ctx->Rsi;
    wow->Edi = (ULONG)ctx->Rdi;
    wow->Ebp = (ULONG)ctx->Rbp;
    wow->Esp = (ULONG)ctx->Rsp;
    wow->Eip = (ULONG)ctx->Rip;
    wow->EFlags = ctx->EFlags;
    wow->SegCs = ctx->SegCs;
    wow->SegSs = ctx->SegSs;
    wow->SegDs = ctx->SegDs;
    wow->SegEs = ctx->SegEs;
    wow->SegFs = ctx->SegFs;
    wow->SegGs = ctx->SegGs;
    wow->Dr0 = wow->Dr1 = wow->Dr2 = wow->Dr3 = wow->Dr6 = wow->Dr7 = 0;
    memcpy( wow->ExtendedRegisters, &ctx->FltSave, sizeof(ctx->FltSave) );
    fpux_to_fpu( &wow->FloatSave, (const XSAVE_FORMAT *)wow->ExtendedRegisters );
}

/***********************************************************************
 *           unixcall_emu32_run
 *
 * One bounded run: guest state in from the TEB cpu area, run until the
 * guest stops, full state and a reason back out.  Classification is by the
 * bridge's run result plus WHERE the guest stopped -- the two bop sites are
 * the only addresses a trap legitimately parks RIP at.
 */
static NTSTATUS unixcall_emu32_run( void *args )
{
    struct emu32_run_params *params = args;
    struct thread_data *data = get_thread_data();
    I386_CONTEXT *wow = params->context;
    AMD64_CONTEXT ctx;
    void *thread;
    int r;

    if (emu32_status) return emu32_status;
    if (!(thread = p_fexbridge_current_thread()))
    {
        ERR( "no emulator thread state; BTCpuThreadInit did not run or failed\n" );
        return STATUS_UNSUCCESSFUL;
    }

    emu32_context_to_bridge( wow, &ctx );
    if (data) data->emu_fault_rec_valid = FALSE;
    emu_run_depth++;
    r = p_fexbridge_run( thread, &ctx );
    emu_run_depth--;
    emu32_context_from_bridge( &ctx, wow );

    if (r == EMU_RUN_EXITED && (ULONG_PTR)ctx.Rip == emu32_bop_page)
    {
        params->reason = EMU32_RUN_SYSCALL;
        return STATUS_SUCCESS;
    }
    if (r == EMU_RUN_EXITED && (ULONG_PTR)ctx.Rip == emu32_bop_page + 16)
    {
        params->reason = EMU32_RUN_UNIXCALL;
        return STATUS_SUCCESS;
    }

    params->reason = EMU32_RUN_FAULT;
    memset( &params->rec, 0, sizeof(params->rec) );
    switch (r)
    {
    case EMU_RUN_FAULT:
        if (data && data->emu_fault_rec_valid)
        {
            params->rec = data->emu_fault_rec;
            data->emu_fault_rec_valid = FALSE;
        }
        else
        {
            /* the bridge classified a jump to unfetchable memory internally;
             * no host signal fired, execute-at-Rip is the right record */
            params->rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            params->rec.NumberParameters = 2;
            params->rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
            params->rec.ExceptionInformation[1] = (ULONG)ctx.Rip;
        }
        break;
    case EMU_RUN_HLT:
        /* hlt in guest code: privileged instruction, as on Windows */
        params->rec.ExceptionCode = EXCEPTION_PRIV_INSTRUCTION;
        break;
    case EMU_RUN_EXITED:
        /* an int 0x80 that is not one of the two bop sites: a thunk-stub
         * trap if the PE side's dispatcher knows the Eip, an unassigned
         * vector otherwise.  Only the PE side can tell -- the stub tables
         * live in PE modules this side has no view of -- so hand it up as
         * TRAP and let it classify. */
        params->reason = EMU32_RUN_TRAP;
        return STATUS_SUCCESS;
    default:
        ERR( "emulator run error %d at eip=%08x\n", r, (UINT)ctx.Rip );
        return STATUS_UNSUCCESSFUL;
    }
    params->rec.ExceptionAddress = (void *)(ULONG_PTR)(ULONG)ctx.Rip;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           unixcall_emu32_invalidate
 */
static NTSTATUS unixcall_emu32_invalidate( void *args )
{
    struct emu32_invalidate_params *params = args;

    if (emu32_status) return emu32_status;
    if (p_fexbridge_invalidate_code_range)
        p_fexbridge_invalidate_code_range( params->base, params->size );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           the crossing-frequency sink (WINE_PPC64LE_TRAP_STATS)
 *
 * Counts, never times: a sampling profiler answers "where did the time go
 * inside the callee" and can never answer "how many times was the boundary
 * crossed to get there", which is the number a fast path is chosen against.
 *
 * Everything here is off unless WINE_PPC64LE_TRAP_STATS names a file.  The
 * table is allocated once, addressed by both sides of ntdll directly, and
 * only ever formatted here, because only this side has stdio and the path.
 */

static struct emu_xstat_ctl xstat_ctl;
static int xstat_armed = -1;          /* -1 = the environment is unread */
static struct timespec xstat_t0;
static pthread_mutex_t xstat_dump_mutex = PTHREAD_MUTEX_INITIALIZER;

static void xstat_sigusr2( int sig )
{
    xstat_ctl.dump_req = 1;
}

/* Every registered syscall table gets a counter array, and the dispatcher's
 * increment is gated on that pointer being non-NULL -- so the OFF cost in
 * __wine_syscall_dispatcher is one load, one compare and one not-taken
 * branch, and no table ever half-counts.  Called both from here (for the
 * tables that exist when the sink arms) and from KeAddSystemServiceTable (for
 * win32u's, which registers later than a guest's first crossing). */
ULONG_PTR *emu_xstat_syscall_counters( ULONG limit )
{
    if (!emu_xstat_enabled()) return NULL;
    return calloc( limit, sizeof(ULONG_PTR) );
}

int emu_xstat_enabled(void)
{
    if (xstat_armed == -1) xstat_armed = getenv( "WINE_PPC64LE_TRAP_STATS" ) != NULL;
    return xstat_armed;
}

static void xstat_attach_syscall_counters(void)
{
    UINT i;

    for (i = 0; i < ARRAY_SIZE(KeServiceDescriptorTable); i++)
    {
        SYSTEM_SERVICE_TABLE *t = &KeServiceDescriptorTable[i];

        if (!t->ServiceTable || !t->ServiceLimit || t->CounterTable) continue;
        t->CounterTable = calloc( t->ServiceLimit, sizeof(ULONG_PTR) );
    }
}

/***********************************************************************
 *           unixcall_emu_xstat_init
 */
static NTSTATUS unixcall_emu_xstat_init( void *args )
{
    struct emu_xstat_init_params *params = args;

    params->ctl = NULL;
    if (!emu_xstat_enabled()) return STATUS_SUCCESS;
    /* The PE side probes lazily on its first crossing, so several guest
     * threads can arrive here together on a process that starts them at once. */
    pthread_mutex_lock( &xstat_dump_mutex );
    if (!xstat_ctl.rows)
    {
        struct sigaction act;

        if (!(xstat_ctl.rows = calloc( EMU_XSTAT_ROWS, sizeof(*xstat_ctl.rows) )))
        {
            pthread_mutex_unlock( &xstat_dump_mutex );
            return STATUS_SUCCESS;                 /* off rather than fatal */
        }
        xstat_ctl.rows_max = EMU_XSTAT_ROWS;
        clock_gettime( CLOCK_MONOTONIC, &xstat_t0 );
        xstat_attach_syscall_counters();

        /* SIGUSR2 is free in this port -- SIGQUIT and SIGUSR1 are the only two
         * this side installs -- and the handler only raises a flag the next
         * crossing reads, so a dump can be asked for at a chosen moment of a
         * benchmark without stopping it. */
        memset( &act, 0, sizeof(act) );
        act.sa_handler = xstat_sigusr2;
        sigemptyset( &act.sa_mask );
        act.sa_flags = SA_RESTART;
        sigaction( SIGUSR2, &act, NULL );
    }
    pthread_mutex_unlock( &xstat_dump_mutex );
    params->ctl = &xstat_ctl;
    return STATUS_SUCCESS;
}

static int xstat_cmp( const void *a, const void *b )
{
    const struct emu_xstat_row *ra = a, *rb = b;

    if (ra->count != rb->count) return ra->count < rb->count ? 1 : -1;
    return strcmp( ra->name, rb->name );
}

static const char *xstat_class_name( UINT cls )
{
    switch (cls)
    {
    case EMU_XSTAT_FLAT:     return "flat";
    case EMU_XSTAT_COM:      return "com";
    case EMU_XSTAT_SYSCALL:  return "syscall";
    case EMU_XSTAT_CALLBACK: return "callback";
    default:                 return "event";
    }
}

/***********************************************************************
 *           unixcall_emu_xstat_dump
 *
 * Rewritten in full each time through a temporary and a rename, so a reader
 * (or a run killed mid-write) never sees a half-printed table.
 */
static NTSTATUS unixcall_emu_xstat_dump( void *args )
{
    ULONG64 class_total[EMU_XSTAT_CLASSES] = { 0 }, total = 0;
    struct emu_xstat_row *snap;
    const char *path;
    struct timespec now;
    char tmp[PATH_MAX], final[PATH_MAX];
    double secs;
    UINT i, j, n = 0, syscalls = 0;
    FILE *f;

    if (!xstat_ctl.rows) return STATUS_SUCCESS;
    if (!(path = getenv( "WINE_PPC64LE_TRAP_STATS" ))) return STATUS_SUCCESS;
    /* One writer at a time, and a second one simply skips: two threads
     * reaching their periodic dump together would otherwise interleave into
     * one temporary file and rename the mixture into place. */
    if (pthread_mutex_trylock( &xstat_dump_mutex )) return STATUS_SUCCESS;

    clock_gettime( CLOCK_MONOTONIC, &now );
    secs = (now.tv_sec - xstat_t0.tv_sec) + (now.tv_nsec - xstat_t0.tv_nsec) / 1e9;
    if (secs <= 0.0) secs = 1e-9;

    /* Syscalls are counted in the dispatcher's own CounterTable rather than in
     * a row, because the dispatcher is assembly and the table pointer it
     * already loads costs it nothing extra.  They join the table here. */
    for (i = 0; i < ARRAY_SIZE(KeServiceDescriptorTable); i++)
        if (KeServiceDescriptorTable[i].CounterTable) syscalls += KeServiceDescriptorTable[i].ServiceLimit;

    if (!(snap = calloc( xstat_ctl.rows_max + syscalls, sizeof(*snap) )))
    {
        pthread_mutex_unlock( &xstat_dump_mutex );
        return STATUS_NO_MEMORY;
    }

    for (i = 0; i < xstat_ctl.rows_max; i++)
    {
        const struct emu_xstat_row *r = &xstat_ctl.rows[i];
        ULONG64 c = __atomic_load_n( &r->count, __ATOMIC_RELAXED );

        if (!r->key || !c) continue;
        snap[n] = *r;
        snap[n].count = c;
        n++;
    }
    for (i = 0; i < ARRAY_SIZE(KeServiceDescriptorTable); i++)
    {
        const SYSTEM_SERVICE_TABLE *t = &KeServiceDescriptorTable[i];

        if (!t->CounterTable) continue;
        for (j = 0; j < t->ServiceLimit; j++)
        {
            ULONG_PTR c = __atomic_load_n( &t->CounterTable[j], __ATOMIC_RELAXED );
            const char *name = ntdll_syscall_name( i, j );

            if (!c) continue;
            snap[n].count = c;
            snap[n].cls   = EMU_XSTAT_SYSCALL;
            if (name) snprintf( snap[n].name, sizeof(snap[n].name), "%s", name );
            else snprintf( snap[n].name, sizeof(snap[n].name), "syscall %04x", (i << 12) | j );
            n++;
        }
    }

    for (i = 0; i < n; i++)
    {
        total += snap[i].count;
        if (snap[i].cls < EMU_XSTAT_CLASSES) class_total[snap[i].cls] += snap[i].count;
    }
    qsort( snap, n, sizeof(*snap), xstat_cmp );

    /* ONE FILE PER PROCESS, always.  A game launch is a whole Wine session --
     * wineboot, services.exe, explorer.exe, the game -- and every one of them
     * crosses the boundary and dumps here.  Sharing the path made the LAST
     * process to exit the only one measured, which is never the game
     * ([MEASURED]: a complete Cyberpunk benchmark left a 1474-crossing file
     * written by a service).  The reader picks the biggest. */
    snprintf( final, sizeof(final), "%s.%u", path, (UINT)getpid() );
    snprintf( tmp, sizeof(tmp), "%s.tmp", final );
    if (!(f = fopen( tmp, "w" )))
    {
        free( snap );
        pthread_mutex_unlock( &xstat_dump_mutex );
        return STATUS_SUCCESS;
    }
    fprintf( f, "# guest/native crossing frequency, pid %u, %.2f s of process life\n",
             (UINT)getpid(), secs );
    fprintf( f, "# %llu crossings counted in %u named rows\n",
             (unsigned long long)total, n );
    for (i = 0; i < EMU_XSTAT_CLASSES; i++)
        fprintf( f, "# class %-8s %14llu  %12.0f/s\n", xstat_class_name( i ),
                 (unsigned long long)class_total[i], class_total[i] / secs );
    fprintf( f, "\n%-8s %14s %12s  %s\n", "class", "count", "per-sec", "name" );
    for (i = 0; i < n; i++)
        fprintf( f, "%-8s %14llu %12.0f  %s\n", xstat_class_name( snap[i].cls ),
                 (unsigned long long)snap[i].count, snap[i].count / secs, snap[i].name );
    fclose( f );
    rename( tmp, final );
    free( snap );
    pthread_mutex_unlock( &xstat_dump_mutex );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           unixcall_emu_name_host_addr
 *
 * dladdr over an address the PE-side fault reporter could not place in any
 * loader-list image: names the dlopened unix .so (DXVK, the wine .so's) a
 * leaked host pointer landed in.  Diagnostic only, called at most a handful
 * of times per process (the reporter self-limits), so the dladdr cost is
 * nothing and failure is an honest found=0.
 */
static NTSTATUS unixcall_emu_name_host_addr( void *args )
{
    struct emu_name_host_addr_params *params = args;
    Dl_info info;

    params->found = 0;
    params->name[0] = 0;
    params->base = 0;
    if (dladdr( (void *)(ULONG_PTR)params->addr, &info ) && info.dli_fname)
    {
        const char *tail = strrchr( info.dli_fname, '/' );
        size_t n;

        tail = tail ? tail + 1 : info.dli_fname;
        n = strlen( tail );
        if (n >= sizeof(params->name)) n = sizeof(params->name) - 1;
        memcpy( params->name, tail, n );
        params->name[n] = 0;
        params->base = (UINT64)(ULONG_PTR)info.dli_fbase;
        params->found = 1;
    }
    return STATUS_SUCCESS;
}

static const unixlib_entry_t unix_call_funcs[] =
{
    load_so_dll,
    unwind_builtin_dll,
    unixcall_wine_dbg_write,
    unixcall_wine_server_call,
    unixcall_wine_server_fd_to_handle,
    unixcall_wine_server_handle_to_fd,
    unixcall_wine_spawnvp,
    system_time_precise,
    unixcall_emu_run_entry,
    unixcall_emu_guest_stack,
    unixcall_emu_fiber_stack,
    unixcall_emu32_init,
    unixcall_emu32_thread,
    unixcall_emu32_run,
    unixcall_emu32_invalidate,
    unixcall_emu_xstat_init,
    unixcall_emu_xstat_dump,
    unixcall_emu_ctx_materialize,
    unixcall_emu_register_ec,
    unixcall_emu_name_host_addr,
};


#ifdef _WIN64

static NTSTATUS wow64_load_so_dll( void *args ) { return STATUS_INVALID_IMAGE_FORMAT; }
static NTSTATUS wow64_unwind_builtin_dll( void *args ) { return STATUS_UNSUCCESSFUL; }

static NTSTATUS wow64_emu_run_entry( void *args ) { return STATUS_NOT_SUPPORTED; }

const unixlib_entry_t unix_call_wow64_funcs[] =
{
    wow64_load_so_dll,
    wow64_unwind_builtin_dll,
    wow64_wine_dbg_write,
    wow64_wine_server_call,
    wow64_wine_server_fd_to_handle,
    wow64_wine_server_handle_to_fd,
    wow64_wine_spawnvp,
    system_time_precise,
    wow64_emu_run_entry,
    wow64_emu_run_entry,   /* unix_emu_guest_stack: same not-supported answer */
    wow64_emu_run_entry,   /* unix_emu_fiber_stack: likewise */
    /* the emu32 group is the 64-bit CPU backend's own interface; nothing a
     * 32-bit caller could say through it is meaningful */
    wow64_emu_run_entry,   /* unix_emu32_init */
    wow64_emu_run_entry,   /* unix_emu32_thread */
    wow64_emu_run_entry,   /* unix_emu32_run */
    wow64_emu_run_entry,   /* unix_emu32_invalidate */
    wow64_emu_run_entry,   /* unix_emu_xstat_init: the sink counts native-lane
                              crossings, which a wow64 caller does not make */
    wow64_emu_run_entry,   /* unix_emu_xstat_dump */
    wow64_emu_run_entry,   /* unix_emu_ctx_materialize: 64-bit trap frames only */
    wow64_emu_run_entry,   /* unix_emu_register_ec: 64-bit stub arrays only */
    wow64_emu_run_entry,   /* unix_emu_name_host_addr: 64-bit diagnostics only */
};

#endif  /* _WIN64 */


static inline char *prepend( char *buffer, const char *str, size_t len )
{
    return memcpy( buffer - len, str, len );
}

static inline char *prepend_build_dir_path( char *ptr, const char *ext, const char *arch_dir,
                                            const char *top_dir, const char *build_dir )
{
    char *name = ptr;
    unsigned int namelen = strlen(name), extlen = strlen(ext);

    if (namelen > extlen && !strcmp( name + namelen - extlen, ext )) namelen -= extlen;
    ptr = prepend( ptr, arch_dir, strlen(arch_dir) );
    ptr = prepend( ptr, name, namelen );
    ptr = prepend( ptr, top_dir, strlen(top_dir) );
    ptr = prepend( ptr, build_dir, strlen(build_dir) );
    return ptr;
}


/***********************************************************************
 *	open_dll_file
 *
 * Open a file for a new dll. Helper for open_builtin_pe_file.
 */
static NTSTATUS open_dll_file( const char *name, OBJECT_ATTRIBUTES *attr, HANDLE *mapping )
{
    LARGE_INTEGER size;
    NTSTATUS status;
    HANDLE handle;

    if ((status = open_unix_file( &handle, name, GENERIC_READ | SYNCHRONIZE, attr, 0,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
                                  FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0 )))
    {
        if (status != STATUS_OBJECT_PATH_NOT_FOUND && status != STATUS_OBJECT_NAME_NOT_FOUND)
        {
            /* if the file exists but failed to open, report the error */
            struct stat st;
            if (!stat( name, &st )) return status;
        }
        /* otherwise continue searching */
        return STATUS_DLL_NOT_FOUND;
    }

    size.QuadPart = 0;
    status = NtCreateSection( mapping, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY |
                              SECTION_MAP_READ | SECTION_MAP_EXECUTE,
                              NULL, &size, PAGE_EXECUTE_READ, SEC_IMAGE, handle );
    NtClose( handle );
    return status;
}


/***********************************************************************
 *           open_builtin_pe_file
 */
static NTSTATUS open_builtin_pe_file( const char *name, OBJECT_ATTRIBUTES *attr, void **module,
                                      SIZE_T *size, SECTION_IMAGE_INFORMATION *image_info,
                                      ULONG_PTR limit_low, ULONG_PTR limit_high,
                                      WORD machine, BOOL prefer_native, off_t offset )
{
    NTSTATUS status;
    HANDLE mapping;

    *module = NULL;
    status = open_dll_file( name, attr, &mapping );
    if (!status)
    {
        status = virtual_map_builtin_module( mapping, module, size, image_info,
                                             limit_low, limit_high, machine, prefer_native, offset );
        NtClose( mapping );
    }
    return status;
}


/***********************************************************************
 *           find_builtin_dll
 */
static NTSTATUS find_builtin_dll( UNICODE_STRING *nt_name, ANSI_STRING *exp_name, void **module,
                                  SIZE_T *size_ptr, SECTION_IMAGE_INFORMATION *image_info,
                                  ULONG_PTR limit_low, ULONG_PTR limit_high, USHORT search_machine,
                                  USHORT load_machine, BOOL prefer_native, off_t offset )
{
    unsigned int i, pos, len, namepos = 0, maxlen = 0;
    char *ptr = NULL, *file, *ext = NULL;
    const char *pe_dir = get_pe_dir( search_machine );
    const char *so_dir = get_so_dir( current_machine );
    const char *pe_build_dir = build_dir;
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status = STATUS_DLL_NOT_FOUND;
    BOOL found_image = FALSE;

    InitializeObjectAttributes( &attr, nt_name, 0, 0, NULL );

    if (!exp_name || !exp_name->Length)
    {
        len = nt_name->Length / sizeof(WCHAR);
        for (i = 0; i < len; i++)
            if (nt_name->Buffer[i] == '/' || nt_name->Buffer[i] == '\\') namepos = i + 1;
        len -= namepos;
        if (!len) return STATUS_DLL_NOT_FOUND;
    }
    else len = exp_name->Length;

    if (build_dir)
    {
        if (alt_build_dir && search_machine == get_alt_machine( current_machine ))
            pe_build_dir = alt_build_dir;
        maxlen = max( strlen(build_dir), strlen(pe_build_dir) ) + sizeof("/programs/") + len;
    }
    maxlen = max( maxlen, dll_path_maxlen + 1 ) + len + sizeof("/aarch64-windows") + sizeof(".so");

    if (!(file = malloc( maxlen ))) return STATUS_NO_MEMORY;

    pos = maxlen - len - sizeof(".so");
    if (!exp_name || !exp_name->Length)
    {
        /* we don't want to depend on the current codepage here */
        for (i = 0; i < len; i++)
        {
            if (nt_name->Buffer[namepos + i] > 127) goto done;
            file[pos + i] = (char)nt_name->Buffer[namepos + i];
        }
    }
    else memcpy( file + pos, exp_name->Buffer, len );

    for (i = 0; i < len; i++)
    {
        if (file[pos + i] >= 'A' && file[pos + i] <= 'Z') file[pos + i] += 'a' - 'A';
        else if (file[pos + i] == '.') ext = file + pos + i;
    }
    file[pos + len] = 0;
    file[--pos] = '/';

    TRACE( "looking for %s for file %s\n", debugstr_a(file + pos + 1), debugstr_us(nt_name) );

    if (build_dir)
    {
        /* try as a dll */
        ptr = prepend_build_dir_path( file + pos, ".dll", pe_dir, "/dlls", pe_build_dir );
        status = open_builtin_pe_file( ptr, &attr, module, size_ptr, image_info,
                                       limit_low, limit_high, load_machine, prefer_native, offset );
        ptr = prepend_build_dir_path( file + pos, ".dll", "", "/dlls", build_dir );
        if (status != STATUS_DLL_NOT_FOUND) goto done;
        status = open_builtin_so_file( ptr, &attr, module, image_info,
                                       search_machine, load_machine, prefer_native );
        if (status != STATUS_DLL_NOT_FOUND) goto done;

        /* now as a program */
        ptr = prepend_build_dir_path( file + pos, ".exe", pe_dir, "/programs", pe_build_dir );
        status = open_builtin_pe_file( ptr, &attr, module, size_ptr, image_info,
                                       limit_low, limit_high, load_machine, prefer_native, offset );
        ptr = prepend_build_dir_path( file + pos, ".exe", "", "/programs", build_dir );
        if (status != STATUS_DLL_NOT_FOUND) goto done;
        status = open_builtin_so_file( ptr, &attr, module, image_info,
                                       search_machine, load_machine, prefer_native );
        if (status != STATUS_DLL_NOT_FOUND) goto done;
    }

    for (i = 0; dll_paths[i]; i++)
    {
        ptr = file + pos;
        ptr = prepend( ptr, pe_dir, strlen(pe_dir) );
        ptr = prepend( ptr, dll_paths[i], strlen(dll_paths[i]) );
        status = open_builtin_pe_file( ptr, &attr, module, size_ptr, image_info, limit_low, limit_high,
                                       load_machine, prefer_native, offset );
        /* use so dir for unix lib */
        ptr = file + pos;
        ptr = prepend( ptr, so_dir, strlen(so_dir) );
        ptr = prepend( ptr, dll_paths[i], strlen(dll_paths[i]) );
        if (status != STATUS_DLL_NOT_FOUND) goto done;
        status = open_builtin_so_file( ptr, &attr, module, image_info,
                                       search_machine, load_machine, prefer_native );
        if (status != STATUS_DLL_NOT_FOUND) goto done;
        ptr = prepend( file + pos, dll_paths[i], strlen(dll_paths[i]) );
        status = open_builtin_pe_file( ptr, &attr, module, size_ptr, image_info, limit_low, limit_high,
                                       load_machine, prefer_native, offset );
        if (status == STATUS_NOT_SUPPORTED)
        {
            found_image = TRUE;
            continue;
        }
        if (status != STATUS_DLL_NOT_FOUND) goto done;
        status = open_builtin_so_file( ptr, &attr, module, image_info,
                                       search_machine, load_machine, prefer_native );
        if (status == STATUS_NOT_SUPPORTED) found_image = TRUE;
        else if (status != STATUS_DLL_NOT_FOUND) goto done;
    }

    if (found_image) status = STATUS_NOT_SUPPORTED;
    WARN( "cannot find builtin library for %s\n", debugstr_us(nt_name) );
done:
    if (NT_SUCCESS(status) && ext)
    {
        strcpy( ext, ".so" );
        set_builtin_unixlib_name( *module, ptr );
    }
    free( file );
    return status;
}


/***********************************************************************
 *           load_builtin
 *
 * Load the builtin dll if specified by load order configuration.
 * Return STATUS_IMAGE_ALREADY_LOADED if we should keep the native one that we have found.
 */
NTSTATUS load_builtin( struct pe_mapping_info *pe_mapping, USHORT machine,
                       SECTION_IMAGE_INFORMATION *info, void **module, SIZE_T *size,
                       ULONG_PTR limit_low, ULONG_PTR limit_high, off_t offset )
{
    NTSTATUS status;
    USHORT sysdir_machine, search_machine = pe_mapping->image.machine;
    BOOL is_system_dir = is_system_dir_path( &pe_mapping->nt_name, &sysdir_machine );
    enum loadorder loadorder = get_load_order( &pe_mapping->nt_name, is_system_dir, pe_mapping );

    if (loadorder == LO_DISABLED) return STATUS_DLL_NOT_FOUND;

    /* A file that lives in a GUEST machine's own system directory --
     * C:\windows\sysx8664 and its siblings -- has already been chosen, on
     * purpose, by the guest branch of find_dll_file(), which searches that
     * directory BEFORE anything else precisely so that a module staged there
     * wins.  Re-resolving the NAME here would undo that decision, and it can
     * only ever substitute a DIFFERENT module: find_builtin_dll() looks in the
     * build and install trees and never in the prefix, so the file just opened
     * is not even a candidate for the search that would replace it.
     *
     * Measured, with Proton's x86-64 msvcp100.dll staged there for Styx:
     *
     *   find_builtin_dll looking for "msvcp100.dll" for file
     *     L"\??\C:\windows\sysx8664\msvcp100.dll"
     *   map_image_into_view mapping PE file
     *     L"\??\C:\windows\sysx8664\msvcp100.dll" at ...-0x...3000
     *     section .rdata ... section .reloc ...
     *
     * -- a three-page image wearing the staged file's name, which is this
     * tree's own msvcp100 guest thunk.  That thunk has NO exports at all,
     * because all 46 are MSVC-mangled C++ member functions the signature
     * oracle refuses, so the 1.7 MB module the user staged was opened,
     * machine-checked, and then thrown away for one that answers NULL to
     * every GetProcAddress.
     *
     * The nt_name is a reliable discriminator rather than a guess: it is the
     * name the SERVER recorded for the file the section was created from
     * (server/mapping.c, get_nt_name), so an ordinary guest thunk resolved out
     * of the build tree arrives here as
     * L"\??\Z:\home\...\dlls\msvcr100\x86_64-windows\msvcr100.dll" and does not
     * match, while a staged file arrives as its sysx8664 path and does.
     * find_builtin_without_file() rewrites the CALLER's copy of the name to
     * the synthetic system-directory form afterwards -- which is what the
     * module list shows -- but that happens after the mapping already exists.
     *
     * AMD64's directory ONLY, and that restriction is load-bearing rather than
     * cautious.  The rule is really "this machine's modules are not staged into
     * the prefix, so a file found there was put there deliberately and the
     * builtin search cannot be looking for it" -- and that is true of
     * sysx8664, which this port introduced and which nothing populates
     * automatically, while it is false of syswow64.  Measured: a booted prefix
     * here has 858 entries in C:\windows\syswow64, wineboot's own copies of the
     * 32-bit builtins, "Wine builtin DLL" in their DOS stubs and machine i386 --
     * every one of which would match a rule written on sysdir_machine alone.
     * Those must keep going through find_builtin_dll, because that is the path
     * that also resolves a builtin's .so unixlib (set_builtin_unixlib_name);
     * mapping them as if they were ordinary files would take the unixlib away
     * from every 32-bit module that has one.
     *
     * The current machine's own system32 is excluded for the same reason and
     * one more: there, builtin-first is the load order Wine has always had, and
     * changing it would change every native load in every prefix.  LO_BUILTIN
     * is excluded too -- an explicit "=b" asks for the tree's builtin and must
     * still get it.
     */
    if (is_system_dir && sysdir_machine == IMAGE_FILE_MACHINE_AMD64 &&
        sysdir_machine != current_machine &&
        pe_mapping->image.machine == sysdir_machine && loadorder != LO_BUILTIN)
    {
        TRACE( "%s is staged in the %04x system directory; keeping it rather "
               "than re-resolving the name against the builtins\n",
               debugstr_us(&pe_mapping->nt_name), sysdir_machine );
        return STATUS_IMAGE_ALREADY_LOADED;
    }

    if (pe_mapping->image.wine_builtin)
    {
        if (loadorder == LO_NATIVE) return STATUS_DLL_NOT_FOUND;
        loadorder = LO_BUILTIN_NATIVE;  /* load builtin, then fallback to the file we found */
    }
    else if (pe_mapping->image.wine_fakedll)
    {
        TRACE( "%s is a fake Wine dll\n", debugstr_us(&pe_mapping->nt_name) );
        if (loadorder == LO_NATIVE) return STATUS_DLL_NOT_FOUND;
        loadorder = LO_BUILTIN;  /* builtin with no fallback since mapping a fake dll is not useful */
    }

    if (is_arm64ec() && pe_mapping->image.is_hybrid && search_machine == IMAGE_FILE_MACHINE_AMD64)
        search_machine = current_machine;

    switch (loadorder)
    {
    case LO_NATIVE:
    case LO_NATIVE_BUILTIN:
        return STATUS_IMAGE_ALREADY_LOADED;
    case LO_BUILTIN:
        return find_builtin_dll( &pe_mapping->nt_name, &pe_mapping->exp_name, module, size, info,
                                 limit_low, limit_high, search_machine, machine, FALSE, offset );
    default:
        status = find_builtin_dll( &pe_mapping->nt_name, &pe_mapping->exp_name, module, size, info,
                                   limit_low, limit_high, search_machine, machine,
                                   (loadorder == LO_DEFAULT), offset );
        if (status == STATUS_DLL_NOT_FOUND || status == STATUS_NOT_SUPPORTED)
            return STATUS_IMAGE_ALREADY_LOADED;
        return status;
    }
}


/***********************************************************************
 *           load_unixlib_by_name
 */
NTSTATUS load_unixlib_by_name( const UNICODE_STRING *nt_name, void **handle_ret )
{
    unsigned int i, pos, maxlen = 0;
    unsigned int len = nt_name->Length / sizeof(WCHAR);
    const char *so_dir = get_so_dir( current_machine );
    char *ptr = NULL, *file, *ext = NULL;
    void *handle = NULL;

    if (!len) return STATUS_DLL_NOT_FOUND;

    for (i = 0; i < len; i++) if (nt_name->Buffer[i] == '/' || nt_name->Buffer[i] == '\\') break;

    if (i < len)  /* explicit path */
    {
        UNICODE_STRING true_nt_name;
        OBJECT_ATTRIBUTES attr;

        InitializeObjectAttributes( &attr, (UNICODE_STRING *)nt_name, 0, 0, NULL );
        if (!get_nt_and_unix_names( &attr, &true_nt_name, &file, FILE_OPEN, FALSE ))
            handle = dlopen( file, RTLD_NOW );
        free( true_nt_name.Buffer );
        goto done;
    }

    if (build_dir) maxlen = strlen(build_dir) + sizeof("/dlls/") + len;
    maxlen = max( maxlen, dll_path_maxlen + 1 ) + len + sizeof("/aarch64-unix") + sizeof(".so");

    if (!(file = malloc( maxlen ))) return STATUS_NO_MEMORY;

    pos = maxlen - len - 4;
    ext = file + pos + len;
    /* we don't want to depend on the current codepage here */
    for (i = 0; i < len; i++)
    {
        if (nt_name->Buffer[i] > 127) goto done;
        file[pos + i] = (char)nt_name->Buffer[i];
        if (file[pos + i] >= 'A' && file[pos + i] <= 'Z') file[pos + i] += 'a' - 'A';
        else if (file[pos + i] == '.') ext = file + pos + i;
    }
    file[pos + len] = 0;
    file[--pos] = '/';

    if (build_dir)
    {
        ptr = prepend_build_dir_path( file + pos, ".so", "", "/dlls", build_dir );
        strcpy( ext, ".so" );
        if ((handle = dlopen( ptr, RTLD_NOW ))) goto done;
    }

    strcpy( ext, ".so" );
    for (i = 0; dll_paths[i]; i++)
    {
        ptr = prepend( file + pos, so_dir, strlen(so_dir) );
        ptr = prepend( ptr, dll_paths[i], strlen(dll_paths[i]) );
        if ((handle = dlopen( ptr, RTLD_NOW ))) goto done;

        ptr = prepend( file + pos, dll_paths[i], strlen(dll_paths[i]) );
        if ((handle = dlopen( ptr, RTLD_NOW ))) goto done;
    }

 done:
    free( file );
    if (!handle) return STATUS_DLL_NOT_FOUND;
    *handle_ret = handle;
    return STATUS_SUCCESS;
}


/***************************************************************************
 *	get_machine_wow64_dir
 *
 * cf. GetSystemWow64Directory2.
 */
static const WCHAR *get_machine_wow64_dir( WORD machine )
{
    static const WCHAR system32[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m','3','2','\\',0};
    static const WCHAR syswow64[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','w','o','w','6','4','\\',0};
    static const WCHAR sysarm32[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','a','r','m','3','2','\\',0};
    static const WCHAR sysx8664[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','x','8','6','6','4','\\',0};

    if (machine == native_machine) machine = IMAGE_FILE_MACHINE_TARGET_HOST;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_TARGET_HOST: return system32;
    case IMAGE_FILE_MACHINE_I386:        return syswow64;
    case IMAGE_FILE_MACHINE_ARMNT:       return sysarm32;
    /* x86-64 as a non-native machine, i.e. emulated: it needs a system
     * directory of its own so that its modules are a separate universe from
     * the native ones, exactly as syswow64 is for i386.  Unreachable when
     * x86-64 is native, since that is mapped to TARGET_HOST above.  On ARM64
     * the pair belongs to ARM64EC instead, whose x86-64 modules are hybrid
     * images living in system32, so leave that alone.  Must agree with
     * get_machine_system_dir() in the PE loader. */
    case IMAGE_FILE_MACHINE_AMD64:
        if (native_machine == IMAGE_FILE_MACHINE_ARM64) return NULL;
        return sysx8664;
    default: return NULL;
    }
}


/***************************************************************************
 *	is_system_dir_path
 *
 * Check if path is inside a system directory, to support loading builtins
 * when the corresponding file doesn't exist yet.
 */
BOOL is_system_dir_path( const UNICODE_STRING *path, WORD *machine )
{
    unsigned int i, len = path->Length / sizeof(WCHAR), dirlen;
    const WCHAR *sysdir, *p = path->Buffer;

    for (i = 0; i < supported_machines_count; i++)
    {
        sysdir = get_machine_wow64_dir( supported_machines[i] );
        if (!sysdir) continue;
        dirlen = wcslen( sysdir );
        if (len <= dirlen) continue;
        if (wcsnicmp( p, sysdir, dirlen )) continue;
        /* check for remaining path components */
        for (p += dirlen, len -= dirlen; len; p++, len--) if (*p == '\\') return FALSE;
        *machine = supported_machines[i];
        return TRUE;
    }
    return FALSE;
}


/***********************************************************************
 *           open_main_image
 */
static NTSTATUS open_main_image( UNICODE_STRING *nt_name, void **module, SECTION_IMAGE_INFORMATION *info,
                                 enum loadorder loadorder, USHORT machine )
{
    OBJECT_ATTRIBUTES attr;
    SIZE_T size = 0;
    char *unix_name;
    NTSTATUS status;
    HANDLE mapping;
    UNICODE_STRING true_nt_name;

    if (loadorder == LO_DISABLED) NtTerminateProcess( GetCurrentProcess(), STATUS_DLL_NOT_FOUND );

    InitializeObjectAttributes( &attr, nt_name, OBJ_CASE_INSENSITIVE, 0, NULL );
    if (get_nt_and_unix_names( &attr, &true_nt_name, &unix_name, FILE_OPEN, FALSE )) return STATUS_DLL_NOT_FOUND;

    status = open_dll_file( unix_name, &attr, &mapping );
    if (!status)
    {
        status = virtual_map_module( mapping, module, &size, info, 0, 0, machine );
        if (status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH && info->ComPlusNativeReady)
        {
            info->Machine = is_machine_64bit( native_machine ) ? IMAGE_FILE_MACHINE_AMD64 : native_machine;
            status = STATUS_SUCCESS;
        }
        NtClose( mapping );
    }
    else if (status == STATUS_INVALID_IMAGE_NOT_MZ && loadorder != LO_NATIVE)
    {
        status = open_main_image_so_file( unix_name, attr.ObjectName, module, info );
    }
    free( unix_name );
    free( true_nt_name.Buffer );
    return status;
}


/***********************************************************************
 *           load_main_exe
 */
NTSTATUS load_main_exe( UNICODE_STRING *nt_name, USHORT load_machine, void **module )
{
    unsigned int status;
    SIZE_T size;
    USHORT search_machine;
    BOOL is_system_dir = is_system_dir_path( nt_name, &search_machine );
    enum loadorder loadorder = get_load_order( nt_name, is_system_dir, NULL );

    status = open_main_image( nt_name, module, &main_image_info, loadorder, load_machine );

    switch (status)
    {
    case STATUS_DLL_NOT_FOUND:
    case STATUS_INVALID_IMAGE_FORMAT:
    case STATUS_NOT_SUPPORTED:
        /* if path is in system dir, we can load the builtin even if the file itself doesn't exist */
        if (loadorder != LO_NATIVE && is_prefix_bootstrap && is_system_dir)
            status = find_builtin_dll( nt_name, NULL, module, &size, &main_image_info, 0, 0,
                                       search_machine, load_machine, FALSE, 0 );
        break;
    }
    return status;
}


/***********************************************************************
 *           load_start_exe
 *
 * Load start.exe as main image.
 */
NTSTATUS load_start_exe( UNICODE_STRING *nt_name, void **module )
{
    static const WCHAR startW[] = {'s','t','a','r','t','.','e','x','e',0};
    unsigned int status;
    SIZE_T size;
    WCHAR *image = malloc( sizeof("\\??\\C:\\windows\\system32\\start.exe") * sizeof(WCHAR) );

    wcscpy( image, get_machine_wow64_dir( current_machine ));
    wcscat( image, startW );
    init_unicode_string( nt_name, image );
    status = find_builtin_dll( nt_name, NULL, module, &size, &main_image_info, 0, 0, current_machine, 0, FALSE, 0 );
    if (!NT_SUCCESS(status))
    {
        MESSAGE( "wine: failed to load start.exe: %x\n", status );
        NtTerminateProcess( GetCurrentProcess(), status );
    }
    return status;
}

static ULONG_PTR find_ordinal_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports, DWORD ordinal )
{
    const DWORD *functions = (const DWORD *)((BYTE *)module + exports->AddressOfFunctions);

    if (ordinal >= exports->NumberOfFunctions) return 0;
    if (!functions[ordinal]) return 0;
    return (ULONG_PTR)module + functions[ordinal];
}

static ULONG_PTR find_named_export( HMODULE module, const IMAGE_EXPORT_DIRECTORY *exports,
                                    const char *name )
{
    const WORD *ordinals = (const WORD *)((BYTE *)module + exports->AddressOfNameOrdinals);
    const DWORD *names = (const DWORD *)((BYTE *)module + exports->AddressOfNames);
    int min = 0, max = exports->NumberOfNames - 1;

    while (min <= max)
    {
        int res, pos = (min + max) / 2;
        char *ename = (char *)module + names[pos];
        if (!(res = strcmp( ename, name ))) return find_ordinal_export( module, exports, ordinals[pos] );
        if (res > 0) max = pos - 1;
        else min = pos + 1;
    }
    return 0;
}

static inline void *get_rva( void *module, ULONG_PTR addr )
{
    return (BYTE *)module + addr;
}

static const void *get_module_data_dir( HMODULE module, ULONG dir, ULONG *size )
{
    const IMAGE_NT_HEADERS *nt = get_rva( module, ((IMAGE_DOS_HEADER *)module)->e_lfanew );
    const IMAGE_DATA_DIRECTORY *data;

    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        data = &((const IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.DataDirectory[dir];
    else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        data = &((const IMAGE_NT_HEADERS32 *)nt)->OptionalHeader.DataDirectory[dir];
    else
        return NULL;
    if (!data->VirtualAddress || !data->Size) return NULL;
    if (size) *size = data->Size;
    return get_rva( module, data->VirtualAddress );
}

/***********************************************************************
 *           load_ntdll_functions
 */
static void load_ntdll_functions( HMODULE module )
{
    void **p__wine_syscall_dispatcher;
    void **p__wine_unix_call_dispatcher;
    void **p__wine_unix_call_dispatcher_arm64ec = NULL;
    unixlib_handle_t *p__wine_unixlib_handle;
    const IMAGE_EXPORT_DIRECTORY *exports;

    exports = get_module_data_dir( module, IMAGE_DIRECTORY_ENTRY_EXPORT, NULL );
    assert( exports );

#define GET_FUNC(name) \
    if (!(p##name = (void *)find_named_export( module, exports, #name ))) \
        ERR( "%s not found\n", #name )

    GET_FUNC( DbgUiRemoteBreakin );
    GET_FUNC( KiRaiseUserExceptionDispatcher );
    GET_FUNC( KiUserExceptionDispatcher );
    GET_FUNC( KiUserApcDispatcher );
    GET_FUNC( KiUserCallbackDispatcher );
    GET_FUNC( LdrInitializeThunk );
    GET_FUNC( LdrSystemDllInitBlock );
    GET_FUNC( RtlUserThreadStart );
    GET_FUNC( __wine_ctrl_routine );
#ifdef __powerpc64__
    /* PowerPC64 has no spare register for the TEB: r13 is glibc's thread
     * pointer and r2 is the TOC.  The PE side keeps it in an initial-exec
     * thread-local that only it can write, so the unix side has to hand the
     * TEB over once per thread.  See dlls/ntdll/signal_ppc64.c. */
    GET_FUNC( __wine_init_teb );
#endif
    GET_FUNC( __wine_syscall_dispatcher );
    GET_FUNC( __wine_unix_call_dispatcher );
    GET_FUNC( __wine_unixlib_handle );
    if (is_arm64ec())
    {
        GET_FUNC( __wine_unix_call_dispatcher_arm64ec );
        GET_FUNC( KiUserEmulationDispatcher );
    }
    *p__wine_syscall_dispatcher = __wine_syscall_dispatcher;
    *p__wine_unixlib_handle = (UINT_PTR)unix_call_funcs;
    if (p__wine_unix_call_dispatcher_arm64ec)
    {
        /* redirect __wine_unix_call_dispatcher to __wine_unix_call_dispatcher_arm64ec */
        *p__wine_unix_call_dispatcher = *p__wine_unix_call_dispatcher_arm64ec;
        *p__wine_unix_call_dispatcher_arm64ec = __wine_unix_call_dispatcher;
    }
    else *p__wine_unix_call_dispatcher = __wine_unix_call_dispatcher;
#undef GET_FUNC
}


/***********************************************************************
 *           load_ntdll_wow64_functions
 */
static void load_ntdll_wow64_functions( HMODULE module )
{
    const IMAGE_EXPORT_DIRECTORY *exports;

    exports = get_module_data_dir( module, IMAGE_FILE_EXPORT_DIRECTORY, NULL );
    assert( exports );

    pLdrSystemDllInitBlock->ntdll_handle = (ULONG_PTR)module;

#define GET_FUNC(name) pLdrSystemDllInitBlock->p##name = find_named_export( module, exports, #name )
    GET_FUNC( KiUserApcDispatcher );
    GET_FUNC( KiUserCallbackDispatcher );
    GET_FUNC( KiUserExceptionDispatcher );
    GET_FUNC( LdrInitializeThunk );
    GET_FUNC( LdrSystemDllInitBlock );
    GET_FUNC( RtlUserThreadStart );
    GET_FUNC( RtlpFreezeTimeBias );
    GET_FUNC( RtlpQueryProcessDebugInformationRemote );
#undef GET_FUNC

    p__wine_ctrl_routine = (void *)find_named_export( module, exports, "__wine_ctrl_routine" );

#ifdef _WIN64
    {
        unixlib_handle_t *p__wine_unixlib_handle = (void *)find_named_export( module, exports,
                                                                              "__wine_unixlib_handle" );
        *p__wine_unixlib_handle = (UINT_PTR)unix_call_wow64_funcs;
    }
#endif

    /* also set the 32-bit LdrSystemDllInitBlock */
    memcpy( (void *)(ULONG_PTR)pLdrSystemDllInitBlock->pLdrSystemDllInitBlock,
            pLdrSystemDllInitBlock, sizeof(*pLdrSystemDllInitBlock) );
}


/***********************************************************************
 *           redirect_arm64ec_rva
 *
 * Redirect an address through the arm64ec redirection table.
 */
ULONG_PTR redirect_arm64ec_rva( void *base, ULONG_PTR rva, const IMAGE_ARM64EC_METADATA *metadata )
{
    const IMAGE_ARM64EC_REDIRECTION_ENTRY *map = get_rva( base, metadata->RedirectionMetadata );
    int min = 0, max = metadata->RedirectionMetadataCount - 1;

    while (min <= max)
    {
        int pos = (min + max) / 2;
        if (map[pos].Source == rva) return map[pos].Destination;
        if (map[pos].Source < rva) min = pos + 1;
        else max = pos - 1;
    }
    return rva;
}


/***********************************************************************
 *           redirect_ntdll_functions
 *
 * Redirect ntdll functions on arm64ec.
 */
static void redirect_ntdll_functions( HMODULE module )
{
    const IMAGE_LOAD_CONFIG_DIRECTORY *loadcfg;
    const IMAGE_ARM64EC_METADATA *metadata;

    if (!(loadcfg = get_module_data_dir( module, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, NULL ))) return;
    if (!(metadata = (void *)loadcfg->CHPEMetadataPointer)) return;
#define REDIRECT(name) \
    p##name = get_rva( module, redirect_arm64ec_rva( module, (char *)p##name - (char *)module, metadata ))
    REDIRECT( DbgUiRemoteBreakin );
    REDIRECT( KiRaiseUserExceptionDispatcher );
    REDIRECT( KiUserExceptionDispatcher );
    REDIRECT( KiUserApcDispatcher );
    REDIRECT( KiUserCallbackDispatcher );
    REDIRECT( KiUserEmulationDispatcher );
    REDIRECT( LdrInitializeThunk );
    REDIRECT( RtlUserThreadStart );
#undef REDIRECT
}


/***********************************************************************
 *           load_ntdll
 */
static void load_ntdll(void)
{
    static WCHAR path[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\',
                           's','y','s','t','e','m','3','2','\\','n','t','d','l','l','.','d','l','l',0};
    const char *pe_dir = get_pe_dir( current_machine );
    USHORT machine = current_machine;
    unsigned int status;
    SECTION_IMAGE_INFORMATION info;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING str;
    void *module;
    SIZE_T size = 0;
    char *name = NULL;

    init_unicode_string( &str, path );
    InitializeObjectAttributes( &attr, &str, 0, 0, NULL );

    if (build_dir) asprintf( &name, "%s%s/ntdll.dll", ntdll_dir, pe_dir );
    else asprintf( &name, "%s%s/ntdll.dll", dll_dir, pe_dir );

    if (is_arm64ec()) machine = main_image_info.Machine;
    status = open_builtin_pe_file( name, &attr, &module, &size, &info, 0, 0, machine, FALSE, 0 );
    if (status == STATUS_DLL_NOT_FOUND)
    {
        free( name );
        asprintf( &name, "%s/ntdll.dll%c.so", ntdll_dir, 0 );
        status = open_builtin_so_file( name, &attr, &module, &info, machine, 0, FALSE );
    }
    if (status == STATUS_IMAGE_NOT_AT_BASE) status = virtual_relocate_module( module );
    if (status) fatal_error( "failed to load %s error %x\n", name, status );
    free( name );
    load_ntdll_functions( module );
    if (is_arm64ec()) redirect_ntdll_functions( module );
}


/***********************************************************************
 *           load_apiset_dll
 */
static void load_apiset_dll(void)
{
    static WCHAR path[] = {'\\','?','?','\\','C',':','\\','w','i','n','d','o','w','s','\\',
                           's','y','s','t','e','m','3','2','\\',
                           'a','p','i','s','e','t','s','c','h','e','m','a','.','d','l','l',0};
    const char *pe_dir = get_pe_dir( current_machine );
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_SECTION_HEADER *sec;
    API_SET_NAMESPACE *map;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING str;
    unsigned int status;
    HANDLE handle, mapping;
    SIZE_T size;
    char *name = NULL;
    void *ptr;
    UINT i;

    init_unicode_string( &str, path );
    InitializeObjectAttributes( &attr, &str, 0, 0, NULL );

    if (build_dir) asprintf( &name, "%s/dlls/apisetschema%s/apisetschema.dll", build_dir, pe_dir );
    else asprintf( &name, "%s%s/apisetschema.dll", dll_dir, pe_dir );
    status = open_unix_file( &handle, name, GENERIC_READ | SYNCHRONIZE, &attr, 0,
                             FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
                             FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0 );
    free( name );

    if (!status)
    {
        status = NtCreateSection( &mapping, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY | SECTION_MAP_READ,
                                  NULL, NULL, PAGE_READONLY, SEC_COMMIT, handle );
        NtClose( handle );
    }
    if (!status)
    {
        status = map_section( mapping, &ptr, &size, PAGE_READONLY );
        NtClose( mapping );
    }
    if (!status)
    {
        nt = get_rva( ptr, ((IMAGE_DOS_HEADER *)ptr)->e_lfanew );
        sec = IMAGE_FIRST_SECTION( nt );

        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        {
            if (memcmp( (char *)sec->Name, ".apiset", 8 )) continue;
            map = (API_SET_NAMESPACE *)((char *)ptr + sec->PointerToRawData);
            if (sec->PointerToRawData < size &&
                size - sec->PointerToRawData >= sec->Misc.VirtualSize &&
                map->Version == 6 &&
                map->Size <= sec->Misc.VirtualSize)
            {
                peb->ApiSetMap = map;
                if (wow_peb) wow_peb->ApiSetMap = PtrToUlong(map);
                TRACE( "loaded %s apiset at %p\n", debugstr_w(path), map );
                return;
            }
            break;
        }
        NtUnmapViewOfSection( NtCurrentProcess(), ptr );
        status = STATUS_APISET_NOT_PRESENT;
    }
    ERR( "failed to load apiset: %x\n", status );
}


/***********************************************************************
 *           load_wow64_ntdll
 */
static void load_wow64_ntdll( USHORT machine )
{
    static const WCHAR ntdllW[] = {'n','t','d','l','l','.','d','l','l',0};
    SECTION_IMAGE_INFORMATION info;
    UNICODE_STRING nt_name;
    void *module;
    unsigned int status;
    SIZE_T size;
    const WCHAR *wow64_dir;
    WCHAR *path;

    if (machine == current_machine) return;
    /* WoW64 is 32-on-64 by construction.  A guest machine of the same word
     * size as ours is served by an embedded emulator instead, and has no
     * ntdll of its own to load -- its modules are thunks that call the native
     * ones.  Without this, adding a system directory for such a machine (see
     * get_machine_wow64_dir) makes this fatal_error on every guest image. */
    if (is_machine_64bit( machine ) == is_machine_64bit( current_machine )) return;
    if (!(wow64_dir = get_machine_wow64_dir( machine ))) return;

    path = malloc( sizeof("\\??\\C:\\windows\\system32\\ntdll.dll") * sizeof(WCHAR) );
    wcscpy( path, wow64_dir );
    wcscat( path, ntdllW );
    init_unicode_string( &nt_name, path );
    status = find_builtin_dll( &nt_name, NULL, &module, &size, &info, 0, 0, machine, 0, FALSE, 0 );
    if (status == STATUS_IMAGE_NOT_AT_BASE) status = virtual_relocate_module( module );
    if (status) fatal_error( "failed to load %s error %x\n", debugstr_w(path), status );
    load_ntdll_wow64_functions( module );
    TRACE("loaded %s at %p\n", debugstr_w(path), module );
    free( path );
}


/***********************************************************************
 *           get_image_address
 */
static ULONG_PTR get_image_address(void)
{
#ifdef HAVE_GETAUXVAL
    ULONG_PTR size, num, phdr_addr = getauxval( AT_PHDR );
    ElfW(Phdr) *phdr;

    if (!phdr_addr) return 0;
    phdr = (ElfW(Phdr) *)phdr_addr;
    size = getauxval( AT_PHENT );
    num = getauxval( AT_PHNUM );
    while (num--)
    {
        if (phdr->p_type == PT_PHDR) return phdr_addr - phdr->p_offset;
        phdr = (ElfW(Phdr) *)((char *)phdr + size);
    }
#elif defined(__APPLE__) && defined(TASK_DYLD_INFO)
    struct task_dyld_info dyld_info;
    mach_msg_type_number_t size = TASK_DYLD_INFO_COUNT;

    if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&dyld_info, &size) == KERN_SUCCESS)
        return dyld_info.all_image_info_addr;
#endif
    return 0;
}

/***********************************************************************
 *           start_main_thread
 */
static void start_main_thread(void)
{
    TEB *teb = virtual_alloc_first_teb();

    dbg_init();
    startup_info_size = server_init_process();
    virtual_map_user_shared_data();
    init_cpu_info();
    init_files();
    init_startup_info();
    *(ULONG_PTR *)&peb->CloudFileFlags = get_image_address();
    set_load_order_app_name( main_wargv[0] );
    init_thread_stack( teb, 0, 0, 0 );
    NtCreateKeyedEvent( &keyed_event, GENERIC_READ | GENERIC_WRITE, NULL, 0 );
    load_ntdll();
    load_wow64_ntdll( main_image_info.Machine );
    load_apiset_dll();
    server_init_process_done();
}


#ifdef __APPLE__
static void *apple_wine_thread( void *arg )
{
    start_main_thread();
    return NULL;
}

/***********************************************************************
 *           apple_create_wine_thread
 *
 * Spin off a secondary thread to complete Wine initialization, leaving
 * the original thread for the Mac frameworks.
 *
 * Invoked as a CFRunLoopSource perform callback.
 */
static void apple_create_wine_thread( void *arg )
{
    pthread_t thread;
    pthread_attr_t attr;

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE );
    if (pthread_create( &thread, &attr, apple_wine_thread, NULL )) exit(1);
    pthread_attr_destroy( &attr );
}


/***********************************************************************
 *           apple_main_thread
 *
 * Park the process's original thread in a Core Foundation run loop for
 * use by the Mac frameworks, especially receiving and handling
 * distributed notifications.  Spin off a new thread for the rest of the
 * Wine initialization.
 */
static void apple_main_thread(void)
{
    CFRunLoopSourceContext source_context = { 0 };
    CFRunLoopSourceRef source;

    if (!pthread_main_np()) return;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    /* Multi-processing Services can get confused about the main thread if the
     * first time it's used is on a secondary thread.  Use it here to make sure
     * that doesn't happen. */
    MPTaskIsPreemptive(MPCurrentTaskID());
#pragma clang diagnostic pop

    /* Give ourselves the best chance of having the distributed notification
     * center scheduled on this thread's run loop.  In theory, it's scheduled
     * in the first thread to ask for it. */
    CFNotificationCenterGetDistributedCenter();

    /* We use this run loop source for two purposes.  First, a run loop exits
     * if it has no more sources scheduled.  So, we need at least one source
     * to keep the run loop running.  Second, although it's not critical, it's
     * preferable for the Wine initialization to not proceed until we know
     * the run loop is running.  So, we signal our source immediately after
     * adding it and have its callback spin off the Wine thread. */
    source_context.perform = apple_create_wine_thread;
    source = CFRunLoopSourceCreate( NULL, 0, &source_context );
    CFRunLoopAddSource( CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes );
    CFRunLoopSourceSignal( source );
    CFRelease( source );
    CFRunLoopRun(); /* Should never return, except on error. */
}
#endif  /* __APPLE__ */


#if defined(__linux__) && !defined(__ANDROID__) && (defined(__i386__) || defined(__arm__))

static void check_vmsplit( void *stack )
{
    if (stack < (void *)0x80000000)
    {
        /* if the stack is below 0x80000000, assume we can safely try a munmap there */
        if (munmap( (void *)0x80000000, 1 ) == -1 && errno == EINVAL)
            ERR( "Warning: memory above 0x80000000 doesn't seem to be accessible.\n"
                 "Wine requires a 3G/1G user/kernel memory split to work properly.\n" );
    }
}

static int pre_exec(void)
{
    int temp;

    check_vmsplit( &temp );
    return 1;  /* we have a preloader on x86/arm */
}

#elif (defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__DragonFly__))

static int pre_exec(void)
{
    struct rlimit rl;

    rl.rlim_cur = 0x02000000;
    rl.rlim_max = 0x02000000;
    setrlimit( RLIMIT_DATA, &rl );
    return 1;
}

#elif defined(__APPLE__)

static int pre_exec(void)
{
    if (build_dir)
    {
        char *path = getenv( "DYLD_LIBRARY_PATH" );
        if (path) asprintf( &path, "%s/dlls/ntdll:%s/dlls/win32u:%s", build_dir, build_dir, path );
        else asprintf( &path, "%s/dlls/ntdll:%s/dlls/win32u", build_dir, build_dir );
        setenv( "DYLD_LIBRARY_PATH", path, 1 );
        return 1;
    }
#ifdef HAVE_WINE_PRELOADER
    return 1;
#else
    return 0;
#endif
}

#else

static int pre_exec(void)
{
#ifdef HAVE_WINE_PRELOADER
    return 1;  /* we have a preloader */
#else
    return 0;  /* no exec needed */
#endif
}

#endif


static void reexec_loader( int argc, char *argv[], char *extra_arg )
{
    WORD machine = current_machine;
    char **new_argv;

    /* have to exec if we have a preloader, or an argument, or if we are the initial wrapper */
    if (!pre_exec() && !extra_arg && dlsym( RTLD_DEFAULT, "wine_main_preload_info" )) return;

    if (extra_arg)
    {
        new_argv = malloc( (argc + 3) * sizeof(*argv) );
        memcpy( new_argv + 3, argv + 1, argc * sizeof(*argv) );
        new_argv[2] = extra_arg;
    }
    else
    {
        new_argv = malloc( (argc + 2) * sizeof(*argv) );
        memcpy( new_argv + 2, argv + 1, argc * sizeof(*argv) );
    }

    /* default to 32-bit loader to support 32-bit prefixes */
    if (machine == IMAGE_FILE_MACHINE_AMD64) machine = IMAGE_FILE_MACHINE_I386;

    loader_exec( new_argv, machine );
    fatal_error( "could not exec the wine loader\n" );
}

/***********************************************************************
 *           check_command_line
 *
 * Check if command line is one that needs to be handled specially.
 */
static void check_command_line( int argc, char *argv[] )
{
    char *basename;
    static const char usage[] =
        "Usage: wine PROGRAM [ARGUMENTS...]   Run the specified program\n"
        "       wine --help                   Display this help and exit\n"
        "       wine --version                Output version information and exit";

    if ((basename = strrchr( argv[0], '/' ))) basename++;
    else basename = argv[0];

    if (strcmp( basename, "wine" )) /* check if there's a builtin exe corresponding to the base name */
    {
        const char *pe_dir = get_pe_dir( current_machine );
        char *exe;

        if (build_dir)
        {
            asprintf( &exe, "%s/programs/%s%s/%s.exe", build_dir, basename, pe_dir, basename );
            if (!access( exe, R_OK )) reexec_loader( argc, argv, basename );
            free( exe );
        }
        else
        {
            for (int i = 0; dll_paths[i]; i++)
            {
                asprintf( &exe, "%s%s/%s.exe", dll_paths[i], pe_dir, basename );
                if (!access( exe, R_OK )) reexec_loader( argc, argv, basename );
                free( exe );
            }
        }
    }

    if (argc <= 1)
    {
        fprintf( stderr, "%s\n", usage );
        exit(1);
    }
    if (!strcmp( argv[1], "--help" ))
    {
        printf( "%s\n", usage );
        exit(0);
    }
    if (!strcmp( argv[1], "--version" ))
    {
        printf( "%s\n", wine_build );
        exit(0);
    }

    reexec_loader( argc, argv, NULL );
}


/***********************************************************************
 *           __wine_main
 *
 * Main entry point called by the wine loader.
 */
DECLSPEC_EXPORT void __wine_main( int argc, char *argv[] )
{
    main_argc = argc;
    main_argv = argv;

    init_paths();
    if (!getenv( "WINELOADERNOEXEC" ) || argc <= 1) check_command_line( argc, argv );
    unsetenv( "WINELOADERNOEXEC" );

#ifdef RLIMIT_NOFILE
    set_max_limit( RLIMIT_NOFILE );
#endif
#ifdef RLIMIT_AS
    set_max_limit( RLIMIT_AS );
#endif
#ifdef RLIMIT_NICE
    set_max_limit( RLIMIT_NICE );
#endif

    virtual_init();
    init_environment();

#ifdef __APPLE__
    apple_main_thread();
#endif
    start_main_thread();
}
