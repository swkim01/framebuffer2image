#define __USE_POSIX
#define _BSD_SOURCE
#include <limits.h>
#include <inttypes.h>
#include <stdint.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/time.h>
#include "read_fb.h"
#include "encode_png.h"
#include "encode_jpeg.h"

#define DEFAULT_FRAMEBUFFER_DEVICE "/dev/fb0"
#define DEFAULT_OUTPUT_IMAGE_FILENAME_PREFIX "out"

enum image_type{
	IT_PNG, IT_JPEG
};

void usage(char *progname, FILE *f);
void print_sc(struct fb_var_screeninfo sc);
void output_image_to_file(uint8_t *encoded_image, uint32_t encoded_image_size, enum image_type type, char *filename_base, _Bool filename_base_as_is);

int main(int argc, char *argv[])
{
	int opt;
	char *dev;
	_Bool dev_set=0;
	enum image_type type;
	char *type_str;
	_Bool type_set=0;
	long clevel=-1;
	char *endptr;
	_Bool clevel_set=0;
	struct fb_var_screeninfo sc;
	uint8_t fb_effective_bytes_per_pixel;
	uint64_t size;
	uint8_t *buf;
	_Bool verbose=0;
	uint8_t *encoded_image;
	uint32_t encoded_image_size;
	int output_filename_base_len;
	char *output_filename_base;
	_Bool output_filename_base_set=0;
	struct timeval start_read, end_read, start_conv, end_conv;

	dev=(char*)malloc((_POSIX_PATH_MAX+1)*sizeof(char));
	if(dev==NULL){
		fprintf(stderr, "error: failed to malloc dev\n");
		exit(EXIT_FAILURE);
	}

	type_str=(char*)malloc((4+1)*sizeof(char));
	if(type_str==NULL){
		fprintf(stderr, "error: failed to malloc type_str\n");
		exit(EXIT_FAILURE);
	}

	while((opt=getopt(argc, argv, "d:t:c:f:vh"))!=-1){
		switch(opt){
			case 'd':
				if(dev_set){
					fprintf(stderr, "error: framebuffer device option is specified more than once\n");
					exit(EXIT_FAILURE);
				}
				if(strlen(optarg)>_POSIX_PATH_MAX){
					fprintf(stderr, "error: framebuffer device path name is too long\n");
					exit(EXIT_FAILURE);
				}
				strncpy(dev, optarg, _POSIX_PATH_MAX+1);
				dev_set=!0;
				break;
			case 't':
				if(type_set){
					fprintf(stderr, "error: output image type option is specified more than once\n");
					exit(EXIT_FAILURE);
				}
				if(strlen(optarg)>4){
					fprintf(stderr, "error: output image type name is too long\n");
					exit(EXIT_FAILURE);
				}
				strncpy(type_str, optarg, 4+1);
				if(!strcasecmp("png", type_str))
					type=IT_PNG;
				else if(!strcasecmp("jpg", type_str))
					type=IT_JPEG;
				else if(!strcasecmp("jpeg", type_str))
					type=IT_JPEG;
				else{
					fprintf(stderr, "error: unrecognized output image type: %s\n", type_str);
					usage(argv[0], stderr);
					exit(EXIT_FAILURE);
				}
				free(type_str);
				type_set=!0;
				break;
			case 'c':
				if(clevel_set){
					fprintf(stderr, "error: compression level option is specified more than once\n");
					exit(EXIT_FAILURE);
				}
				clevel=strtol(optarg, &endptr, 10);
				if((clevel==LONG_MIN)||(clevel==LONG_MAX)){
					fprintf(stderr, "error: strtol returned %s\n", clevel==LONG_MIN?"LONG_MIN":"LONG_MAX");
					perror("strtol");
					exit(EXIT_FAILURE);
				}else if(endptr==optarg){
					fprintf(stderr, "error: no digits were found\n");
					exit(EXIT_FAILURE);
				}else if(*endptr!='\0'){
					fprintf(stderr, "error: further characters after number: %s\n", endptr);
					exit(EXIT_FAILURE);
				}
				clevel_set=!0;
				break;
			case 'f':
				if(output_filename_base_set){
					fprintf(stderr, "error: base output filename option is specified more than once\n");
					exit(EXIT_FAILURE);
				}
				output_filename_base_len=strlen(optarg);
				output_filename_base=(char*)malloc((output_filename_base_len+1)*sizeof(char));
				if(output_filename_base==NULL){
					fprintf(stderr, "error: failed to malloc output_filename_base\n");
					exit(EXIT_FAILURE);
				}
				strcpy(output_filename_base, optarg);
				output_filename_base_set=!0;
				break;
			case 'v':
				verbose=!0;
				break;
			case 'h':
			default:
				usage(argv[0], stderr);
				exit(EXIT_SUCCESS);
		}
	}

	if(optind!=argc){
		fprintf(stderr, "error: extra option specified\n");
		usage(argv[0], stderr);
		exit(EXIT_FAILURE);
	}

	if(!dev_set){
		strncpy(dev, DEFAULT_FRAMEBUFFER_DEVICE, _POSIX_PATH_MAX+1);
		dev_set=!0;
	}

	if(verbose&&!type_set){
		fprintf(stderr, "info: type is not specified on command line; using the default value\n");
		type=IT_PNG;
	}

	if(verbose&&!clevel_set)
		fprintf(stderr, "info: clevel is not specified on command line; using the default value\n");

	if(!output_filename_base_set){
		if(verbose)
			fprintf(stderr, "info: output_filename_base is not specified on command line; using the default string\n");
		output_filename_base_len=strlen(DEFAULT_OUTPUT_IMAGE_FILENAME_PREFIX);
		output_filename_base=(char*)malloc((output_filename_base_len+1)*sizeof(char));
		if(output_filename_base==NULL){
			fprintf(stderr, "error: failed to malloc output_filename_base\n");
			exit(EXIT_FAILURE);
		}
		strcpy(output_filename_base, DEFAULT_OUTPUT_IMAGE_FILENAME_PREFIX);
	}

	read_fb_init(dev, &sc, &fb_effective_bytes_per_pixel, &size);

	if(verbose){
		printf("Device: %s\n", dev);
		print_sc(sc);
	}

	if(sc.nonstd){
		fprintf(stderr, "error: %s has non-standard pixel format\n", dev);
		exit(EXIT_FAILURE);
	}

	if((sc.red.msb_right!=0)||(sc.green.msb_right!=0)||(sc.blue.msb_right!=0)||(sc.transp.length==0?0:sc.transp.msb_right!=0)){
		fprintf(stderr, "error: sc.*.msb_right is not 0\n");
		exit(EXIT_FAILURE);
	}

	if(sc.bits_per_pixel>64){
		fprintf(stderr, "error: color bits per pixel is too high\n");
		exit(EXIT_FAILURE);
	}

	if((uint64_t)sc.xres*sc.yres>(~((uint64_t)0))/fb_effective_bytes_per_pixel){
		fprintf(stderr, "error: the framebuffer resoltion is too high\n");
		exit(EXIT_FAILURE);
	}

	buf=(uint8_t*)malloc(size);
	if(buf==NULL){
		fprintf(stderr, "error: failed to malloc buf\n");
		exit(EXIT_FAILURE);
	}

	if(verbose)
		gettimeofday(&start_read, NULL);
	read_fb(buf, size);
	if(verbose)
		gettimeofday(&end_read, NULL);

	read_fb_finalize();

	switch(type){
		case IT_PNG:
			encode_png_init(sc, fb_effective_bytes_per_pixel, clevel);
			if(verbose)
				gettimeofday(&start_conv, NULL);
			encoded_image=encode_png(buf, &encoded_image_size);
			if(verbose)
				gettimeofday(&end_conv, NULL);

			break;

		case IT_JPEG:
			encode_jpeg_init(sc, fb_effective_bytes_per_pixel, clevel);
			if(verbose)
				gettimeofday(&start_conv, NULL);
			encoded_image=encode_jpeg(buf, &encoded_image_size);
			if(verbose)
				gettimeofday(&end_conv, NULL);

			break;

		default:
			fprintf(stderr, "error: unknown output image type (internal error)\n");
			exit(EXIT_FAILURE);
	}

	free(dev);

	free(buf);

	if(verbose){
		printf("Time (read): %f sec\n", (end_read.tv_sec - start_read.tv_sec) + (end_read.tv_usec - start_read.tv_usec) * 1e-6);
		printf("Time (conv): %f sec\n", (end_conv.tv_sec - start_conv.tv_sec) + (end_conv.tv_usec - start_conv.tv_usec) * 1e-6);
	}

	output_image_to_file(encoded_image, encoded_image_size, type, output_filename_base, output_filename_base_set);

	switch(type){
		case IT_PNG:
			encode_png_finalize();
			break;

		case IT_JPEG:
			encode_jpeg_finalize();
			break;

		default:
			fprintf(stderr, "error: unknown output image type (internal error)\n");
			exit(EXIT_FAILURE);
	}

	return 0;
}

void usage(char *progname, FILE *f)
{
	fprintf(f, "Usage: %s [OPTION]...\n", progname);
	fprintf(f, "  -d DEV      input frame buffer device (default: %s)\n", DEFAULT_FRAMEBUFFER_DEVICE);
	fprintf(f, "  -t TYPE     output image type (png or jpeg (jpg)) (default: png)\n");
	fprintf(f, "  -f FILE     output image to FILE (default: %s.[png|jpg])\n", DEFAULT_OUTPUT_IMAGE_FILENAME_PREFIX);
	fprintf(f, "  -c LEVEL    compression level\n" \
	           "              0-3 for PNG and 0-100 for JPEG\n" \
	           "              specify -1 to use the default compression level\n");
	fprintf(f, "  -v          be verbose\n");
	fprintf(f, "  -h          show this help and exit\n");

	return;
}

void print_sc(struct fb_var_screeninfo sc)
{
	printf("xres: %" PRIu32 "\n", sc.xres);
	printf("yres: %" PRIu32 "\n", sc.yres);
	printf("bits_per_pixel: %" PRIu32 "\n", sc.bits_per_pixel);
	printf("grayscale: %" PRIu32 "\n", sc.grayscale);
	if(!sc.grayscale){
		printf("red.offset: %" PRIu32 "\n", sc.red.offset);
		printf("red.length: %" PRIu32 "\n", sc.red.length);
		printf("red.msb_right: %" PRIu32 "\n", sc.red.msb_right);
		printf("green.offset: %" PRIu32 "\n", sc.green.offset);
		printf("green.length: %" PRIu32 "\n", sc.green.length);
		printf("green.msb_right: %" PRIu32 "\n", sc.green.msb_right);
		printf("blue.offset: %" PRIu32 "\n", sc.blue.offset);
		printf("blue.length: %" PRIu32 "\n", sc.blue.length);
		printf("blue.msb_right: %" PRIu32 "\n", sc.blue.msb_right);
	}
	printf("transp.offset: %" PRIu32 "\n", sc.transp.offset);
	printf("transp.length: %" PRIu32 "\n", sc.transp.length);
	printf("transp.msb_right: %" PRIu32 "\n", sc.transp.msb_right);
	printf("nonstd: %" PRIu32 "\n", sc.nonstd);
	printf("rotate: %" PRIu32 "\n", sc.rotate);

	return;
}

void output_image_to_file(uint8_t *encoded_image, uint32_t encoded_image_size, enum image_type type, char *filename_base, _Bool filename_base_as_is)
{
	int fd;
	ssize_t wc;
	int filename_base_len;
	char *filename;

	if(!filename_base_as_is){
		if(filename_base!=NULL)
			filename_base_len=strlen(filename_base);
		else
			filename_base_len=0;

		filename=(char*)malloc((filename_base_len+4+1)*sizeof(char));
		if(filename==NULL){
			fprintf(stderr, "error: failed to malloc filename\n");
			exit(EXIT_FAILURE);
		}

		if(filename_base!=NULL)
			strcpy(filename, filename_base);

		switch(type){
			case IT_PNG:
				strcat(filename, ".png");
				break;

			case IT_JPEG:
				strcat(filename, ".jpg");
				break;

			default:
				fprintf(stderr, "error: unknown output image type (internal error)\n");
				exit(EXIT_FAILURE);
		}
	}else
		filename=filename_base;

	if((fd=open(filename, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH))==-1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	wc=write(fd, encoded_image, encoded_image_size);
	if(wc==-1){
		perror("write");
		exit(EXIT_FAILURE);
	}else if((uint32_t)wc!=encoded_image_size){
		fprintf(stderr, "error: write returned unexpected write count\n");
		exit(EXIT_FAILURE);
	}

	if(close(fd)==-1){
		perror("close");
		exit(EXIT_FAILURE);
	}

	return;
}
