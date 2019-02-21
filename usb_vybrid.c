/*
 * Phoenix-RTOS
 *
 * Phoenix server
 *
 * load modules for Vybrid
 *
 * Copyright 2014, 2018 Phoenix Systems
 * Author: Kamil Amanowicz, Pawel Tryfon
 *
 * This file is part of Phoenix-RTOS.
 *
 * See the LICENSE
 */


#include<libusb-1.0/libusb.h>
#if defined(__CYGWIN__)
# if !defined(LIBUSB_API_VERSION) || (LIBUSB_API_VERSION < 0x01000106)
#  error "libusb API version too low. Reqiured minimum 0x01000106"
# endif
# define change_libusb_backend(ctx_ptr) libusb_set_option(ctx_ptr, LIBUSB_OPTION_USE_USBDK)
#else
# define change_libusb_backend(ctx_ptr)
#endif


#include<stdio.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<errno.h>
#include<string.h>
#include<unistd.h>
#include<stdint.h>
#include<arpa/inet.h>
#include<stdlib.h>


/* SDP protocol section */
#define SET_CMD_TYPE(b,v) (b)[0]=(b)[1]=(v)
#define SET_ADDR(b,v) *((uint32_t*)((b)+2))=htonl(v)
#define SET_COUNT(b,v) *((uint32_t*)((b)+7))=htonl(v);
#define SET_DATA(b,v) *((uint32_t*)((b)+11))=htonl(v);
#define SET_FORMAT(b,v) (b)[6]=(v);

int silent = 0;

#define dispatch_msg(silent, fmt, ...)		\
	do {								\
		if (!silent)					\
			printf(fmt, ##__VA_ARGS__);		\
	} while(0)


static inline void set_write_file_cmd(unsigned char* b,uint32_t addr,uint32_t size)
{
	SET_CMD_TYPE(b,0x04);
	SET_ADDR(b,addr);
	SET_COUNT(b,size);
	SET_FORMAT(b,0x20);
}


static inline void set_jmp_cmd(unsigned char* b,uint32_t addr)
{
	SET_CMD_TYPE(b,0x0b);
	SET_ADDR(b,addr);
	SET_FORMAT(b,0x20);
}


static inline void set_status_cmd(unsigned char* b)
{
	SET_CMD_TYPE(b,0x05);
}


static inline void set_write_reg_cmd(unsigned char* b,uint32_t addr,uint32_t v)
{
	SET_CMD_TYPE(b,0x02);
	SET_ADDR(b,addr);
	SET_DATA(b,v);
	SET_FORMAT(b,0x20);
	SET_COUNT(b,4);
}


void print_cmd(unsigned char* b)
{
	printf("Command:\n  type=%02x%02x, addr=%08x, format=%02x, count=%08x, data=%08x\n",b[0],b[1],*(uint32_t*)(b+2),b[6],*(uint32_t*)(b+7),*(uint32_t*)(b+11));

}


#define HID_GET_REPORT			0x01
#define HID_SET_REPORT			0x09
#define HID_REPORT_TYPE_OUTPUT	0x02
#define CTRL_OUT                LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE
#define ENDPOINT_ADDRESS        0x81
#define USB_TIMEOUT				50


static int open_vybrid(libusb_device_handle** h, libusb_context* ctx)
{
	libusb_device **list = 0;
	struct libusb_device_descriptor desc;
	ssize_t i, cnt;
	int rc, retval = 0;

	cnt = libusb_get_device_list(ctx, &list);

	if (cnt < 0)
		fprintf(stderr,"Error getting device list\n");

	for (i = 0; i < cnt; i++) {
		if (LIBUSB_SUCCESS != (rc = libusb_get_device_descriptor(list[i], &desc))) {
			fprintf(stderr, "Failed to get device descriptor (%s)\n", libusb_error_name(rc));
			continue;
		}
		if (desc.idVendor == 0x15a2) {
			if ((desc.idProduct == 0x0080) || (desc.idProduct == 0x007d) || (desc.idProduct == 0x006a))
				dispatch_msg(silent, "Found supported device\n");
			else
				printf("Found unsuported product of known vendor, trying standard settings for this device\n");

			if (LIBUSB_SUCCESS != (rc = libusb_open(list[i], h))) {
				fprintf(stderr,"Failed to open device (%s)\n", libusb_error_name(rc));
				continue;
			} else {
				retval = 1;
				break;
			}
		}
	}

	if (list)
		libusb_free_device_list(list, 1);

	return retval;
}


static inline int control_transfer(libusb_device_handle* h,unsigned char* b,size_t n)
{
	//printf("libusb_control_transfer(h,%x,%x,%x,%x,b,%d,timeout)\n",CTRL_OUT,
	 //       HID_SET_REPORT,HID_REPORT_TYPE_OUTPUT<<8 | b[0],0,n);
	return libusb_control_transfer(h,CTRL_OUT, HID_SET_REPORT,HID_REPORT_TYPE_OUTPUT<<8 | b[0],0 /*interface*/,
			b,n,USB_TIMEOUT);
}


static inline int interrupt_transfer(libusb_device_handle* h, unsigned char* b,int n,int* transferred)
{
	return libusb_interrupt_transfer(h,ENDPOINT_ADDRESS,b,n,transferred,USB_TIMEOUT);
}


#define CMD_SIZE 17
#define BUF_SIZE 1025
#define INTERRUPT_SIZE 65
int load_file(struct libusb_device_handle* h,char* filename,uint32_t  addr)
{
	int fd=-1,n,rc;
	struct stat f_st;
	unsigned char b[BUF_SIZE]={0};

	if (0>(fd=open(filename,O_RDONLY))) {
		fprintf(stderr,"Failed to open file (%s)\n",strerror(errno));
		return -1;
	}

	if(fstat(fd,&f_st) != 0) {
		fprintf(stderr,"File stat failed (%s)\n",strerror(errno));
		close(fd);
		return -1;
	}

	b[0]=1;
	set_write_file_cmd(b+1,addr,f_st.st_size);
	//print_cmd(b+1);
	if((rc = control_transfer(h,b,CMD_SIZE)) < 0){
		fprintf(stderr,"Failed to send write_file command (%s)\n",libusb_error_name(rc));
		goto END;
	}

	b[0]=2;
	while((n = read(fd,b+1,BUF_SIZE-1)) > 0)
		if((rc = control_transfer(h,b,n+1)) < 0) {
			fprintf(stderr,"Failed to send file contents (%s)\n",libusb_error_name(rc));
			goto END;
		}

	if(n < 0) {
		fprintf(stderr,"Error reading file (%d,%s)\n",n,strerror(errno));
		rc = -1;
		goto END;
	}

	//Receive report 3
	if(((rc=interrupt_transfer(h,b,BUF_SIZE,&n)) < 0) || n!=5) {
		fprintf(stderr,"Failed to receive HAB mode (%s,n=%d)\n",libusb_error_name(rc),n);
		rc = -1;
		goto END;
	}
	//printf("HAB mode: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if(((rc = interrupt_transfer(h,b,BUF_SIZE,&n)) < 0) || *(uint32_t*)(b+1)!=0x88888888) {
			fprintf(stderr,"Failed to receive complete status (%s,status=%02x%02x%02x%02x)\n",libusb_error_name(rc),b[1],b[2],b[3],b[4]);
			goto END;
	}

END:
	if(fd>-1) close(fd);
	return rc;
}

int load_image(struct libusb_device_handle *h, void *image, ssize_t size, uint32_t  addr)
{
	int n,rc;
	ssize_t offset = 0;
	unsigned char b[BUF_SIZE]={0};

	b[0] = 1;
	set_write_file_cmd(b + 1, addr, size);
	//print_cmd(b+1);
	if ((rc = control_transfer(h, b, CMD_SIZE)) < 0) {
		fprintf(stderr,"Failed to send write_file command (%s)\n",libusb_error_name(rc));
		goto END;
	}

	b[0] = 2;
	while (offset < size) {
		n = (BUF_SIZE - 1 > size - offset) ? (size - offset) : (BUF_SIZE - 1);
		memcpy(b + 1, image + offset, n);
		offset += n;
		if((rc = control_transfer(h, b, n + 1)) < 0) {
			fprintf(stderr,"Failed to send image contents (%s)\n",libusb_error_name(rc));
			goto END;
		}
	}

	//Receive report 3
	if (((rc = interrupt_transfer(h, b, BUF_SIZE, &n)) < 0) || n != 5) {
		fprintf(stderr,"Failed to receive HAB mode (%s,n=%d)\n",libusb_error_name(rc),n);
		rc = -1;
		goto END;
	}
	//printf("HAB mode: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if (((rc = interrupt_transfer(h, b, BUF_SIZE, &n)) < 0) || *(uint32_t*)(b + 1) != 0x88888888)
			fprintf(stderr,"Failed to receive complete status (%s,status=%02x%02x%02x%02x)\n",libusb_error_name(rc),b[1],b[2],b[3],b[4]);

END:
	return rc;
}

int jmp_2_addr(libusb_device_handle* h,uint32_t addr)
{
	int n,rc = 0;
	unsigned char b[INTERRUPT_SIZE]={0};

	b[0]=1;
	set_jmp_cmd(b+1,addr);
	//print_cmd(b+1);
	if((rc = control_transfer(h,b,CMD_SIZE)) < 0) {
		fprintf(stderr,"Failed to send jmp command (%s)",libusb_error_name(rc));
		goto END;
	}
	if((rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n)) < 0) {
		fprintf(stderr,"Failed to receive HAB mode (%s,n=%d)",libusb_error_name(rc),n);
		goto END;
	}
	//printf("HAB: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);
	if((rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n)) >= 0) {
		fprintf(stderr,"Received HAB error status (%s,n=%d): %02x%02x%02x%02x\nJump address command failed\n",libusb_error_name(rc),n,b[1],b[2],b[3],b[4]);
		goto END;
	} else
		rc = 0;

END:
	return rc;
}


int write_reg(libusb_device_handle* h,uint32_t addr,uint32_t v)
{
	int n,rc;
	unsigned char b[INTERRUPT_SIZE]={0};

	b[0]=1;
	set_write_reg_cmd(b+1,addr,v);
	//print_cmd(b+1);
	rc = control_transfer(h,b,CMD_SIZE);
	if(rc < 0)
		fprintf(stderr,"Failed to send write command (%s)",libusb_error_name(rc));
	else
		rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n);
	if(rc < 0 || n != 5)
		fprintf(stderr,"Failed to receive HAB mode (%s,n=%d)",libusb_error_name(rc),n);
	else
		rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n);
	if(rc < 0)
		fprintf(stderr,"Failed to receive status (%s,n=%d)",libusb_error_name(rc),n);
	//printf("Status: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);

	return rc;
}

int do_status(struct libusb_device_handle* h)
{
	unsigned char b[INTERRUPT_SIZE]={0};
	int rc,n;
	b[0]=1;
	set_status_cmd(b+1);
	//print_cmd(b+1);
	if (silent)
		fprintf(stderr, "\n");

	if((rc = control_transfer(h,b,CMD_SIZE)) < 0) {
		fprintf(stderr,"Failed to send status command (%d,%s)\n",rc,libusb_error_name(rc));
		goto END;
	}
	if((rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n)) < 0 || n != 5) {
		fprintf(stderr,"Failed to receive HAB mode (%s,n=%d)\n",libusb_error_name(rc),n);
		goto END;
	}
	if((rc=interrupt_transfer(h,b,INTERRUPT_SIZE,&n)) < 0) {
		fprintf(stderr,"Failed to receive status (%s,n=%d)\n",libusb_error_name(rc),n);
		goto END;
	}
	//printf("Status: %02x%02x%02x%02x\n",b[1],b[2],b[3],b[4]);

END:
	return rc;
}


int usb_vybrid_dispatch(char* kernel, char* loadAddr, char* jumpAddr, void *image, ssize_t size)
{
	int rc;
	int err = 0;
	libusb_device_handle *h = 0;
//	int kernel_attached = 0;

	libusb_context* ctx = NULL;
	libusb_init(&ctx);
	change_libusb_backend(ctx);

	dispatch_msg(silent, "Starting usb loader.\nWaiting for compatible USB device to be discovered ...\n");
	while(1){
		if (err) {
			usleep(500000);
			if (err > 5)
				return -1;
		}
		err++;

		if(open_vybrid(&h, ctx) == 0) {
			if (err)
				err--;
			continue;
		}

		if(libusb_kernel_driver_active(h,0) > 0){
//			kernel_attached=1;
			libusb_detach_kernel_driver(h,0);
		}
		if((rc = libusb_claim_interface(h,0)) < 0) {
			fprintf(stderr,"Failed to claim device interface (%s)\n",libusb_error_name(rc));
			continue;
		}

		if((rc = do_status(h)) != 0) {
			fprintf(stderr,"Device failure (check if device is in serial download mode, check USB connection)\n");
			return -1;
		}

		uint32_t load_addr = 0;
		if (kernel != NULL && image == NULL && size == 0) {
			if (loadAddr != NULL)
				load_addr = strtoul(loadAddr, NULL, 16);
			if (load_addr == 0)
				load_addr = 0x3f000000;

			if((rc = load_file(h,kernel,load_addr)) != 0) {
				fprintf(stderr,"Failed to load file to device\n");
				continue;
			}
		} else {
			if (loadAddr != NULL)
				load_addr = *(uint32_t *)loadAddr;
			if (load_addr == 0)
				load_addr = 0x3f000000;
			if ((rc = load_image(h, image, size, load_addr)) != 0) {
				fprintf(stderr,"Failed to load image to device\n");
				continue;
			}
		}
		dispatch_msg(silent, "Image file loaded.\n");

		uint32_t jump_addr = 0;
		if (kernel != NULL && image == NULL && size == 0) {
			if(jumpAddr != NULL)
				jump_addr = strtoul(jumpAddr,NULL,16);
		} else {
			if(jumpAddr != NULL)
				jump_addr = *(uint32_t *)jumpAddr;
		}
		if(jump_addr == 0)
			jump_addr = 0x3f000400;
		if((rc = jmp_2_addr(h,jump_addr)) != 0) {
			fprintf(stderr,"Failed to send jump command to device (%d)\n",rc);
			continue;
		}
		dispatch_msg(silent, "Code execution started.\n");

		libusb_release_interface(h,0);
//		if(kernel_attached)
//			libusb_attach_kernel_driver(h,0);

		break;
	}

	dispatch_msg(silent, "Closing usb loader\n");
	if(h) libusb_close(h);
	libusb_exit(ctx);
	ctx = NULL;
	return rc;
}

