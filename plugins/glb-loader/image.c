/* PNG to RGBA8: chunks, zlib inflate, the five scanline filters, Adam7, every
 * colour type and bit depth (16-bit keeps the high byte), tRNS. */
#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/* Keeps a hostile header from asking for gigabytes. */
#define MAX_SIDE 8192u

static uint32_t be32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool fail(char *error, size_t size, const char *message) {
	snprintf(error, size, "%s", message);
	return false;
}

bool image_is_jpeg(const uint8_t *data, size_t size) {
	return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

struct png {
	uint32_t width, height;
	int depth, color;
	int channels;         /* samples per pixel */
	uint8_t palette[256][4];
	int palette_size;
	bool has_trns_key;
	uint16_t trns_key[3];
};

static int paeth(int a, int b, int c) {
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Undo the filters of one (sub)image in place: rows of 1 + stride bytes. */
static bool unfilter(uint8_t *data, uint32_t rows, size_t stride, size_t bpp) {
	uint8_t *prev = NULL;
	for (uint32_t y = 0; y < rows; ++y) {
		uint8_t *row = data + y * (stride + 1);
		uint8_t filter = row[0];
		uint8_t *cur = row + 1;
		for (size_t x = 0; x < stride; ++x) {
			int a = x >= bpp ? cur[x - bpp] : 0;
			int b = prev ? prev[x] : 0;
			int c = prev && x >= bpp ? prev[x - bpp] : 0;
			switch (filter) {
			case 0: break;
			case 1: cur[x] = (uint8_t)(cur[x] + a); break;
			case 2: cur[x] = (uint8_t)(cur[x] + b); break;
			case 3: cur[x] = (uint8_t)(cur[x] + (a + b) / 2); break;
			case 4: cur[x] = (uint8_t)(cur[x] + paeth(a, b, c)); break;
			default: return false;
			}
		}
		prev = cur;
	}
	return true;
}

/* Sample i of a row at the image's bit depth. */
static unsigned sample(const uint8_t *row, size_t i, int depth) {
	switch (depth) {
	case 16: return (unsigned)row[i * 2] << 8 | row[i * 2 + 1];
	case 8: return row[i];
	default: {
		size_t bit = i * (size_t)depth;
		unsigned byte = row[bit / 8];
		unsigned shift = 8 - depth - (unsigned)(bit % 8);
		return byte >> shift & ((1u << depth) - 1);
	}
	}
}

static void put_pixel(const struct png *p, const uint8_t *row, uint32_t x, uint8_t *out) {
	int d = p->depth;
	unsigned max = (1u << d) - 1;
	/* To 8 bits: 16-bit keeps the high byte, low depths scale up. */
#define TO8(v) (uint8_t)(d == 16 ? (v) >> 8 : (v) * 255u / max)
	size_t s = (size_t)x * (size_t)p->channels;
	switch (p->color) {
	case 0: { /* grey */
		unsigned g = sample(row, s, d);
		out[0] = out[1] = out[2] = TO8(g);
		out[3] = p->has_trns_key && g == p->trns_key[0] ? 0 : 255;
		break;
	}
	case 2: { /* RGB */
		unsigned r = sample(row, s, d), g = sample(row, s + 1, d), b = sample(row, s + 2, d);
		out[0] = TO8(r);
		out[1] = TO8(g);
		out[2] = TO8(b);
		out[3] = p->has_trns_key && r == p->trns_key[0] && g == p->trns_key[1] &&
			b == p->trns_key[2] ? 0 : 255;
		break;
	}
	case 3: { /* palette */
		unsigned i = sample(row, s, d);
		if ((int)i < p->palette_size) memcpy(out, p->palette[i], 4);
		else memset(out, 0, 4);
		break;
	}
	case 4: /* grey + alpha */
		out[0] = out[1] = out[2] = TO8(sample(row, s, d));
		out[3] = TO8(sample(row, s + 1, d));
		break;
	default: /* RGBA */
		for (int c = 0; c < 4; ++c) out[c] = TO8(sample(row, s + (size_t)c, d));
		break;
	}
#undef TO8
}

/* Pixel bytes of a row of `width` pixels. */
static size_t row_bytes(const struct png *p, uint32_t width) {
	return ((size_t)width * (size_t)p->channels * (size_t)p->depth + 7) / 8;
}

bool image_decode_png(const uint8_t *data, size_t size, struct image *out,
		char *error, size_t error_size) {
	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	*out = (struct image){0};
	if (size < 8 || memcmp(data, signature, 8)) return fail(error, error_size, "not a PNG");

	struct png p = {0};
	bool interlaced = false, have_header = false;
	uint8_t *z = NULL;
	size_t z_size = 0, z_cap = 0;
	size_t pos = 8;
	while (pos + 12 <= size) {
		uint32_t length = be32(data + pos);
		const uint8_t *type = data + pos + 4, *body = data + pos + 8;
		if (length > size - pos - 12) {
			free(z);
			return fail(error, error_size, "truncated PNG chunk");
		}
		if (!memcmp(type, "IHDR", 4) && length >= 13) {
			p.width = be32(body);
			p.height = be32(body + 4);
			p.depth = body[8];
			p.color = body[9];
			interlaced = body[12] == 1;
			have_header = true;
			static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
			if (p.color > 6 || !channels[p.color] || body[10] || body[11] || body[12] > 1 ||
					!(p.depth == 1 || p.depth == 2 || p.depth == 4 || p.depth == 8 || p.depth == 16) ||
					((p.color == 2 || p.color == 4 || p.color == 6) && p.depth < 8) ||
					(p.color == 3 && p.depth > 8)) {
				free(z);
				return fail(error, error_size, "unsupported PNG format");
			}
			p.channels = channels[p.color];
			if (!p.width || !p.height || p.width > MAX_SIDE || p.height > MAX_SIDE) {
				free(z);
				return fail(error, error_size, "PNG too large or empty");
			}
		} else if (!memcmp(type, "PLTE", 4)) {
			p.palette_size = (int)(length / 3 > 256 ? 256 : length / 3);
			for (int i = 0; i < p.palette_size; ++i) {
				memcpy(p.palette[i], body + i * 3, 3);
				p.palette[i][3] = 255;
			}
		} else if (!memcmp(type, "tRNS", 4)) {
			if (p.color == 3) {
				for (uint32_t i = 0; i < length && i < 256; ++i) p.palette[i][3] = body[i];
			} else if (p.color == 0 && length >= 2) {
				p.has_trns_key = true;
				p.trns_key[0] = (uint16_t)(body[0] << 8 | body[1]);
			} else if (p.color == 2 && length >= 6) {
				p.has_trns_key = true;
				for (int c = 0; c < 3; ++c) p.trns_key[c] = (uint16_t)(body[c * 2] << 8 | body[c * 2 + 1]);
			}
		} else if (!memcmp(type, "IDAT", 4)) {
			if (z_size + length > z_cap) {
				size_t cap = z_cap ? z_cap : 65536;
				while (cap < z_size + length) cap *= 2;
				uint8_t *grown = realloc(z, cap);
				if (!grown) {
					free(z);
					return fail(error, error_size, "out of memory");
				}
				z = grown;
				z_cap = cap;
			}
			memcpy(z + z_size, body, length);
			z_size += length;
		} else if (!memcmp(type, "IEND", 4)) {
			break;
		}
		pos += 12 + length;
	}
	if (!have_header || !z_size || (p.color == 3 && !p.palette_size)) {
		free(z);
		return fail(error, error_size, "incomplete PNG");
	}

	/* The size of the filtered data, over the seven passes when interlaced. */
	static const int start_x[7] = { 0, 4, 0, 2, 0, 1, 0 }, start_y[7] = { 0, 0, 4, 0, 2, 0, 1 };
	static const int step_x[7] = { 8, 8, 4, 4, 2, 2, 1 }, step_y[7] = { 8, 8, 8, 4, 4, 2, 2 };
	int passes = interlaced ? 7 : 1;
	uint32_t pass_w[7], pass_h[7];
	size_t raw_size = 0;
	for (int i = 0; i < passes; ++i) {
		pass_w[i] = interlaced ? (p.width - (uint32_t)start_x[i] + (uint32_t)step_x[i] - 1) / (uint32_t)step_x[i] : p.width;
		pass_h[i] = interlaced ? (p.height - (uint32_t)start_y[i] + (uint32_t)step_y[i] - 1) / (uint32_t)step_y[i] : p.height;
		if (p.width <= (uint32_t)start_x[i] && interlaced) pass_w[i] = 0;
		if (p.height <= (uint32_t)start_y[i] && interlaced) pass_h[i] = 0;
		if (pass_w[i] && pass_h[i]) raw_size += (row_bytes(&p, pass_w[i]) + 1) * pass_h[i];
	}
	uint8_t *raw = malloc(raw_size ? raw_size : 1);
	uint8_t *rgba = malloc((size_t)p.width * p.height * 4);
	if (!raw || !rgba) {
		free(z);
		free(raw);
		free(rgba);
		return fail(error, error_size, "out of memory");
	}
	uLongf inflated = (uLongf)raw_size;
	int zr = uncompress(raw, &inflated, z, (uLong)z_size);
	free(z);
	if ((zr != Z_OK && zr != Z_BUF_ERROR) || inflated != raw_size) {
		free(raw);
		free(rgba);
		return fail(error, error_size, "corrupt PNG data");
	}

	size_t bpp = ((size_t)p.channels * (size_t)p.depth + 7) / 8;
	uint8_t *pass = raw;
	for (int i = 0; i < passes; ++i) {
		if (!pass_w[i] || !pass_h[i]) continue;
		size_t stride = row_bytes(&p, pass_w[i]);
		if (!unfilter(pass, pass_h[i], stride, bpp)) {
			free(raw);
			free(rgba);
			return fail(error, error_size, "bad PNG filter");
		}
		for (uint32_t y = 0; y < pass_h[i]; ++y) {
			const uint8_t *row = pass + y * (stride + 1) + 1;
			uint32_t oy = interlaced ? (uint32_t)start_y[i] + y * (uint32_t)step_y[i] : y;
			for (uint32_t x = 0; x < pass_w[i]; ++x) {
				uint32_t ox = interlaced ? (uint32_t)start_x[i] + x * (uint32_t)step_x[i] : x;
				put_pixel(&p, row, x, rgba + ((size_t)oy * p.width + ox) * 4);
			}
		}
		pass += (stride + 1) * pass_h[i];
	}
	free(raw);
	out->width = p.width;
	out->height = p.height;
	out->rgba = rgba;
	return true;
}
