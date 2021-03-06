// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#pragma once
#include <stdlib.h>
#include "compiler.h"

/// Code for generating convolution kernels

typedef struct conv {
	int w, h;
	double *rsum;
	double data[];
} conv;

/// Calculate the sum of a rectangle part of the convolution kernel
/// the rectangle is defined by top left (x, y), and a size (width x height)
double attr_pure sum_kernel(const conv *map, int x, int y, uint width, uint height);
double attr_pure sum_kernel_normalized(const conv *map, int x, int y, uint width, uint height);

/// Create a kernel with gaussian distribution of radius r
conv *gaussian_kernel(double r);

/// preprocess kernels to make shadow generation faster
/// shadow_sum[x*d+y] is the sum of the kernel from (0, 0) to (x, y), inclusive
void sum_kernel_preprocess(conv *map);

static inline void free_conv(conv *k) {
	free(k->rsum);
	free(k);
}
