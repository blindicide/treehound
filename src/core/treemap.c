/* SPDX-License-Identifier: MIT */
#include "treehound/treemap.h"
#include <math.h>
static void layout(const double *w, size_t n, double sum, th_rect b, th_rect *out)
{
    if (!n || sum <= 0) return;
    if (n == 1) { out[0] = b; return; }
    size_t split = 1; double left = w[0];
    while (split < n - 1 && left + w[split] <= sum * .5) left += w[split++];
    /* A pathological sequence of zeros must still have bounded recursion. */
    if (left <= 0 || left >= sum) {
        split = n / 2; left = 0;
        for (size_t i = 0; i < split; i++) left += w[i];
    }
    th_rect a = b, c = b; double fraction = left / sum;
    if (b.width >= b.height) { a.width *= fraction; c.x += a.width; c.width -= a.width; }
    else { a.height *= fraction; c.y += a.height; c.height -= a.height; }
    layout(w, split, left, a, out); layout(w + split, n - split, sum - left, c, out + split);
}
void th_treemap(const double *weights, size_t n, th_rect bounds, th_rect *out)
{
    double sum = 0;
    for (size_t i = 0; i < n; i++) out[i] = (th_rect){0};
    for (size_t i = 0; i < n; i++) { if (!isfinite(weights[i]) || weights[i] < 0) return; sum += weights[i]; }
    if (!isfinite(sum) || bounds.width <= 0 || bounds.height <= 0) return;
    layout(weights, n, sum, bounds, out);
}
