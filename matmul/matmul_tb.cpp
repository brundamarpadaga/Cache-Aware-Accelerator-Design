// matmul_tb.cpp
#include <cstdio>
#include <cstdlib>
#include <cmath>

// Declaration matching your top-level function in matmul.cpp
void matmul(float *A, float *B, float *B_T, float *C, int N);

int main() {
    const int N = 32;   // keep small for fast simulation

    static float A[N*N], B[N*N], B_T[N*N], C[N*N], C_ref[N*N];

    // A = identity matrix
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            A[i*N+j] = (i == j) ? 1.0f : 0.0f;

    // B = known pattern
    for (int i = 0; i < N*N; i++)
        B[i] = (float)(i % 7) + 1.0f;

    // Software reference: identity * B = B
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < N; k++)
                sum += A[i*N+k] * B[k*N+j];
            C_ref[i*N+j] = sum;
        }

    // Call the HLS kernel under test
    matmul(A, B, B_T, C, N);

    // Compare
    int errors = 0;
    for (int i = 0; i < N*N; i++) {
        float diff = fabsf(C[i] - C_ref[i]);
        if (diff > 1e-3f) {
            printf("MISMATCH at [%d]: got %f expected %f\n", i, C[i], C_ref[i]);
            errors++;
        }
    }

    printf(errors == 0 ? "TEST PASSED\n" : "TEST FAILED (%d errors)\n", errors);
    return errors;   // non-zero return = simulation marked as FAILED in HLS
}
