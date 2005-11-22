/*
 * Phoenix-RTOS
 * 
 * Phoenix server
 *
 * BSP protocol implementation
 *
 * Copyright 2001 Pawel Pisarczyk
 *
 * This file is part of Phoenix-RTOS.
 *
 * Phoenix-RTOS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phoenix-RTOS kernel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phoenix-RTOS kernel; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

#include "types.h"
#include "errors.h"
#include "serial.h"
#include "bsp.h"
#include "elf.h"


#define KERNEL_BASE  0xc0000000


/* Function sends BSP message */
int bsp_send(int fd, u8 t, u8 *buffer, uint len)
{
  s16 fcs;
  uint k, i;
  int err;
  u8 frame[BSP_FRAMESZ];

  if (len > BSP_MSGSZ)
	 return ERR_ARG;

	/* Calculate checksum */
	frame[0] = t;
	fcs = t;
  for (k = 0, i = BSP_HDRSZ; k < len; k++) {
  	if ((buffer[k] != BSP_ESCCHAR) && (buffer[k] != BSP_ENDCHAR))
  		fcs += (char)buffer[k];  		
  	else
  		frame[i++] = BSP_ESCCHAR;
  	frame[i++] = buffer[k];
  }
  frame[i++] = BSP_ENDCHAR;
  *(s16 *)&frame[1] = fcs;
 
	if ((err = serial_write(fd, frame, i)) < 0)
				return err;

  return ERR_NONE;
}


/* Function receives BSP message */
int bsp_recv(int fd, u8 *t, u8 *buffer, uint len, uint timeout)
{
	u8 c;
	uint i, escfl = 0;
	s16 fcs, sfcs;
	int err;

	if (len < BSP_MSGSZ)
		return ERR_ARG;
	
	if ((err = serial_read(fd, t, 1, timeout)) < 0)
		return err;
	if ((err = serial_read(fd, (u8 *)&sfcs, 2, timeout)) < 0)
		return err;
	
	for (fcs = *t, i = 0, escfl = 0;;) {
		if ((err = serial_read(fd, &c, 1, timeout)) < 0)
			return err;
		if (i == BSP_MSGSZ)
			return ERR_SIZE;

		if (escfl) {
			buffer[i++] = c;
			escfl = 0;
			continue;
		}
		
		if (c == BSP_ENDCHAR) {
			if (sfcs != fcs)
				return ERR_BSP_FCS;
			break;
		}
			
		if (c == BSP_ESCCHAR) {
			escfl = 1;
			continue;
		}
		buffer[i++] = c;
		fcs += (char)c;
	}
	
	return i;
}


/* Function sends BSP request (sends message and waits for answer) */
int bsp_req(int fd, u8 st, u8 *sbuff, uint slen, u8 *rt, u8 *rbuff, uint rlen, u16 num, u16 *rnum)
{
	int err;
	uint fails;
	
	for (fails = 0; fails < BSP_MAXREP; fails++) {
		if ((err = bsp_send(fd, st, sbuff, slen)) < 0)
			return err;
		
		err = bsp_recv(fd, rt, rbuff, rlen, BSP_TIMEOUT);	
		if (err <= 0) {
			if (err == ERR_SERIAL_TIMEOUT)
				return err;
		}
		else {
			if (*(u16 *)rbuff != num) {
				*rnum = *(u16 *)rbuff; 
				return err;
			}
		}
	}
	
	return ERR_BSP_RETR;
}


/*
 * Complex routines
 */


/* Functions sends kernel to Phoenix node */
int bsp_sendkernel(int fd, char *kernel)
{
  FILE *f;
  char sbuff[BSP_MSGSZ], rbuff[BSP_MSGSZ];
  Elf32_Ehdr hdr;
  Elf32_Phdr phdr;
  Elf32_Half k, i;
  Elf32_Half seg, offs;
  int size, l, err;
  u8 t;
  u16 num = 0;
  
	/* Open kernel file and read ELF header */
	if ((f = fopen(kernel, "r")) == NULL)
		return ERR_FILE;	
	if (fread(&hdr, sizeof(Elf32_Ehdr), 1, f) != 1) {
		fclose(f);
		return ERR_FILE;
	}

	/* Read program headers and send segments */
	for (k = 0; k < hdr.e_phnum; k++) {
		fseek(f, hdr.e_phoff + k * sizeof(Elf32_Phdr), SEEK_SET);
		if (fread(&phdr, sizeof(Elf32_Phdr), 1, f) != 1) {
			fclose(f);
			return ERR_FILE;
		}
		
		if ((phdr.p_type == PT_LOAD) && (phdr.p_vaddr != 0)) {

			/* Calculate realmode address */
			seg = (phdr.p_vaddr - KERNEL_BASE) / 16;
			offs = (phdr.p_vaddr - KERNEL_BASE) % 16;
			
			*(u16 *)sbuff = seg;
			*(u16 *)&sbuff[2] = offs;
			
			if ((err = bsp_req(fd, BSP_TYPE_SHDR, sbuff, 4, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
				fclose(f);
				return err;
			}

			/* Send segment data */
			fseek(f, phdr.p_offset, SEEK_SET);
			for (i = 0; i < phdr.p_filesz / BSP_MSGSZ; i++) {
				if (fread(sbuff, 1, BSP_MSGSZ, f) != BSP_MSGSZ) {
					fclose(f);
					return ERR_FILE;
				}

				if ((err = bsp_req(fd, BSP_TYPE_KDATA, sbuff, BSP_MSGSZ, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
					fclose(f);
					return err;
				}
			}
			
		  size = phdr.p_filesz % BSP_MSGSZ;
		  if (size != 0) {
				if ((l = fread(sbuff, 1, size, f)) != size) {
					fclose(f);
 					return ERR_FILE;
 				}
				if ((err = bsp_req(fd, BSP_TYPE_KDATA, sbuff, size, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
					fclose(f);
					return err;
				}
			}
		}
	}

	/* Last message */
	if ((err = bsp_send(fd, BSP_TYPE_GO, sbuff, 1)) < 0) {
		fclose(f);
	  return err;
	}
	fclose(f);
	fprintf(stderr, "[%d] System started\n", getpid());

	return 0;
}


/* Function sends user program to Phoenix node */
int bsp_sendprogram(int fd, char *name, char *sysdir)
{
  FILE *f;
  char sbuff[BSP_MSGSZ], rbuff[BSP_MSGSZ];
  Elf32_Ehdr hdr;
  Elf32_Phdr phdr;
  Elf32_Half k, i;
  uint size, l;
  u8 t;
  u16 num = 0;
  char *tname;
  int err;
  
  if ((tname = (char *)malloc(strlen(sysdir) + 1 + strlen(name) + 1)) == NULL)
  	return ERR_MEM;
	sprintf(tname, "%s/%s", sysdir, name);
	
	if ((f = fopen(tname, "r")) == NULL) {
		bsp_req(fd, BSP_TYPE_ERR, sbuff, 1, &t, rbuff, BSP_MSGSZ, num, &num);
		free(tname);
		return ERR_FILE;
	}
	free(tname);
	
	if (fread(&hdr, sizeof(Elf32_Ehdr), 1, f) != 1) {
		bsp_req(fd, BSP_TYPE_ERR, sbuff, 1, &t, rbuff, BSP_MSGSZ, num, &num);
		fclose(f);
		return ERR_FILE;
	}
	
	/* Send ELF header */	
	if ((err = bsp_req(fd, BSP_TYPE_EHDR, (char *)&hdr, sizeof(Elf32_Ehdr), &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
		fclose(f);
		return err;
	}
	
	/* Send program segments */
	for (k = 0; k < hdr.e_phnum; k++) {
		fseek(f, hdr.e_phoff + k * sizeof(Elf32_Phdr), SEEK_SET);
		if (fread(&phdr, sizeof(Elf32_Phdr), 1, f) != 1) {
			fclose(f);
			return ERR_FILE;
		}
		
		if ((phdr.p_type == PT_LOAD) && (phdr.p_vaddr != 0)) {
			if ((err = bsp_req(fd, BSP_TYPE_PHDR, (char *)&phdr, sizeof(Elf32_Phdr), &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
				fclose(f);
				return err;
			}

			fseek(f, phdr.p_offset, SEEK_SET);
			for (i = 0; i < phdr.p_filesz / BSP_MSGSZ; i++) {
				if (fread(sbuff, 1, BSP_MSGSZ, f) != BSP_MSGSZ) {
					fclose(f);
					return ERR_FILE;
				}
				if ((err = bsp_req(fd, BSP_TYPE_PDATA, sbuff, BSP_MSGSZ, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
					fclose(f);				
					return err;
				}
			}
			
		  size = phdr.p_filesz % BSP_MSGSZ;
		  if (size != 0) {
				if ((l = fread(sbuff, 1, size, f)) != size) {
					fclose(f);
					return ERR_FILE;
				}
				if ((err = bsp_req(fd, BSP_TYPE_PDATA, sbuff, size, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
					fclose(f);
					return err;
				}
			}
		}
	}

	/* Last frame, which finishes transaction */
	if ((err = bsp_req(fd, BSP_TYPE_GO, sbuff, 1, &t, rbuff, BSP_MSGSZ, num, &num)) < 0) {
		fclose(f);
		return err;
	}
	
	fclose(f);
	return ERR_NONE;
}
