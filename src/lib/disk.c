/*
  disk.[ch]
 
  Copyright (C) 2001 Dell Computer Corporation <Matt_Domsch@dell.com>
 
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <string.h>
#include <linux/types.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "disk.h"
#include "scsi_ioctls.h"
#include "gpt.h"

int
disk_info_from_fd(int fd, 
		  int *interface_type,
		  unsigned int *controllernum, 
		  unsigned int *disknum,
		  unsigned char *part)
{
	struct stat buf;
	int rc;
	__u64 major;
	unsigned char minor;
	memset(&buf, 0, sizeof(struct stat));
	rc = fstat(fd, &buf);
	if (rc == -1) {
		perror("stat");
		return 1;
	}
	if (!(S_ISBLK(buf.st_mode) || S_ISREG(buf.st_mode))) {
		printf("Cannot stat non-block or non-regular file\n");
		return 1;
	}
	major = buf.st_dev >> 8;
	minor = buf.st_dev && 0xFF;

	/* IDE disks can have up to 64 partitions, or 6 bits worth,
	 * and have one bit for the disk number.
	 * This leaves an extra bit at the top.
	 */
	if (major == 3) {
		*disknum = (minor >> 6) & 1;
		*controllernum = (major - 3 + 0) + *disknum;
		*interface_type = ata;
		*part    = minor & 0x3F;
		return 0;
	}
	else if (major == 22) {
		*disknum = (minor >> 6) & 1;
		*controllernum = (major - 22 + 2) + *disknum;
		*interface_type = ata;
		*part    = minor & 0x3F;
		return 0;
	}
	else if (major >= 33 && major <= 34) {
		*disknum = (minor >> 6) & 1;
		*controllernum = (major - 33 + 4) + *disknum;
		*interface_type = ata;
		*part    = minor & 0x3F;
		return 0;
	}
	else if (major >= 56 && major <= 57) {
		*disknum = (minor >> 6) & 1;
		*controllernum = (major - 56 + 8) + *disknum;
		*interface_type = ata;
		*part    = minor & 0x3F;
		return 0;
	}
	else if (major >= 88 && major <= 91) {
		*disknum = (minor >> 6) & 1;
		*controllernum = (major - 88 + 12) + *disknum;
		*interface_type = ata;
		*part    = minor & 0x3F;
		return 0;
	}
 	
        /* I2O disks can have up to 16 partitions, or 4 bits worth. */
	if (major >= 80 && major <= 87) {
		*interface_type = i2o;
		*disknum = 16*(major-80) + (minor >> 4);
		*part    = (minor & 0xF);
		return 0;
	}

	/* SCSI disks can have up to 16 partitions, or 4 bits worth
	 * and have one bit for the disk number.
	 */
	if (major == 8) {
		*interface_type = scsi;
		*disknum = (minor >> 4);
		*part    = (minor & 0xF);
		return 0;
	}
	else  if ( major >= 65 && major <= 71) {
		*interface_type = scsi;
		*disknum = 16*(major-64) + (minor >> 4);
		*part    = (minor & 0xF);
		return 0;
	}
	    
	printf("Unknown interface type.\n");
	return 1;
}

static int
disk_get_scsi_pci(int fd, 
	     unsigned char *bus,
	     unsigned char *device,
	     unsigned char *function)
{
	int rc, nodefd, usefd=fd;
	struct stat buf;
	char slot_name[8];
	unsigned int b=0,d=0,f=0;
	memset(&buf, 0, sizeof(buf));
	rc = fstat(fd, &buf);
	if (rc == -1) {
		perror("stat");
		return 1;
	}
	if (S_ISREG(buf.st_mode)) {
		/* can't call ioctl() on this file and have it succeed.  
		 * instead, need to open the block device
		 * from /dev/.
		 */
		fprintf(stderr, "You must call this program with "
			"a file name such as /dev/sda.\n");
		return 1;
	}

	rc = get_scsi_pci(usefd, slot_name);
	if (rc) {
		perror("get_scsi_pci");
		return rc;
	}
	rc = sscanf(slot_name, "%x:%x.%x", &b,&d,&f);
	if (rc != 3) {
		printf("sscanf failed\n");
		return 1;
	}
	*bus      = b & 0xFF;
	*device   = d & 0xFF;
	*function = f & 0xFF;
	return 0;
}

/*
 * The PCI interface treats multi-function devices as independent
 * devices.  The slot/function address of each device is encoded
 * in a single byte as follows:
 *
 *	7:3 = slot
 *	2:0 = function
 *
 *  pci bus 00 device 39 vid 8086 did 7111 channel 1
 *               00:07.1
 */
#define PCI_DEVFN(slot,func)	((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)		(((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)		((devfn) & 0x07)

static int
disk_get_ide_pci(int fd,
	     unsigned char *bus,
	     unsigned char *device,
	     unsigned char *function)
{
	int num_scanned, procfd;
	unsigned int b=0,d=0,disknum=0, controllernum=0;
	unsigned char part=0;
	char procname[80], infoline[80];
	size_t read_count;
	int interface_type;
	
	disk_info_from_fd(fd, &interface_type, &controllernum,
			  &disknum, &part);


	sprintf(procname, "/proc/ide/ide%d/config", controllernum);
	
	procfd = open(procname, O_RDONLY);
	if (!procfd) {
		perror("opening /proc/ide/ide*/config");
		return 1;
	}
	read_count = read(procfd, infoline, sizeof(infoline)-1);
	close(procfd);
	
	num_scanned = sscanf(infoline,
			     "pci bus %x device %x vid %*x did %*x channel %*x",
			     &b, &d);
	
	if (num_scanned == 2) {
		*bus      = b;
		*device   = PCI_SLOT(d);
		*function = PCI_FUNC(d);
	}

}



/* this is a list of devices */
static int
disk_get_md_parts(int fd)
{

}

int
disk_get_pci(int fd,
	     unsigned char *bus,
	     unsigned char *device,
	     unsigned char *function)
{
	int interface_type=interface_type_unknown;
	unsigned int controllernum=0, disknum=0;
	unsigned char part=0;
	
	disk_info_from_fd(fd,
			  &interface_type,
			  &controllernum, 
			  &disknum,
			  &part);
	switch (interface_type) {
	case ata:
		return disk_get_ide_pci(fd, bus, device, function);
		break;
	case scsi:
		return disk_get_scsi_pci(fd, bus, device, function);
		break;
	case i2o:
		break;
	case md:
		break;
	default:
		break;
	}
	return 1;
}	

int
disk_get_size(int fd, long *size)
{
	return ioctl(fd, BLKGETSIZE, size);
}


/************************************************************
 * msdos_disk_get_extended partition_info()
 * Requires:
 *  - open file descriptor fd
 *  - start, size
 * Modifies: all these
 * Returns:
 *  0 on success
 *  non-zero on failure
 *
 ************************************************************/

static int
msdos_disk_get_extended_partition_info (int fd, LegacyMBR_t *mbr,
					__u32 num,
					__u64 *start, __u64 *size)
{
        /* Until I can handle these... */
        fprintf(stderr, "Extended partition info not supported.\n");
        return 1;
}

/************************************************************
 * msdos_disk_get_partition_info()
 * Requires:
 *  - mbr
 *  - open file descriptor fd (for extended partitions)
 *  - start, size, signature, mbr_type, signature_type
 * Modifies: all these
 * Returns:
 *  0 on success
 *  non-zero on failure
 *
 ************************************************************/

static int
msdos_disk_get_partition_info (int fd, LegacyMBR_t *mbr,
			       __u32 num,
			       __u64 *start, __u64 *size,
			       char *signature,
			       __u8 *mbr_type, __u8 *signature_type)
{	

	long disk_size=0;

	*mbr_type = 0x01;
	*signature_type = 0x01;
	*(__u32 *)signature = mbr->UniqueMBRSignature;

        if (num > 4) {
		/* Extended partition */
                return msdos_disk_get_extended_partition_info(fd, mbr, num,
                                                              start, size);
        }
	else if (num == 0) {
		/* Whole disk */
                *start = 0;
		disk_get_size(fd, &disk_size);
                *size = disk_size;
	}
	else if (num >= 1 && num <= 4) {
		/* Primary partition */
                *start = mbr->PartitionRecord[num-1].StartingLBA;
                *size  = mbr->PartitionRecord[num-1].SizeInLBA;
                
	}
	return 0;
}



/************************************************************
 * disk_get_partition_info()
 * Requires:
 *  - open file descriptor fd
 *  - start, size, signature, mbr_type, signature_type
 * Modifies: all these
 * Returns:
 *  0 on success
 *  non-zero on failure
 *
 ************************************************************/



int
disk_get_partition_info (int fd, 
			 __u32 num,
			 __u64 *start, __u64 *size,
			 char *signature,
			 __u8 *mbr_type, __u8 *signature_type)
{
	LegacyMBR_t mbr;
	off_t offset;
	int bytes_read = 0, this_bytes_read = 0;
	int i, is_gpt=0;

	memset(&mbr, 0, sizeof(mbr));
	offset = lseek(fd, 0, SEEK_SET);
	this_bytes_read = read(fd, &mbr, sizeof(mbr));
	if (this_bytes_read < sizeof(mbr)) return 1;

	
	if (mbr.Signature == MSDOS_MBR_SIGNATURE) {
	
		for (i=0; i<4; i++) {
			if (mbr.PartitionRecord[i].OSType ==
			    EFI_PMBR_OSTYPE_EFI_GPT) {
				is_gpt=1;
				break;
			}
		}

		if (!is_gpt) 
			return msdos_disk_get_partition_info(fd, &mbr, num,
							     start, size,
							     signature,
							     mbr_type,
							     signature_type);
	}
	
	return gpt_disk_get_partition_info(fd, num,
					   start, size,
					   signature,
					   mbr_type,
					   signature_type);
}





#ifdef DISK_EXE
int
main (int argc, char *argv[])
{
	int fd, rc;
	unsigned char bus=0,device=0,func=0;
	if (argc <= 1) return 1;
	fd = open(argv[1], O_RDONLY);
	rc = disk_get_pci(fd, &bus, &device, &func);
	if (!rc) {
		printf("PCI %02x:%02x.%02x\n", bus, device, func);
	}
	return rc;
}
#endif
