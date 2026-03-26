#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <png.h>
#include <string.h>
#include "escreen.h"

struct png_buffer {
	void *data;
	size_t size;
};

static void png_write_fn(png_structp png, png_bytep data, png_size_t length) {
	struct png_buffer *out = png_get_io_ptr(png);
	out->data = realloc(out->data, out->size + length);
	memcpy((uint8_t*)out->data + out->size, data, length);
	out->size += length;
}

static void png_flush_fn(png_structp png) {
	(void)png;
}

void image_save(struct escreen_state *state, void *data, int32_t width, int32_t height, int32_t stride) {
	struct png_buffer pb = {0};
	
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	
	if (setjmp(png_jmpbuf(png))) {
		fprintf(stderr, "Error during png creation\n");
		png_destroy_write_struct(&png, &info);
		free(pb.data);
		return;
	}

	png_set_write_fn(png, &pb, png_write_fn, png_flush_fn);
	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_bgr(png); // Cairo stores pixels as BGRA in memory on LE
	png_write_info(png, info);

	uint8_t *pixels = data;
	png_bytep row_pointers[height];
	for (int y = 0; y < height; y++) {
		row_pointers[y] = pixels + y * stride;
	}

	png_write_image(png, row_pointers);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);

	if (state->clipboard) {
		clipboard_send_data(state, pb.data, pb.size);
	}
	
	if (state->save_file) {
		char filename[256];
		snprintf(filename, sizeof(filename), "escreen-%ld.png", time(NULL));
		FILE *fp = fopen(filename, "wb");
		if (fp) {
			fwrite(pb.data, 1, pb.size, fp);
			fclose(fp);
			printf("Screenshot saved to %s\n", filename);
		} else {
			perror("fopen failed");
		}
	}
	
	free(pb.data);
}
