/*
 * present_capture.c -- read the compositor's own framebuffer and prove the
 * presented colour is in it, unscaled and unblended.
 *
 * The second observer in ppc64le/dxvk/check-present-smoke.sh.  present_smoke.c
 * reports what D3D11 says is in the back buffer; this reports what the display
 * server put on the screen, and the gate requires the two to agree.  They are
 * deliberately different programs on different sides of the whole stack: this
 * is a native ppc64le ELF binary reading a PNG the COMPOSITOR wrote, with no
 * Wine, no guest, no DXVK and no Vulkan in the process at all.  Nothing is
 * shared between them that could make both wrong the same way -- not even the
 * channel order, which each states for itself.
 *
 * WHAT IT ASSERTS, AND WHY IT IS STRONGER THAN "THE MIDDLE PIXEL IS RIGHT":
 * it finds every pixel that is EXACTLY the expected colour, takes their
 * bounding box, and requires that box to be exactly the window's size and to
 * be completely filled -- w*h matching pixels inside a w-by-h rectangle.  A
 * scaled frame fails (the box is the wrong size).  A blended or faded frame
 * fails (there are no exact matches at all).  A frame drawn at the wrong depth
 * or through a colour transform fails (the values differ).  And the position
 * never has to be agreed in advance, so no shell placement policy is baked
 * into the gate.
 *
 * WHY A PNG AND NOT XGetImage, WHICH THIS FILE USED TO DO.  [MEASURED]
 * 2026-08-17, op4k: an Xvfb has no DRI3, and RADV refuses to present to an X
 * server without it -- `vkcube` on Xvfb prints "MESA: info: vulkan: No DRI3
 * support detected - required for presentation" and dumps core, with no Wine
 * anywhere in the process.  So the isolated display this gate is allowed to
 * create cannot be an X server on this machine, and the swapchain that DXVK
 * builds against a win32u client surface there is lost on its first acquire.
 * A headless Weston with the GL renderer has no such limitation: it imports
 * the frame as a dmabuf on the same GPU, and its own screenshooter writes
 * exactly what it composited.  That is what this reads.
 *
 *   present_capture <png> <w> <h> <rr> <gg> <bb>
 *
 * with the expected colour as three hex bytes in RGB order -- the order PNG
 * stores, stated in the order it is read rather than converted twice.
 *
 * Exit 0 if the bounding box is exactly <w>x<h> and every pixel in it matches,
 * 1 if not, 2 if the capture could not be read at all -- a skip is not a pass.
 *
 * Copyright 2026 the ppc64le port authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <png.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main( int argc, char **argv )
{
    unsigned int want_w, want_h, want_r, want_g, want_b;
    unsigned int x, y, width, height, matched = 0;
    unsigned int minx = ~0u, miny = ~0u, maxx = 0, maxy = 0;
    unsigned long csum = 2166136261u;
    png_bytep *rows;
    png_structp png;
    png_infop info;
    FILE *fh;

    if (argc != 7)
    {
        fprintf( stderr, "usage: %s <png> <w> <h> <rr> <gg> <bb>\n", argv[0] );
        return 2;
    }
    want_w = strtoul( argv[2], NULL, 0 );
    want_h = strtoul( argv[3], NULL, 0 );
    want_r = strtoul( argv[4], NULL, 16 );
    want_g = strtoul( argv[5], NULL, 16 );
    want_b = strtoul( argv[6], NULL, 16 );

    if (!(fh = fopen( argv[1], "rb" )))
    {
        fprintf( stderr, "present_capture: cannot open %s\n", argv[1] );
        return 2;
    }
    if (!(png = png_create_read_struct( PNG_LIBPNG_VER_STRING, NULL, NULL, NULL )) ||
        !(info = png_create_info_struct( png )))
    {
        fprintf( stderr, "present_capture: libpng would not start\n" );
        fclose( fh );
        return 2;
    }
    if (setjmp( png_jmpbuf( png ) ))
    {
        fprintf( stderr, "present_capture: %s is not a PNG this can read\n", argv[1] );
        fclose( fh );
        return 2;
    }
    png_init_io( png, fh );
    png_read_info( png, info );

    /* Normalise to 8-bit RGB with no alpha and no palette, so the comparison
     * below is against the same three bytes whatever the compositor chose to
     * write.  These transforms are libpng's, not ours; the pixel values are
     * not touched by any of them for the 8-bit-per-channel case this gate
     * actually sees. */
    png_set_strip_16( png );
    png_set_palette_to_rgb( png );
    png_set_expand_gray_1_2_4_to_8( png );
    png_set_strip_alpha( png );
    png_set_gray_to_rgb( png );
    png_read_update_info( png, info );

    width = png_get_image_width( png, info );
    height = png_get_image_height( png, info );
    if (png_get_channels( png, info ) != 3 || png_get_bit_depth( png, info ) != 8)
    {
        fprintf( stderr, "present_capture: %s is %d channel(s) at %d bits after "
                 "normalisation, expected 3 at 8\n", argv[1],
                 png_get_channels( png, info ), png_get_bit_depth( png, info ) );
        fclose( fh );
        return 2;
    }

    if (!(rows = calloc( height, sizeof(*rows) ))) { fclose( fh ); return 2; }
    for (y = 0; y < height; y++)
        if (!(rows[y] = malloc( png_get_rowbytes( png, info ) ))) { fclose( fh ); return 2; }
    png_read_image( png, rows );
    fclose( fh );

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            const png_byte *p = rows[y] + (size_t)x * 3;

            csum ^= p[0]; csum = (csum * 0x01000193u) & 0xffffffffu;
            csum ^= p[1]; csum = (csum * 0x01000193u) & 0xffffffffu;
            csum ^= p[2]; csum = (csum * 0x01000193u) & 0xffffffffu;

            if (p[0] != want_r || p[1] != want_g || p[2] != want_b) continue;
            matched++;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }

    printf( "capture: %ux%u screenshot, fnv1a=0x%08lX\n", width, height,
            csum & 0xffffffffu );
    printf( "capture: exact matches of RGB %02X %02X %02X: %u\n",
            want_r, want_g, want_b, matched );

    if (!matched)
    {
        printf( "capture: NOTHING on screen holds that colour\n" );
        return 1;
    }

    printf( "capture: bounding box (%u,%u)-(%u,%u) = %ux%u, wanted %ux%u\n",
            minx, miny, maxx, maxy, maxx - minx + 1, maxy - miny + 1,
            want_w, want_h );

    if (maxx - minx + 1 != want_w || maxy - miny + 1 != want_h)
    {
        printf( "capture: the presented rectangle is the WRONG SIZE -- the "
                "frame was scaled or clipped between D3D11 and the screen\n" );
        return 1;
    }
    if (matched != want_w * want_h)
    {
        printf( "capture: the rectangle is the right size but only %u of its "
                "%u pixels hold the colour -- something is drawn over it, or "
                "it is partly blended\n", matched, want_w * want_h );
        return 1;
    }

    printf( "capture: all %u pixels of a %ux%u rectangle hold exactly the "
            "colour the guest cleared to\n", matched, want_w, want_h );
    return 0;
}
