/*
 *  buildimage.c
 *
 *  Takes the memory image of a loaded kernel and modules and repackages 
 *  it as a linux bzImage
 *
 *  Copyright (C) 2003-2004  Tim Deegan (tjd21@cl.cam.ac.uk)
 * 
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * $Id: buildimage.c,v 1.3 2008/12/09 12:55:11 tjd Exp $
 *
 */



#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <elf.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "mbootpack.h"
#include "mb_header.h"

/*  We will build an image that a bzImage-capable bootloader will load like 
 *  this:
 * 
 *  ==============   (0)
 *  (BIOS memory)
 *  --------------
 *  (Bootloader)
 *  --------------
 *  bzImage startup code
 *  MBI, command-lines, module info
 *  ==============   (0xa0000)
 *  (memory hole)
 *  ==============   (0x100000)
 *  Kernel and modules
 *  ==============
 * 
 *  The bzImage startup code is mostly taken straight from the linux kernel
 *  (see bootsect.S, startup.S).  It does the usual unpleasant start-of-day
 *  tasks to get to 32-bit protected mode, then sets registers appropriately 
 *  and jumps to the kernel's entry address.
 *  
 *  It also does some relocation to make sure the MBI is where we expect it, 
 *  and parses the linux command line.
 */

#define BZ_SETUP_OFFSET       (512 * (1 + SETUPSECTS)) 
/* These *MUST* fit the offsets of entry_address and mbi_address in setup.S */
#define BZ_ENTRY_OFFSET       0x30
#define BZ_MBI_OFFSET         0x34
#define BZ_HIGH_START_OFFSET  0x38
#define BZ_HIGH_SIZE_OFFSET   0x3C

/* Bring in the bzImage boot sector and setup code */
#include "bzimage_header.c"

address_t place_mbi(long int size) 
/* Find space at the top of *low* memory for the MBI and associated red tape */
{
    address_t start;
    start = 0xa000 - size;
    if (start < 0x9000 + sizeof(bzimage_bootsect) + sizeof(bzimage_setup)) {
        printf("Fatal: command-lines too long: need %i, have %i bytes\n",
               size, 
               0x1000 - (sizeof(bzimage_bootsect) + sizeof(bzimage_setup)));
        exit(1);        
    }
    if (!quiet) {
        printf("Placed MBI and strings (%p+%p)\n", 
               start, size);
    }
    return start;
}

void make_bzImage(section_t *sections, 
                  address_t entry, 
                  address_t mbi,
                  FILE *fp)
/* Rework this list of sections into a bzImage and write it out to fp */
{
    int i;
    size_t offset;
    section_t *s;
    address_t highmem_start = 0xffffffff, highmem_size = 0;

    /* Patch the kernel and mbi addresses into the setup code */
    *(address_t *)(bzimage_setup + BZ_ENTRY_OFFSET) = entry;
    *(address_t *)(bzimage_setup + BZ_MBI_OFFSET) = mbi;
    if (!quiet) printf("Kernel entry is %p, MBI is %p.\n", entry, mbi);

    /* If the high-memory sections don't start at 1MB, shuffle them down 
     * in the file and record how they should be relocated at boot time. */
    for (s = sections, i = 0; s; s = s->next) {
	if (s->start < HIGHMEM_START) continue;
	highmem_start = MIN(highmem_start, s->start);
	highmem_size = MAX(highmem_size, s->start + s->size - highmem_start);
    }
    if (highmem_size != 0) {
	*(address_t *)(bzimage_setup + BZ_HIGH_START_OFFSET) = highmem_start;
	*(address_t *)(bzimage_setup + BZ_HIGH_SIZE_OFFSET) = highmem_size;
	if (!quiet) printf("High sections: %p bytes at %p.\n", 
			   highmem_size, highmem_start);
    } else
	highmem_start = HIGHMEM_START;

    /* Write out header and trampoline */
    if (fseek(fp, 0, SEEK_SET) < 0) {
        printf("Fatal: error seeking in output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(bzimage_bootsect, sizeof(bzimage_bootsect), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(bzimage_setup, sizeof(bzimage_setup), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }

    if (!quiet) printf("Wrote bzImage header: %i + %i bytes.\n", 
                       sizeof(bzimage_bootsect), sizeof(bzimage_setup));

    /* Sorted list of sections below 1MB: write them out */
    for (s = sections, i = 0; s; s = s->next) {
        if (s->start >= HIGHMEM_START) continue;
        offset = (s->start - 0x9000);
        if (fseek(fp, offset, SEEK_SET) < 0) {
            printf("Fatal: error seeking in output file: %s\n", 
                   strerror(errno));
            exit(1);
        }
        if (fwrite(s->buffer, s->size, 1, fp) != 1) {
            printf("Fatal: error writing to output file: %s\n", 
                   strerror(errno));
            exit(1);
        }
        i++;
    }

    if (!quiet) printf("Wrote %i low-memory sections.\n", i);

    /* Sorted list of sections higher than 1MB: write them out */
    for (s = sections, i = 0; s; s = s->next) {
        if (s->start < HIGHMEM_START) continue;
        offset = (s->start - highmem_start) + BZ_SETUP_OFFSET;
        if (fseek(fp, offset, SEEK_SET) < 0) {
            printf("Fatal: error seeking in output file: %s\n", 
                   strerror(errno));
            exit(1);
        }
        if (fwrite(s->buffer, s->size, 1, fp) != 1) {
            printf("Fatal: error writing to output file: %s\n", 
                   strerror(errno));
            exit(1);
        }
        i++;
    }
    
    if (!quiet) printf("Wrote %i high-memory sections.\n", i);
}


/*
 *  EOF(buildimage.c)
 */
