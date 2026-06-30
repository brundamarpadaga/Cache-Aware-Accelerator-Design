// matmul_tb.cpp
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Declaration matching your top-level function in matmul.cpp
void matmul(float *A, float *B, float *B_T, float *C, int N);

int main() {
    const int N = 32;   // multiple of TILE=4, exercises multi-tile accumulation

    static float A[N*N], B[N*N], B_T[N*N], C[N*N], C_ref[N*N];

    // A = patterned, NOT identity — every row/column distinct
    // A[i][j] = (i + 2*j + 1) % 5 + 1    values in [1,5], no symmetry
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            A[i*N+j] = (float)((i + 2*j + 1) % 5 + 1);

    // B = a different pattern so A and B aren't accidentally identical
    // B[i][j] = (3*i + j + 2) % 7 + 1    values in [1,7]
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            B[i*N+j] = (float)((3*i + j + 2) % 7 + 1);

    // Software reference: standard triple-loop matmul, C_ref = A x B
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < N; k++)
                sum += A[i*N+k] * B[k*N+j];
            C_ref[i*N+j] = sum;
        }

    // Call the HLS kernel under test
    matmul(A, B, B_T, C, N);

    // Compare with a tolerance (floating point accumulation order differs
    // between the tiled hardware path and this simple reference loop)
    int errors = 0;
    for (int i = 0; i < N*N; i++) {
        float diff = fabsf(C[i] - C_ref[i]);
        float tol  = 1e-2f * fmaxf(1.0f, fabsf(C_ref[i]));  // relative-ish tolerance
        if (diff > tol) {
            printf("MISMATCH at [%d]: got %f expected %f (diff %f)\n",
                   i, C[i], C_ref[i], diff);
            errors++;
        }
    }

    printf(errors == 0 ? "TEST PASSED\n" : "TEST FAILED (%d errors)\n", errors);
    return errors;
}
