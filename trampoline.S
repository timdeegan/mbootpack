/*
 *  trampoline.S
 *
 *  Brief setup code for the multiboot output format
 * 
 *  Copyright (C) 2004  Tim Deegan (tjd21@cl.cam.ac.uk)
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
 * $Id: trampoline.S,v 1.2 2004/11/10 09:53:42 tjd21 Exp $
 *
 */

.globl mb_trampoline, mb_trampoline_end, mb_mbi_address, mb_entry_address

.code32
.data   /* Not .text, because we need to be able to write to it */
		
/* This code is called by the *real* multiboot bootloader.
 * It copies some values into the MBI struct and jumps at the
 * kernel.  Twice here there's a load-immediate instruction
 * fabricated from bytes so we can alter it at link time.  Ugh. */
	
mb_trampoline:	
	/* Copy some things from the *old* multiboot struct */
	movl	%ebx, %ecx
	/* ... to the *new* multiboot information struct */
	.byte	0xbb
mb_mbi_address:	
	.long	0xdeadbeef	#  == mov mbi_address, %ebx
	movl	0(%ecx), %eax
	andl	$0x1, %eax
	jz	no_memfields
	orl	$0x1, 0(%ebx) 
	movl	4(%ecx), %eax
	movl	%eax, 4(%ebx)
	movl	8(%ecx), %eax
	movl	%eax, 8(%ebx)
no_memfields:
	
	movl	0(%ecx), %eax
	andl	$0x2, %eax
	jz	no_bootdev
	orl	$0x2, 0(%ebx) 
	movl	12(%ecx), %eax
	movl	%eax, 12(%ebx)
no_bootdev:

	movl	0(%ecx), %eax
	andl	$0x40, %eax
	jz	no_memmap
	orl	$0x40, 0(%ebx)
	movl	44(%ecx), %eax
	movl	%eax, 44(%ebx)
	movl	48(%ecx), %eax
	movl	%eax, 48(%ebx)
no_memmap:

	/* Magic multiboot number */
	movl	$0x2BADB002, %eax
	/* Launch the kernel */
	.byte	0xb9
mb_entry_address:	
	.long	0xdeadbeef	# == mov entry_address, %ecx
	jmp	*%ecx
mb_trampoline_end:

foo:	nop
	jmp foo
	
/*
 *  EOF (trampoline.S)
 */
