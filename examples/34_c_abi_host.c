#include <stdio.h>

/* Cobra []f32 is a pointer followed by an element count on SysV x86-64. */
extern int cobra_affine(float *values, long length);

int main(void) {
    float values[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    int status = cobra_affine(values, 4);
    printf("status=%d values=%.1f,%.1f,%.1f,%.1f\n",
           status, values[0], values[1], values[2], values[3]);
    return status;
}
