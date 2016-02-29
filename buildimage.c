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
 * $Id: buildimage.c,v 1.2 2004/11/10 10:23:26 tjd21 Exp $
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
#include <asm/page.h>

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
 */

#define BZ_SETUP_OFFSET    (512 * (1 + 6)) /* boot sector + 6 setup sectors */
/* This *MUST* match the SETUPSECTS in bootsect.S */

#define BZ_ENTRY_OFFSET    0x30
#define BZ_MBI_OFFSET      0x34
/* These *MUST* fit the offsets of entry_address and mbi_address in setup.S */

/* Bring in the bzImage boot sector and setup code */
#include "bzimage_header.c"

void make_bzImage(section_t *sections, 
                  address_t entry, 
                  address_t mbi,
                  FILE *fp)
/* Rework this list of sections into a bzImage and write it out to fp */
{
    int i;
    size_t offset;
    section_t *s;

    for (s = sections; s; s = s->next) {
        if (s->start < HIGHMEM_START) {
            printf("Fatal: kernel uses low memory.  Sorry, you'll have to "
                   "wait for the next\n"
                   "       version to load kernels below 1MB.\n");
            exit(1);
        }
    }

    /* Patch the kernel and mbi addresses into the setup code */
    *(address_t *)(bzimage_setup + BZ_ENTRY_OFFSET) = entry;
    *(address_t *)(bzimage_setup + BZ_MBI_OFFSET) = mbi;
    if (!quiet) printf("Kernel entry is %p, MBI is %p.\n", entry, mbi);

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

    /* Sorted list of sections higher than 1MB: write them out */
    for (s = sections, i=0; s; s = s->next) {
        offset = (s->start - HIGHMEM_START) + BZ_SETUP_OFFSET;
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
 *  Option 2: build an image that a multiboot loader can load (if, say,
 *  your multiboot loader can't handle multiple modules).  We take the
 *  memory image and stick a tiny piece of trampoline code on the end,
 *  which copies some values from the mbi struct that the loader
 *  sets up (e.g. memory size), and then jumps to the kernel entry address.
 *  (This code is in trampoline.S)
 * 
 *  Then we prepend multiboot and elf32 headers, giving our trampoline 
 *  as the entry address for the overall package.
 */


void make_mb_image(section_t *sections, 
                   address_t entry, 
                   address_t mbi,
                   FILE *fp)
/* Rework this list of sections into a multiboot image and write it to fp */
{
    int i;
    size_t offset;
    section_t *s;
    address_t end_of_image, loadsize;
    struct multiboot_header mbh;
    Elf32_Ehdr ehdr;
    Elf32_Phdr phdr;

    /* Fill in the details in the trampoline code */
    mb_mbi_address = mbi;
    mb_entry_address = entry;
    if (!quiet) printf("Kernel entry is %p, MBI is %p.\n", entry, mbi);

    end_of_image = 0;
    for (s = sections; s; s = s->next) {
        if (s->start < HIGHMEM_START) {
            printf("Fatal: kernel uses low memory.  Sorry, you'll have to "
                   "wait for the next\n"
                   "       version to load kernels below 1MB.\n");
            exit(1);
        }
        end_of_image = MAX(end_of_image, (s->start + s->size));
    }

    loadsize = (end_of_image - HIGHMEM_START) 
        + (mb_trampoline_end - mb_trampoline);

    /* Multiboot and ELF headers */
    memset ((char *)&mbh, 0, sizeof mbh);
    memset ((char *)&ehdr, 0, sizeof ehdr); 
    memset ((char *)&phdr, 0, sizeof phdr);

    mbh.magic = 0x1BADB002;
    mbh.flags = MULTIBOOT_MEMORY_INFO;
    mbh.checksum = (~(mbh.magic + mbh.flags)) + 1;

    *(unsigned long *)&ehdr = 0x464c457f;
    ehdr.e_ident[EI_CLASS] = ELFCLASS32;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_STANDALONE;
    ehdr.e_type = ET_EXEC;
    ehdr.e_machine = EM_386;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = end_of_image;
    ehdr.e_phoff = sizeof ehdr;
    ehdr.e_ehsize = sizeof ehdr;
    ehdr.e_phentsize = sizeof phdr;
    ehdr.e_phnum = 1;
    
    /* One great big program header.  Ought to do one header per section, 
     * but they're pretty much contiguous anyway.  In any case this 
     * will test the writeout code from the bzImage buider. */

    phdr.p_type = PT_LOAD;
    phdr.p_flags = PF_X|PF_R|PF_W;
    phdr.p_offset = sizeof ehdr + sizeof phdr + sizeof mbh;
    phdr.p_vaddr = HIGHMEM_START;
    phdr.p_paddr = HIGHMEM_START;
    phdr.p_filesz = loadsize;
    phdr.p_memsz = loadsize;
    phdr.p_align = 0;
    

    /* Write out headers */
    if (fseek(fp, 0, SEEK_SET) < 0) {
        printf("Fatal: error seeking in output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(&ehdr, sizeof(ehdr), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(&phdr, sizeof(phdr), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(&mbh, sizeof(mbh), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (!quiet) printf("Wrote ELF/multiboot headers.\n");

    
    /* Sorted list of sections higher than 1MB: write them out */
    for (s = sections, i=0; s; s = s->next) {
        offset = (s->start - HIGHMEM_START) 
            + sizeof mbh + sizeof phdr + sizeof ehdr;
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

    /* Write out trampoline */
    offset = (end_of_image - HIGHMEM_START) 
            + sizeof mbh + sizeof phdr + sizeof ehdr;
    if (fseek(fp, offset, SEEK_SET) < 0) {
        printf("Fatal: error seeking in output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (fwrite(mb_trampoline, (mb_trampoline_end-mb_trampoline), 1, fp) != 1) {
        printf("Fatal: error writing to output file: %s\n", 
               strerror(errno));
        exit(1);
    }
    if (!quiet) printf("Wrote multiboot trampoline.\n");
    
}



/*
 *  EOF(buildimage.c)
 */
