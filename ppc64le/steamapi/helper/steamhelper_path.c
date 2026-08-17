/*
 * steamhelper_path.c -- DOS <-> unix file name translation for a helper
 * process that has no Wine prefix of its own.
 *
 * WHY THIS FILE EXISTS.  Proton's unix half converts file names in both
 * directions and does it in 85 of the vendored wrappers: a game hands
 * ISteamRemoteStorage / ISteamUGC / ISteamScreenshots a DOS path and the
 * wrapper calls steamclient_dos_to_unix_path() before passing it to the Steam
 * client, and ISteamApps::GetAppInstallDir / ISteamUGC::GetItemInstallInfo
 * come back the other way through steamclient_unix_path_to_dos_path().  Both
 * bottom out in two ntdll entry points that, inside Wine, read the calling
 * process's own prefix.  This helper is a separate Linux process with no
 * prefix, so both used to be refused by name and every path crossed
 * unconverted -- which for a cloud save means the Steam client is handed a
 * literal "C:\users\...\save.dat" to open, and for an install directory means
 * the game is handed a unix path it will never open.
 *
 * WHERE THE MAPPING COMES FROM, AND WHY NOT FROM HERE.  The drive map is the
 * client's to know: the game's process is the only party that knows WHICH
 * prefix these paths belong to, and it can ask Wine itself (kernel32's
 * wine_get_unix_file_name) rather than parse dosdevices or trust $WINEPREFIX
 * in an environment that was never guaranteed to match the game's.  So the
 * client measures the map once per connection and sends it
 * (STEAMRPC_CODE_SETDRIVES), and this file does the joining -- which has to
 * happen here, because the conversion calls are made from deep inside a
 * Steamworks wrapper that the client half never sees.
 *
 * WHAT IS FAITHFUL AND WHAT IS NOT.  Wine resolves each path component
 * case-insensitively against a case-sensitive filesystem, and games do spell
 * "C:\Users\..." for a directory that is really "users"; that walk is
 * reproduced here.  Wine's answer for a name that does not exist yet is
 * STATUS_NO_SUCH_FILE plus a usable path -- which is what makes creating a
 * save file work -- and that is reproduced too, including the distinction
 * from a missing DIRECTORY, which stays a hard failure rather than becoming a
 * write into nowhere.  What is not reproduced: nt_to_unix_file_name's full
 * NT-namespace surface (devices, UNC, \\?\ syntax).  Proton only ever passes
 * \??\<letter>: paths it built itself, and anything else is refused by name
 * here rather than guessed at.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"

#include "steamhelper.h"

/* ------------------------------------------------------------ the drives */

struct drive
{
    char letter;
    char *root;      /* canonical unix path, no trailing slash */
    size_t rootlen;
};

static struct drive drives[STEAMRPC_DRIVE_MAX];
static unsigned int ndrives;

unsigned int steamhelper_drive_count( void )
{
    return ndrives;
}

/* Longest root first, so that a unix path under C: is answered as C: and not
 * as Z: -- Z: is the whole filesystem in every Wine prefix, so every path
 * matches it and only the longer match is the right one. */
static int by_rootlen( const void *a, const void *b )
{
    const struct drive *x = (const struct drive *)a, *y = (const struct drive *)b;

    if (x->rootlen != y->rootlen) return x->rootlen < y->rootlen ? 1 : -1;
    return (int)(unsigned char)x->letter - (int)(unsigned char)y->letter;
}

unsigned int steamhelper_set_drives( const struct steamrpc_drive *in, unsigned int count )
{
    char resolved[PATH_MAX];
    unsigned int i;

    for (i = 0; i < ndrives; i++) free( drives[i].root );
    ndrives = 0;

    for (i = 0; i < count && ndrives < STEAMRPC_DRIVE_MAX; i++)
    {
        const char *root = in[i].root;
        char letter = in[i].letter;
        size_t len;

        /* The record is fixed-length on the wire; a root with no terminator
         * is a frame this end does not understand, not a long path. */
        if (letter < 'A' || letter > 'Z' ||
            !memchr( root, 0, STEAMRPC_DRIVE_ROOT_LEN ) || !*root)
        {
            fprintf( stderr, "helper: drive record %u is not a letter and a "
                     "NUL-terminated unix path; dropping it\n", i );
            continue;
        }

        /* Canonicalise.  Wine names a drive root through its dosdevices
         * symlink ("<prefix>/dosdevices/c:"), and the Steam client names the
         * same files through their real path ("<prefix>/drive_c/..."); with
         * the link left unresolved the two would never match and every
         * unix->DOS conversion would come back as Z:\<the whole path>. */
        if (realpath( root, resolved )) root = resolved;
        len = strlen( root );
        while (len > 1 && root[len - 1] == '/') len--;

        if (!(drives[ndrives].root = (char *)malloc( len + 1 ))) break;
        memcpy( drives[ndrives].root, root, len );
        drives[ndrives].root[len] = 0;
        drives[ndrives].rootlen = len;
        drives[ndrives].letter = letter;
        steamhelper_log( "drive %c: -> %s\n", letter, drives[ndrives].root );
        ndrives++;
    }

    qsort( drives, ndrives, sizeof(drives[0]), by_rootlen );
    return ndrives;
}

static const struct drive *find_drive( char letter )
{
    unsigned int i;

    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    for (i = 0; i < ndrives; i++)
        if (drives[i].letter == letter) return &drives[i];
    return NULL;
}

static void no_drives_once( void )
{
    static int said;

    if (said) return;
    said = 1;
    fprintf( stderr, "helper: no drive map has arrived on this connection, so "
             "no file path can be converted; the client sends one at connect "
             "time -- see STEAMRPC_CODE_SETDRIVES\n" );
}

/* -------------------------------------------------------------- DOS -> unix
 *
 * The input is an NT path, because that is what Proton builds: it prefixes
 * "\??\", collapses the "." and ".." components itself, and hands the result
 * to ntdll_get_unix_file_name.  See collapse_path() and get_unix_file_name()
 * in the vendored unixlib.cpp.
 */

/* Append one path component, resolving its case against what is really on
 * disk the way Wine's own lookup does.  Returns 1 when the component exists. */
static int append_component( char *path, size_t *len, size_t max,
                             const char *comp, size_t clen )
{
    struct dirent *ent;
    struct stat st;
    size_t base;
    DIR *dir;

    if (*len + 1 + clen + 1 > max) return 0;
    base = *len;
    path[base] = '/';
    memcpy( path + base + 1, comp, clen );
    path[base + 1 + clen] = 0;
    *len = base + 1 + clen;

    if (!lstat( path, &st )) return 1;

    /* Not there under the name the game spelled.  Wine matches components
     * case-insensitively, and games do spell C:\Users for a users/ that Wine
     * created in lower case, so the parent is scanned before giving up. */
    path[base] = 0;
    dir = opendir( base ? path : "/" );
    path[base] = '/';
    if (!dir) return 0;

    while ((ent = readdir( dir )))
    {
        if (strlen( ent->d_name ) != clen) continue;
        if (strncasecmp( ent->d_name, comp, clen )) continue;
        memcpy( path + base + 1, ent->d_name, clen );
        closedir( dir );
        return 1;
    }
    closedir( dir );
    return 0;
}

NTSTATUS ntdll_get_unix_file_name( const WCHAR *dos, char **unix_name, UINT disposition )
{
    char nt[PATH_MAX], out[PATH_MAX];
    const struct drive *drive;
    const char *p, *end;
    NTSTATUS status;
    size_t len;
    int n;

    if (unix_name) *unix_name = NULL;
    if (!dos || !unix_name) return STATUS_INVALID_PARAMETER;

    for (len = 0; dos[len]; len++) /* nothing */;
    n = ntdll_wcstoumbs( dos, (DWORD)len, nt, sizeof(nt) - 1, FALSE );
    if (n <= 0 || (size_t)n >= sizeof(nt) - 1) return STATUS_NAME_TOO_LONG;
    nt[n] = 0;

    if (!ndrives) { no_drives_once(); return STATUS_OBJECT_PATH_NOT_FOUND; }

    /* "\??\C:\..." is the only shape Proton produces.  Anything else is
     * named rather than guessed at: a device path or a UNC share converted by
     * chopping off four characters would be a wrong file, not a failure. */
    if (strncmp( nt, "\\??\\", 4 ) || !nt[4] || nt[5] != ':')
    {
        fprintf( stderr, "helper: %s is not a \\??\\<drive>: path; this helper "
                 "converts drive-letter paths only, and refuses rather than "
                 "naming some other file\n", nt );
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    }
    if (!(drive = find_drive( nt[4] )))
    {
        fprintf( stderr, "helper: this prefix has no drive %c:, so %s cannot "
                 "be converted\n", nt[4], nt );
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (drive->rootlen >= sizeof(out)) return STATUS_NAME_TOO_LONG;
    memcpy( out, drive->root, drive->rootlen );
    len = drive->rootlen;
    out[len] = 0;
    /* Z: is "/" in every prefix; joining would otherwise produce "//usr". */
    if (len == 1 && out[0] == '/') len = 0;

    status = STATUS_SUCCESS;
    for (p = nt + 6; *p; p = end)
    {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        for (end = p; *end && *end != '\\' && *end != '/'; end++) /* nothing */;

        if (!append_component( out, &len, sizeof(out), p, (size_t)(end - p) ))
        {
            /* A missing LEAF is ordinary -- it is a file about to be created,
             * which is what a cloud save is -- and Wine answers it with
             * STATUS_NO_SUCH_FILE and a usable path, which Proton's
             * get_unix_file_name() deliberately accepts.  A missing DIRECTORY
             * is not ordinary: every deeper component would then be invented,
             * so it stays a hard failure. */
            while (*end == '\\' || *end == '/') end++;
            if (*end)
            {
                fprintf( stderr, "helper: %s has no directory component %.*s "
                         "under %s; refusing rather than inventing the rest of "
                         "the path\n", nt, (int)(end - p), p, drive->root );
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
            status = STATUS_NO_SUCH_FILE;
            break;
        }
    }

    if (!len) { out[0] = '/'; out[1] = 0; len = 1; }
    if (!(*unix_name = (char *)malloc( len + 1 ))) return STATUS_NO_MEMORY;
    memcpy( *unix_name, out, len );
    (*unix_name)[len] = 0;
    return status;
}

/* -------------------------------------------------------------- unix -> DOS
 *
 * The caller is Proton's steamclient_unix_path_to_dos_path(), which strips
 * the "\??\" this produces before handing the result to the game.
 */
NTSTATUS ntdll_get_dos_file_name( const char *unix_name, WCHAR **dos, UINT disposition )
{
    char canon[PATH_MAX], nt[PATH_MAX + 8];
    const struct drive *drive = NULL;
    const char *src, *rest = NULL;
    unsigned int i;
    size_t len, n;

    if (dos) *dos = NULL;
    if (!unix_name || !dos) return STATUS_INVALID_PARAMETER;
    if (!ndrives) { no_drives_once(); return STATUS_OBJECT_PATH_NOT_FOUND; }

    /* Resolve links before matching, for the same reason the drive roots are
     * resolved: the Steam client and the prefix reach the same directory by
     * different names.  A path whose last component does not exist yet is
     * still worth answering, so the parent is resolved instead. */
    src = unix_name;
    if (realpath( unix_name, canon )) src = canon;
    else
    {
        const char *slash = strrchr( unix_name, '/' );

        if (slash && slash != unix_name)
        {
            char parent[PATH_MAX];
            size_t plen = (size_t)(slash - unix_name);

            if (plen < sizeof(parent))
            {
                memcpy( parent, unix_name, plen );
                parent[plen] = 0;
                if (realpath( parent, canon ))
                {
                    n = strlen( canon );
                    if (n + strlen( slash ) < sizeof(canon))
                    {
                        strcpy( canon + n, slash );
                        src = canon;
                    }
                }
            }
        }
    }

    len = strlen( src );
    for (i = 0; i < ndrives; i++)   /* longest root first; see by_rootlen */
    {
        size_t rl = drives[i].rootlen;

        if (len < rl || memcmp( src, drives[i].root, rl )) continue;
        if (src[rl] && src[rl] != '/' && !(rl == 1 && drives[i].root[0] == '/'))
            continue;               /* /home/x must not match a /home/xy root */
        drive = &drives[i];
        rest = src + (rl == 1 && drives[i].root[0] == '/' ? 0 : rl);
        break;
    }
    if (!drive)
    {
        fprintf( stderr, "helper: %s is under no drive this prefix maps; "
                 "refusing rather than naming an arbitrary drive\n", src );
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    n = (size_t)snprintf( nt, sizeof(nt), "\\??\\%c:%s", drive->letter,
                          *rest ? rest : "\\" );
    if (n >= sizeof(nt)) return STATUS_NAME_TOO_LONG;
    for (i = 4; i < n; i++) if (nt[i] == '/') nt[i] = '\\';

    if (!(*dos = (WCHAR *)malloc( (n + 1) * sizeof(WCHAR) ))) return STATUS_NO_MEMORY;
    n = ntdll_umbstowcs( nt, (DWORD)n, *dos, (DWORD)n );
    (*dos)[n] = 0;
    return STATUS_SUCCESS;
}
