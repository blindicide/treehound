/* SPDX-License-Identifier: MIT */
#ifndef TREEHOUND_TREEMAP_H
#define TREEHOUND_TREEMAP_H
#include <stddef.h>
typedef struct { double x, y, width, height; } th_rect;
/* Balanced binary layout, bounded stack, no allocations. Nonpositive weights
 * receive zero area. Input weights must be finite and nonnegative. */
void th_treemap(const double *weights, size_t n, th_rect bounds, th_rect *out);
#endif
