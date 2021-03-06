#define __USE_POSIX
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
#include <sys/wait.h>
#include <unistd.h>
#include <png.h>
#include "encode_png.h"
#include "fill_bits.h"

#define RETBUFSIZESTEP BUFSIZ

struct bitop_procedure_atom{
	uint64_t mask;
	int8_t rshift;
	uint8_t depth;
};

struct bitop_procedure{
	struct bitop_procedure_atom red, green, blue, gray, alpha;
};

struct buf_linear_list_node{
	uint8_t *p;
	uint32_t len;
	struct buf_linear_list_node *next;
};

static int png_colortype;
static int color_components;
static uint8_t fb_effective_bytes_per_pixel;
static int png_effective_bytes_per_pixel_color;
static size_t fb_pointer_size, png_pointer_size;
static uint32_t width, height;
static uint32_t localimagesize=0;
static uint8_t *retbuf=NULL;
static struct bitop_procedure bp;
static int clevel;

static void encode_png_core(uint8_t **finalbuf, uint32_t *imagesize);

void encode_png_init(struct fb_var_screeninfo sc, uint8_t fb_effective_bytes_per_pixel_arg, long clevel_cmd)
{
	switch(clevel_cmd){
		case 0:
			clevel=Z_NO_COMPRESSION;
			break;
		case 1:
			clevel=Z_BEST_SPEED;
			break;
		case 2:
			clevel=Z_BEST_COMPRESSION;
			break;
		case 3:
		case -1:
			clevel=Z_DEFAULT_COMPRESSION;
			break;
		default:
			fprintf(stderr, "error: invalid clevel is specified to PNG\n");
			exit(EXIT_FAILURE);
			break;
	}

	width=sc.xres;
	height=sc.yres;
	fb_effective_bytes_per_pixel=fb_effective_bytes_per_pixel_arg;

	memset(&bp, 0, sizeof(bp));

	if(sc.grayscale){
		if(sc.transp.length==0){
			png_colortype=PNG_COLOR_TYPE_GRAY;
			color_components=1;
			png_effective_bytes_per_pixel_color=(sc.bits_per_pixel<=8)?1:2;

			switch(sc.bits_per_pixel){
				case 1:
				case 2:
				case 4:
				case 8:
				case 16:
					bp.gray.mask=fill_bits(sc.bits_per_pixel);
					bp.gray.depth=sc.bits_per_pixel;
					break;

				default:
					bp.gray.mask=fill_bits(sc.bits_per_pixel);
					if(sc.bits_per_pixel<4)
						bp.gray.depth=4;
					else if(sc.bits_per_pixel<8)
						bp.gray.depth=8;
					else
						bp.gray.depth=16;
					break;
			}
			bp.gray.rshift=sc.bits_per_pixel-png_effective_bytes_per_pixel_color*8;
		}else{
			if((sc.transp.offset!=0)&&(sc.transp.offset+sc.transp.length-1!=sc.bits_per_pixel)){
				fprintf(stderr, "error: gray bit field which is detected by alpha bit field is fragmented\n");
				exit(EXIT_FAILURE);
			}

			png_colortype=PNG_COLOR_TYPE_GRAY_ALPHA;
			color_components=2;
			png_effective_bytes_per_pixel_color=((sc.bits_per_pixel-sc.transp.length<=8)&&(sc.transp.length<=8))?1:2;

			for(bp.gray.depth=sc.bits_per_pixel-sc.transp.length; bp.gray.depth>png_effective_bytes_per_pixel_color*8; bp.gray.depth--)
				;
			for(bp.alpha.depth=sc.transp.length; bp.alpha.depth>png_effective_bytes_per_pixel_color*8; bp.alpha.depth--)
				;

			if(sc.transp.offset==0){
				bp.gray.mask=fill_bits(sc.bits_per_pixel-sc.transp.length)<<sc.transp.length;
				bp.gray.rshift=sc.transp.length+((sc.bits_per_pixel-sc.transp.length)-bp.gray.depth);
				bp.alpha.mask=fill_bits(sc.transp.length);
				bp.alpha.rshift=sc.transp.length-bp.alpha.depth;
			}else if(sc.transp.offset+sc.transp.length-1==sc.bits_per_pixel){
				bp.gray.mask=fill_bits(sc.bits_per_pixel-sc.transp.length);
				bp.gray.rshift=(sc.bits_per_pixel-sc.transp.length)-bp.gray.depth;
				bp.alpha.mask=fill_bits(sc.transp.length)<<(sc.transp.offset-1);
				bp.alpha.rshift=(sc.transp.offset-1)+(sc.transp.length-bp.alpha.depth);
			}
			bp.gray.rshift-=png_effective_bytes_per_pixel_color*8-bp.gray.depth;
			bp.alpha.rshift-=png_effective_bytes_per_pixel_color*8-bp.alpha.depth;
		}
	}else{
		if(sc.transp.length==0){
			png_colortype=PNG_COLOR_TYPE_RGB;
			color_components=3;
			png_effective_bytes_per_pixel_color=((sc.red.length<=8)&&(sc.green.length<=8)&&(sc.blue.length<=8))?1:2;
		}else{
			png_colortype=PNG_COLOR_TYPE_RGB_ALPHA;
			color_components=4;
			png_effective_bytes_per_pixel_color=((sc.red.length<=8)&&(sc.green.length<=8)&&(sc.blue.length<=8)&&(sc.transp.length<=8))?1:2;
		}

		for(bp.red.depth=sc.red.length; bp.red.depth>png_effective_bytes_per_pixel_color*8; bp.red.depth--)
			;
		for(bp.green.depth=sc.green.length; bp.green.depth>png_effective_bytes_per_pixel_color*8; bp.green.depth--)
			;
		for(bp.blue.depth=sc.blue.length; bp.blue.depth>png_effective_bytes_per_pixel_color*8; bp.blue.depth--)
			;
		if(sc.transp.length!=0)
			for(bp.alpha.depth=sc.transp.length; bp.alpha.depth>png_effective_bytes_per_pixel_color*8; bp.alpha.depth--)
				;

		bp.red.mask=fill_bits(sc.red.length)<<sc.red.offset;
		bp.red.rshift=sc.red.offset+(sc.red.length-bp.red.depth)-(png_effective_bytes_per_pixel_color*8-bp.red.depth);
		bp.green.mask=fill_bits(sc.green.length)<<sc.green.offset;
		bp.green.rshift=sc.green.offset+(sc.green.length-bp.green.depth)-(png_effective_bytes_per_pixel_color*8-bp.green.depth);
		bp.blue.mask=fill_bits(sc.blue.length)<<sc.blue.offset;
		bp.blue.rshift=sc.blue.offset+(sc.blue.length-bp.blue.depth)-(png_effective_bytes_per_pixel_color*8-bp.blue.depth);
		if(sc.transp.length!=0){
			bp.alpha.mask=fill_bits(sc.transp.length)<<sc.transp.offset;
			bp.alpha.rshift=sc.transp.offset+(sc.transp.length-bp.alpha.depth)-(png_effective_bytes_per_pixel_color*8-bp.alpha.depth);
		}
	}

	switch(fb_effective_bytes_per_pixel){
		case 1:
			fb_pointer_size=sizeof(uint8_t*);
			break;

		case 2:
			fb_pointer_size=sizeof(uint16_t*);
			break;

		case 4:
			fb_pointer_size=sizeof(uint32_t*);
			break;

		case 8:
			fb_pointer_size=sizeof(uint64_t*);
			break;

		default:
			fprintf(stderr, "error: unknown fb_effective_bytes_per_pixel (internal error)\n");
			exit(EXIT_FAILURE);
	}

	switch(png_effective_bytes_per_pixel_color){
		case 1:
			png_pointer_size=sizeof(uint8_t*);
			break;

		case 2:
			png_pointer_size=sizeof(uint16_t*);
			break;

		default:
			fprintf(stderr, "error: unknown png_effective_bytes_per_pixel_color (internal error)\n");
			exit(EXIT_FAILURE);
	}

	return;
}

uint8_t *encode_png(void *fbbuf_1dim, uint32_t *imagesize)
{
	uint32_t i, j;
	png_bytepp finalbuf;
	uint8_t **fbbuf_orig;
	uint8_t *finalbuf_orig_1dim, **finalbuf_orig;
	uint8_t **buf_8;
	uint16_t **buf_16;
	uint32_t **buf_32;
	uint64_t **buf_64;
	uint8_t **finalbuf_8;
	uint16_t **finalbuf_16;

	fbbuf_orig=(uint8_t**)malloc(height*fb_pointer_size);
	if(fbbuf_orig==NULL){
		fprintf(stderr, "error: failed to malloc fbbuf_orig\n");
		exit(EXIT_FAILURE);
	}

	finalbuf_orig_1dim=(uint8_t*)malloc(width*height*color_components*png_effective_bytes_per_pixel_color);
	if(finalbuf_orig_1dim==NULL){
		fprintf(stderr, "error: failed to malloc finalbuf_orig_1dim\n");
		exit(EXIT_FAILURE);
	}

	finalbuf_orig=(uint8_t**)malloc(height*png_pointer_size);
	if(finalbuf_orig==NULL){
		fprintf(stderr, "error: failed to malloc finalbuf_orig\n");
		exit(EXIT_FAILURE);
	}

	for(i=0; i<height; i++){
		fbbuf_orig[i]=(uint8_t*)fbbuf_1dim+(width*fb_effective_bytes_per_pixel)*i;
		finalbuf_orig[i]=(uint8_t*)finalbuf_orig_1dim+(width*color_components*png_effective_bytes_per_pixel_color)*i;
	}

	switch(png_effective_bytes_per_pixel_color){
		case 1:
			finalbuf_8=(uint8_t**)finalbuf_orig;
			finalbuf=(png_bytepp)finalbuf_8;
			switch(fb_effective_bytes_per_pixel){
				case 1:
					buf_8=(uint8_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j]=(buf_8[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j]=(buf_8[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j*2+0]=(buf_8[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j*2+0]=(buf_8[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*2+1]=(buf_8[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*2+1]=(buf_8[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*3+0]=(buf_8[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*3+0]=(buf_8[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*3+1]=(buf_8[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*3+1]=(buf_8[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*3+2]=(buf_8[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*3+2]=(buf_8[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*4+0]=(buf_8[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*4+0]=(buf_8[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*4+1]=(buf_8[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*4+1]=(buf_8[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*4+2]=(buf_8[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*4+2]=(buf_8[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*4+3]=(buf_8[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*4+3]=(buf_8[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 2:
					buf_16=(uint16_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j]=(buf_16[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j]=(buf_16[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j*2+0]=(buf_16[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j*2+0]=(buf_16[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*2+1]=(buf_16[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*2+1]=(buf_16[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*3+0]=(buf_16[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*3+0]=(buf_16[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*3+1]=(buf_16[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*3+1]=(buf_16[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*3+2]=(buf_16[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*3+2]=(buf_16[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*4+0]=(buf_16[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*4+0]=(buf_16[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*4+1]=(buf_16[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*4+1]=(buf_16[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*4+2]=(buf_16[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*4+2]=(buf_16[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*4+3]=(buf_16[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*4+3]=(buf_16[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 4:
					buf_32=(uint32_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j]=(buf_32[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j]=(buf_32[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j*2+0]=(buf_32[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j*2+0]=(buf_32[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*2+1]=(buf_32[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*2+1]=(buf_32[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*3+0]=(buf_32[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*3+0]=(buf_32[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*3+1]=(buf_32[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*3+1]=(buf_32[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*3+2]=(buf_32[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*3+2]=(buf_32[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*4+0]=(buf_32[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*4+0]=(buf_32[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*4+1]=(buf_32[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*4+1]=(buf_32[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*4+2]=(buf_32[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*4+2]=(buf_32[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*4+3]=(buf_32[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*4+3]=(buf_32[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 8:
					buf_64=(uint64_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j]=(buf_64[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j]=(buf_64[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_8[i][j*2+0]=(buf_64[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_8[i][j*2+0]=(buf_64[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*2+1]=(buf_64[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*2+1]=(buf_64[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*3+0]=(buf_64[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*3+0]=(buf_64[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*3+1]=(buf_64[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*3+1]=(buf_64[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*3+2]=(buf_64[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*3+2]=(buf_64[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_8[i][j*4+0]=(buf_64[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_8[i][j*4+0]=(buf_64[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_8[i][j*4+1]=(buf_64[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_8[i][j*4+1]=(buf_64[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_8[i][j*4+2]=(buf_64[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_8[i][j*4+2]=(buf_64[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_8[i][j*4+3]=(buf_64[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_8[i][j*4+3]=(buf_64[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				default:
					fprintf(stderr, "error: unknown fb_effective_bytes_per_pixel (internal error)\n");
					exit(EXIT_FAILURE);
			}
			break;

		case 2:
			finalbuf_16=(uint16_t**)finalbuf_orig;
			finalbuf=(png_bytepp)finalbuf_16;
			switch(fb_effective_bytes_per_pixel){
				case 1:
					buf_8=(uint8_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j]=(buf_8[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j]=(buf_8[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j*2+0]=(buf_8[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j*2+0]=(buf_8[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*2+1]=(buf_8[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*2+1]=(buf_8[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*3+0]=(buf_8[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*3+0]=(buf_8[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*3+1]=(buf_8[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*3+1]=(buf_8[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*3+2]=(buf_8[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*3+2]=(buf_8[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*4+0]=(buf_8[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*4+0]=(buf_8[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*4+1]=(buf_8[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*4+1]=(buf_8[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*4+2]=(buf_8[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*4+2]=(buf_8[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*4+3]=(buf_8[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*4+3]=(buf_8[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 2:
					buf_16=(uint16_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j]=(buf_16[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j]=(buf_16[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j*2+0]=(buf_16[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j*2+0]=(buf_16[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*2+1]=(buf_16[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*2+1]=(buf_16[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*3+0]=(buf_16[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*3+0]=(buf_16[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*3+1]=(buf_16[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*3+1]=(buf_16[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*3+2]=(buf_16[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*3+2]=(buf_16[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*4+0]=(buf_16[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*4+0]=(buf_16[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*4+1]=(buf_16[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*4+1]=(buf_16[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*4+2]=(buf_16[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*4+2]=(buf_16[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*4+3]=(buf_16[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*4+3]=(buf_16[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 4:
					buf_32=(uint32_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j]=(buf_32[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j]=(buf_32[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j*2+0]=(buf_32[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j*2+0]=(buf_32[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*2+1]=(buf_32[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*2+1]=(buf_32[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*3+0]=(buf_32[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*3+0]=(buf_32[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*3+1]=(buf_32[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*3+1]=(buf_32[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*3+2]=(buf_32[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*3+2]=(buf_32[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*4+0]=(buf_32[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*4+0]=(buf_32[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*4+1]=(buf_32[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*4+1]=(buf_32[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*4+2]=(buf_32[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*4+2]=(buf_32[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*4+3]=(buf_32[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*4+3]=(buf_32[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				case 8:
					buf_64=(uint64_t**)fbbuf_orig;
					switch(png_colortype){
						case PNG_COLOR_TYPE_GRAY:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j]=(buf_64[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j]=(buf_64[i][j]&bp.gray.mask)<<-bp.gray.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_GRAY_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.gray.rshift>=0)
										finalbuf_16[i][j*2+0]=(buf_64[i][j]&bp.gray.mask)>>bp.gray.rshift;
									else
										finalbuf_16[i][j*2+0]=(buf_64[i][j]&bp.gray.mask)<<-bp.gray.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*2+1]=(buf_64[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*2+1]=(buf_64[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*3+0]=(buf_64[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*3+0]=(buf_64[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*3+1]=(buf_64[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*3+1]=(buf_64[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*3+2]=(buf_64[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*3+2]=(buf_64[i][j]&bp.blue.mask)<<-bp.blue.rshift;
								}
							}
							break;

						case PNG_COLOR_TYPE_RGB_ALPHA:
							for(i=0; i<height; i++){
								for(j=0; j<width; j++){
									if(bp.red.rshift>=0)
										finalbuf_16[i][j*4+0]=(buf_64[i][j]&bp.red.mask)>>bp.red.rshift;
									else
										finalbuf_16[i][j*4+0]=(buf_64[i][j]&bp.red.mask)<<-bp.red.rshift;
									if(bp.green.rshift>=0)
										finalbuf_16[i][j*4+1]=(buf_64[i][j]&bp.green.mask)>>bp.green.rshift;
									else
										finalbuf_16[i][j*4+1]=(buf_64[i][j]&bp.green.mask)<<-bp.green.rshift;
									if(bp.blue.rshift>=0)
										finalbuf_16[i][j*4+2]=(buf_64[i][j]&bp.blue.mask)>>bp.blue.rshift;
									else
										finalbuf_16[i][j*4+2]=(buf_64[i][j]&bp.blue.mask)<<-bp.blue.rshift;
									if(bp.alpha.rshift>=0)
										finalbuf_16[i][j*4+3]=(buf_64[i][j]&bp.alpha.mask)>>bp.alpha.rshift;
									else
										finalbuf_16[i][j*4+3]=(buf_64[i][j]&bp.alpha.mask)<<-bp.alpha.rshift;
								}
							}
							break;
						default:
							fprintf(stderr, "error: unknown png_colortype (internal error)\n");
							exit(EXIT_FAILURE);
					}
					break;

				default:
					fprintf(stderr, "error: unknown fb_effective_bytes_per_pixel (internal error)\n");
					exit(EXIT_FAILURE);
			}
			break;

		default:
			fprintf(stderr, "error: unknown png_effective_bytes_per_pixel_color (internal error)\n");
			exit(EXIT_FAILURE);
	}

	encode_png_core(finalbuf, imagesize);

	return retbuf;
}

void encode_png_finalize()
{
	free(retbuf);

	return;
}

void encode_png_core(uint8_t **finalbuf, uint32_t *imagesize)
{
	int pipefd[2];
	pid_t cpid;

	if(pipe(pipefd)==-1){
		perror("pipe");
		exit(EXIT_FAILURE);
	}

	cpid=fork();
	if(cpid==-1){
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if(cpid==0){
		FILE *fp;
		png_structp png_ptr;
		png_infop info_ptr;

		close(pipefd[0]);
		
		png_ptr=png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
		if(!png_ptr){
			fprintf(stderr, "error: png_create_write_struct returned zero\n");
			_exit(EXIT_FAILURE);
		}

		info_ptr=png_create_info_struct(png_ptr);
		if(!info_ptr){
			png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
			fprintf(stderr, "error: png_create_info_struct returned zero\n");
			_exit(EXIT_FAILURE);
		}

		if(setjmp(png_jmpbuf(png_ptr))){
			png_destroy_write_struct(&png_ptr, &info_ptr);
			fprintf(stderr, "info: libpng used setjmp\n");
			_exit(EXIT_FAILURE);
		}

		fp=fdopen(pipefd[1], "wb");
		if(fp==NULL){
			perror("fdopen");
			_exit(EXIT_FAILURE);
		}

		png_init_io(png_ptr, fp);
		png_set_IHDR(png_ptr, info_ptr, width, height, png_effective_bytes_per_pixel_color*8, png_colortype, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
		png_set_compression_level(png_ptr, clevel);
		png_write_info(png_ptr, info_ptr);
		png_write_image(png_ptr, finalbuf);
		png_write_end(png_ptr, info_ptr);
		png_destroy_write_struct(&png_ptr, &info_ptr);

		fclose(fp);

		_exit(EXIT_SUCCESS);
	}else{
		uint8_t *retbuf_orig;
		struct buf_linear_list_node *lbuf_orig=NULL;
		struct buf_linear_list_node *lbuf=NULL;
		struct buf_linear_list_node *lbuf_prev=NULL;
		pid_t w;
		int status;

		close(pipefd[1]);

		for(;;){
			ssize_t rc;

			lbuf_prev=lbuf;
			if(lbuf==NULL){
				lbuf=(struct buf_linear_list_node*)malloc(1*sizeof(struct buf_linear_list_node));
				if(lbuf==NULL){
					fprintf(stderr, "error: failed to malloc lbuf\n");
					exit(EXIT_FAILURE);
				}
				lbuf_orig=lbuf;
				*imagesize=0;
			}else{
				lbuf->next=(struct buf_linear_list_node*)malloc(1*sizeof(struct buf_linear_list_node));
				if(lbuf->next==NULL){
					fprintf(stderr, "error: failed to malloc lbuf\n");
					exit(EXIT_FAILURE);
				}
				lbuf=lbuf->next;
			}
			lbuf->next=NULL;

			lbuf->p=(uint8_t*)malloc(RETBUFSIZESTEP*sizeof(uint8_t));
			if(lbuf->p==NULL){
				fprintf(stderr, "error: failed to malloc lbuf->p\n");
				exit(EXIT_FAILURE);
			}

			rc=read(pipefd[0], lbuf->p, RETBUFSIZESTEP);
			if(rc==-1){
				perror("read");
				exit(EXIT_FAILURE);
			}else if(rc==0){
				free(lbuf->p);
				free(lbuf);
				lbuf=NULL;
				if(lbuf_prev!=NULL)
					lbuf_prev->next=NULL;
				if(lbuf==lbuf_orig)
					lbuf_orig=NULL;
				break;
			}
			lbuf->len=(uint32_t)rc;
			*imagesize+=lbuf->len;

			if(rc!=RETBUFSIZESTEP){
				if((lbuf->p=realloc(lbuf->p, lbuf->len))==NULL){
					fprintf(stderr, "error: failed to realloc lbuf->p\n");
					exit(EXIT_FAILURE);
				}
			}
		}
		close(pipefd[0]);

		if(localimagesize!=*imagesize){
			retbuf=(uint8_t*)malloc(*imagesize*sizeof(uint8_t));
			if(retbuf==NULL){
				fprintf(stderr, "error: failed to malloc retbuf\n");
				exit(EXIT_FAILURE);
			}
			localimagesize=*imagesize;
		}

		retbuf_orig=retbuf;

		while(lbuf_orig!=NULL){
			struct buf_linear_list_node *p;

			memcpy(retbuf, lbuf_orig->p, lbuf_orig->len);
			retbuf+=lbuf_orig->len;
			p=lbuf_orig;
			lbuf_orig=lbuf_orig->next;
			free(p->p);
			free(p);
		}

		retbuf=retbuf_orig;

		if((w=waitpid(cpid, &status, 0))==-1){
			perror("waitpid");
			exit(EXIT_FAILURE);
		}

		if(!WIFEXITED(status)){
			fprintf(stderr, "error: child did not use _exit(2) or exit(3)\n");
			exit(EXIT_FAILURE);
		}else if(WEXITSTATUS(status)!=EXIT_SUCCESS){
			fprintf(stderr, "error: child exited with unexpected value: %d\n", WEXITSTATUS(status));
			exit(EXIT_FAILURE);
		}else if(WIFSIGNALED(status)){
#ifdef WCOREDUMP
			if(WCOREDUMP(status)){
				fprintf(stderr, "error: child was core dumped\n");
				exit(EXIT_FAILURE);
			}
#endif /* WCOREDUMP */
			fprintf(stderr, "error: child was killed by signal %d\n", WTERMSIG(status));
			exit(EXIT_FAILURE);
		}
	}

	return;
}
