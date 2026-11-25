/* PNG decoding for glTF textures (zlib inflates; everything else is here). */
#ifndef SHADY_PLUGINS_IMAGE_H
#define SHADY_PLUGINS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct image {
	uint32_t width, height;
	uint8_t *rgba; /* width * height * 4, rows top first; malloc'd */
};

/* Decode a PNG of any colour type and bit depth, interlaced or not, to
 * RGBA8. On failure returns false with a reason in error. */
bool image_decode_png(const uint8_t *data, size_t size, struct image *out,
	char *error, size_t error_size);

/* Whether data starts like a JPEG, which is not decoded. */
bool image_is_jpeg(const uint8_t *data, size_t size);

#endif
