/* SPDX-License-Identifier: MIT */
#include "treehound/treemap.h"
#include <assert.h>
#include <math.h>
int main(void)
{
    double w[513]; th_rect r[513]; double total = 0;
    for (size_t i = 0; i < 513; i++) { w[i] = i % 7 ? (double)(513 - i) : 0; total += w[i]; }
    th_treemap(w, 513, (th_rect){3, 7, 1000, 600}, r);
    double area = 0;
    for (size_t i = 0; i < 513; i++) {
        double a = r[i].width * r[i].height; area += a;
        assert(fabs(a - 600000 * w[i] / total) < .00001);
        if (!a) continue;
        assert(r[i].x >= 3 && r[i].y >= 7 && r[i].x + r[i].width <= 1003.00001 && r[i].y + r[i].height <= 607.00001);
        for (size_t j = 0; j < i; j++) if (r[j].width * r[j].height > 0)
            assert(fmin(r[i].x+r[i].width,r[j].x+r[j].width)-fmax(r[i].x,r[j].x) < .00001 || fmin(r[i].y+r[i].height,r[j].y+r[j].height)-fmax(r[i].y,r[j].y) < .00001);
    }
    assert(fabs(area - 600000) < .00001);
    for (size_t i = 0; i < 513; i++) w[i] = 0;
    w[512] = 1; th_treemap(w, 513, (th_rect){0,0,100,100}, r);
    assert(r[512].width * r[512].height == 10000);
    th_treemap(w, 513, (th_rect){0,0,0,0}, r); assert(r[512].width == 0);
    return 0;
}
