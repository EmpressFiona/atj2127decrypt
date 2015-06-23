#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "decrypt_impl.h"

/* From SDK */
typedef struct {
	uint8_t   name[11]; /* 8.3 format */
	uint8_t   type; // A, B, H, F, S, U, or I  -- no idea what this is for yet
	uint32_t  address;
	uint32_t  offset; // file offset in bytes
	uint32_t  length; // length in bytes, rounded up to 512 bytes 
	uint32_t  sub_type;
	uint32_t  checksum;
} AFI_DIR_t;

/* Files to look for in the firmware image */
const uint8_t fwimage_key_name[] = "FWIMAGE FW ";
const uint8_t nandid_key_name[] = "FLASH_IDBIN";

/* Assembly-language routines */
extern int func_fw_decrypt_init(struct decrypt_struct *);
extern void func_fw_decrypt_run(uint8_t *buf_out, int length, uint8_t *crypt);

#if 0
static void dump_buffer(char *filename, uint8_t *buf, int length)
{
	int fd;
	fd = open(filename, O_CREAT|O_WRONLY, 0666);
	write(fd, buf, length);
	close(fd);
}

/* Locate a file in an AFI directory (index of files in the upgrade file) */
static int
find_afi_dir_key(uint8_t *buffer, uint32_t buffer_len, const uint8_t *name, uint8_t key_len, AFI_DIR_t *dest)
{
	AFI_DIR_t *current;
	AFI_DIR_t *end = (AFI_DIR_t *)(buffer + buffer_len);

	for(current = (AFI_DIR_t *)buffer; current < end; current++)
    {
        if (memcmp(current->name, name, key_len) == 0) {
			memcpy(dest, current, sizeof(*current));
			return 0;
        }
    }

	return -1;
}
#endif

static int
read_and_decrypt(struct decrypt_struct *decrypt_info, int fd, uint8_t *buffer, int length)
{
	uint32_t read_bytes;

	while(length) {
		if(length > 2048)
			read_bytes = 2048;
		else
			read_bytes = length;

		if(read(fd, decrypt_info->pInOutBuffer, read_bytes) != read_bytes) {
			perror("read_and_decrypt: read");
			return -1;
		}

		func_fw_decrypt_run(decrypt_info->pInOutBuffer, read_bytes, decrypt_info->pGLBuffer);

		memcpy(buffer, decrypt_info->pInOutBuffer, read_bytes);

		length -= read_bytes;
		buffer += read_bytes;
	}

	return 0;
}

static int
firmware_file_write(struct decrypt_struct *decrypt_info, int fd, int fd_out, uint32_t fw_offset, uint32_t fw_length)
{
    uint16_t sector_num;
    uint32_t write_addr;
	uint8_t *data_buffer; /* Scratch memory */

	data_buffer = malloc(16 * 1024);
	if(!data_buffer) {
		perror("Couldn't allocate scratch\n");
		return -1;
	}

    uint16_t sector_total = (uint16_t)((fw_length + 511) / 512);

	lseek(fd, fw_offset, SEEK_SET);
	if(read_and_decrypt(decrypt_info, fd, data_buffer, 16 * 1024) != 0) {
		perror("firmware_file_write: read initial");
		free(data_buffer);
		return -1;
	}

    write_addr = 0;

	if(write(fd_out, data_buffer, 16 * 1024) != 16 * 1024) {
		perror("firmware_file_write: write initial");
		free(data_buffer);
		return -1;
	} else {
        write_addr += 16 * 1024;
    }

    sector_total -= 32;

    while (sector_total > 0) {
        if (sector_total > 32) {
            sector_num = 32;
        } else {
            sector_num = sector_total;
        }

		if(read_and_decrypt(decrypt_info, fd, data_buffer, sector_num * 512) != 0) {
			printf("firmware_file_write: read_and_decrypt in loop\n");
			free(data_buffer);
			return -1;
		}

		if(write(fd_out, data_buffer, sector_num * 512) != sector_num * 512) {
			printf("firmware_file_write: write in loop\n");
			free(data_buffer);
			return -1;
		}

        write_addr += sector_num << 9;
        sector_total -= sector_num;
    }

	free(data_buffer);
    return 0;
}

static void
make_sensible_direntry_filename(AFI_DIR_t *direntry, char *out)
{
	for(int i=0; i < 8; i++) {
		if(direntry->name[i] == ' ')
			break;
		*out++ = direntry->name[i];
	}

	*out++ = '.';

	for(int i=0; i < 3; i++) {
		if(direntry->name[i + 8] == ' ')
			break;
		*out++ = direntry->name[i + 8];
	}

	*out++ = 0;
}

static int
dump_single_file(struct decrypt_struct *decrypt_info, int fd, char *output_dir, uint32_t firmware_base, AFI_DIR_t *direntry)
{
	char filename[8 + 3 + 2]; // max 8 character name, 3 character stem, a dot, and a \0.
	char pathname[256];

	make_sensible_direntry_filename(direntry, filename);

	if(snprintf(pathname, 256, "%s/%s", output_dir, filename) > 255) {
		printf("Pathname too long");
		return -1;
	}

	int fd_out = open(pathname, O_CREAT | O_RDWR, 0666);
	if(fd_out < 0) {
		perror("open fd_out");
		return -1;
	}

	printf("Writing %s (type %c)\n", filename, direntry->type);

	firmware_file_write(decrypt_info, fd, fd_out, firmware_base + direntry->offset, direntry->length);
	close(fd_out);
	return 0;
}


static int
do_dump(struct decrypt_struct *decrypt_info, int fd, char *output_dir)
{
	int ret;
	uint32_t firmware_base;
	ret = read(fd, decrypt_info->pInOutBuffer, DECRYPT_INOUT_LENGTH);

	if(ret != DECRYPT_INOUT_LENGTH) {
		fprintf(stderr, "do_dump: read fail\n");
		return -1;
	}

	ret = func_fw_decrypt_init_c(decrypt_info);
	if(ret != 0) {
		printf("Firmware failed validity checks\n");
		return ret;
	}

	firmware_base = DECRYPT_INOUT_LENGTH - decrypt_info->InOutLen;
	//printf("firmware offset %x\n", firmware_base);  // Always 0x800?
	//dump_buffer("inital_decrypt.bin", decrypt_info->pInOutBuffer, DECRYPT_INOUT_LENGTH );
	
	// The decryption gives us a mapping of files to what seem to be offsets. 
	// Count the number of directory entries and store them somewhere.
	int num_entries = 0;
	for(AFI_DIR_t *current = (AFI_DIR_t *)decrypt_info->pInOutBuffer; current->name[0] != 0; current++)
		num_entries ++;

	AFI_DIR_t *directory = malloc(sizeof(AFI_DIR_t) * num_entries);
	if(directory == NULL) {
		perror("malloc central directory\n");
		return -1;
	}
	memcpy(directory, decrypt_info->pInOutBuffer, sizeof(AFI_DIR_t) * num_entries);

	// TODO: First entry seems to be a signature of sorts rather than a file
	for(int entry_idx = 1; entry_idx < num_entries; entry_idx++) {
		AFI_DIR_t *current = &directory[entry_idx];
		dump_single_file(decrypt_info, fd, output_dir, firmware_base, current);
	}

	free(directory);

	return 0;
}

int dump_firmware(char *filename_in)
{
	struct stat stat_buf;
	struct decrypt_struct decrypt_info;
	int fd_in;
	char *output_dir = "out";

	if(stat(filename_in, &stat_buf) != 0) {
		perror("stat");
		return -1;
	}

	memset((void *)&decrypt_info, '\0', sizeof(decrypt_info));

	decrypt_info.pInOutBuffer = malloc(DECRYPT_INOUT_LENGTH);
	decrypt_info.InOutLen = DECRYPT_INOUT_LENGTH;
	decrypt_info.FileLength = stat_buf.st_size;
	decrypt_info.pGLBuffer = malloc(DECRYPT_GL_LENGTH);
	decrypt_info.initusebuffer = malloc(DECRYPT_INIT_LENGTH);
	decrypt_info.initusebufferlen = DECRYPT_INIT_LENGTH;

	/* Create output directory */
	if(access(output_dir, W_OK) != -1) {
		//printf("Output directory %s already exists. Exiting without doing anything\n", output_dir);
		//return -1;
		printf("Output directory %s exists and its contents will be overwritten.\n", output_dir);
	} else {
		if(mkdir(output_dir, 0777) != 0) {
			perror("mkdir");
			return -1;
		}
	}

	fd_in = open(filename_in, O_RDONLY);
	if(fd_in < 0) {
		perror("fd_in");
		return -1;
	}

	if(do_dump(&decrypt_info, fd_in, output_dir) != 0) {
		fprintf(stderr, "do_upgrade fail\n");
		return -1;
	}

	free(decrypt_info.pInOutBuffer);
	free(decrypt_info.pGLBuffer);
	free(decrypt_info.initusebuffer);

	return 0;
}

int main(int argc, char **argv)
{
	return dump_firmware(argv[1]);
}

