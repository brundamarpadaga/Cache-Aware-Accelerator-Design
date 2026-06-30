#include "ap_int.h"
#include "hls_stream.h"

/*Tile size
 * T=16 means each tile is 16×16 floats = 1KB of BRAM per tile.
 * Three tiles (a_tile, b_tile, c_tile) = 3KB total BRAM.
 * XC7Z020 has ~600KB BRAM so this is well within budget.
 * Reuse factor = T = 16 — each DDR read is reused 16 times before eviction.
*/
#define MAX_N  512
#define TILE    4

/*Transpose B into B_T
 * Reads B row by row (sequential, burst-friendly via ACP).
 * Writes B_T column by column (strided writes via HP0).
 * After this, B_T[j][k] = B[k][j] — column j of B is now row j of B_T.
 * This makes B_T access sequential in the matmul kernel.
*/
void transpose(float *B, float *B_T, int N) {
    #pragma HLS INLINE off

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            #pragma HLS PIPELINE II=1
            B_T[j * N + i] = B[i * N + j];
        }
    }
}

/*Tiled matrix multiply
 * Computes C = A × B_T (where B_T is the transpose of B).
 *
 * Three levels of loops:
 *   Outer (ti, tj):  iterate over 16×16 tiles of the output matrix C
 *   Middle (tk):     iterate over 16×16 tiles along the k dimension
 *                    (accumulation direction)
 *   Inner (i,j,k):   compute within a tile entirely from BRAM
 *
 * DDR is only accessed in the tile load and tile write loops.
 * All multiply-accumulate operations read from BRAM (a_tile, b_tile).
*/
void matmul_tiled(float *A, float *B_T, float *C, int N) {
    #pragma HLS INLINE off

    /* On-chip BRAM tile buffers
     * a_tile[TILE][TILE] — holds a 16×16 block of A rows
     * b_tile[TILE][TILE] — holds a 16×16 block of B_T rows
     * c_tile[TILE][TILE] — accumulates the 16×16 output block
     *
     * ARRAY_PARTITION complete dim=2:
     * Splits the second dimension into TILE separate registers.
     * Allows all TILE elements of a row to be read or written in one cycle.
     * Without this, only one element per cycle is accessible (BRAM port limit).
     */
    float a_tile[TILE][TILE];
    float b_tile[TILE][TILE];
    float c_tile[TILE][TILE];

    #pragma HLS ARRAY_PARTITION variable=a_tile complete dim=2
    #pragma HLS ARRAY_PARTITION variable=b_tile complete dim=2
    #pragma HLS ARRAY_PARTITION variable=c_tile complete dim=2

    /*Tile loop over output matrix C\
     * ti: row tile index    (0, 16, 32, ... 496) — 32 iterations
     * tj: column tile index (0, 16, 32, ... 496) — 32 iterations
     * Total output tiles: 32 × 32 = 1024 tiles
     */
    for (int ti = 0; ti < MAX_N; ti += TILE) {
        for (int tj = 0; tj < MAX_N; tj += TILE) {

            /* Step 1 — Zero the output tile
             * c_tile accumulates partial sums across all tk iterations.
             * Must be zeroed before starting a new (ti,tj) output tile.
             * This loop runs TILE×TILE = 256 times per output tile.       */
            for (int i = 0; i < TILE; i++) {
                for (int j = 0; j < TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    c_tile[i][j] = 0.0f;
                }
            }

            /* Accumulation loop over k tiles
             * tk: tile index along accumulation dimension k
             * (0, 16, 32, ... 496) — 32 iterations
             *
             * For each (ti, tj) output tile, we accumulate contributions
             * from 32 pairs of A and B_T tiles.
             *
             * This is the key loop that enables reuse:
             * The same a_tile and b_tile are each reused TILE=16 times
             * across the i and j loops inside — zero extra DDR reads.
             */
            for (int tk = 0; tk < MAX_N; tk += TILE) {

                /* Step 2 — Load 16×16 tile of A from DDR into BRAM
                 *
                 * Accesses A[(ti+i)*N + (tk+k)] — sequential for each row i.
                 * HLS infers a burst of TILE=16 beats per row.
                 * Total DDR reads this step: TILE×TILE = 256 reads.
                 * This tile is then reused TILE=16 times in Step 4.       */
                for (int i = 0; i < TILE; i++) {
                    for (int k = 0; k < TILE; k++) {
                        #pragma HLS PIPELINE II=1
                        a_tile[i][k] = A[(ti + i) * N + (tk + k)];
                    }
                }

                /* Step 3 — Load 16×16 tile of B_T from DDR into BRAM
                 *
                 * Accesses B_T[(tj+j)*N + (tk+k)] — sequential because B
                 * was transposed. Without transpose this would be strided.
                 * Total DDR reads this step: TILE×TILE = 256 reads.
                 * This tile is then reused TILE=16 times in Step 4.       */
                for (int j = 0; j < TILE; j++) {
                    for (int k = 0; k < TILE; k++) {
                        #pragma HLS PIPELINE II=1
                        b_tile[j][k] = B_T[(tj + j) * N + (tk + k)];
                    }
                }

                /* Step 4 — Compute tile multiply-accumulate from BRAM only
                 *
                 * c_tile[i][j] += sum over k of a_tile[i][k] * b_tile[j][k]
                 *
                 * Both operands come from BRAM — zero DDR reads here.
                 * ARRAY_PARTITION on dim=2 means all k elements are
                 * accessible simultaneously — HLS unrolls the k loop.
                 *
                 * PIPELINE II=1 on the j loop:
                 * One new (i,j) output element starts every cycle.
                 * The k loop is fully unrolled by HLS because TILE is
                 * a compile-time constant and both arrays are partitioned.
                 *
                 * This is where the DSP48E1 tiles are used:
                 * TILE=16 parallel multiply-accumulate units.               */
                for (int i = 0; i < TILE; i++) {
                    #pragma HLS PIPELINE II=1
                    for (int j = 0; j < TILE; j++) {
                        #pragma HLS UNROLL
                        float sum = 0.0f;
                        for (int k = 0; k < TILE; k++) {
                            #pragma HLS UNROLL
                            sum += a_tile[i][k] * b_tile[k][j];
                        }
                        c_tile[i][j] += sum;
                    }
                }

            } /* end tk accumulation loop */

            /* Step 5 — Write completed output tile to DDR via HP0
             *
             * Each row of c_tile is written sequentially — burst-friendly.
             * Total DDR writes: TILE×TILE = 256 writes per output tile.
             * This is the only DDR write in the entire kernel.              */
            for (int i = 0; i < TILE; i++) {
                for (int j = 0; j < TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    C[(ti + i) * N + (tj + j)] = c_tile[i][j];
                }
            }

        } /* end tj loop */
    } /* end ti loop */
}

/* Top-level function — HLS synthesis entry point
 *
 * Ports:
 *   A    — input matrix A,        read via ACP (coherent, SCU snooping)
 *   B    — input matrix B,        read via ACP (coherent)
 *   B_T  — scratch buffer for B', written by transpose, read by matmul
 *   C    — output matrix C,       written via HP0 (non-coherent, bulk)
 *   N    — matrix dimension,      written by ARM via AXI-Lite
 *
 * AXI bundle assignment:
 *   ACP bundle: A, B, B_T — all reads use coherent ACP path
 *   HP0 bundle: C          — bulk write uses high-bandwidth HP0 path
 *
 * Execution sequence:
 *   1. transpose(B -> B_T)         converts column access to row access
 *   2. matmul_tiled(A, B_T -> C)   tiled multiply using BRAM for reuse
 */
void matmul(
    float *A,
    float *B,
    float *B_T,
    float *C,
    int    N
) {
	#pragma HLS INTERFACE m_axi port=A   bundle=ACP offset=slave \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=B   bundle=ACP offset=slave \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=B_T bundle=ACP offset=slave \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=C   bundle=HP0 offset=slave \
        num_write_outstanding=16 max_write_burst_length=16
    #pragma HLS INTERFACE s_axilite port=N
    #pragma HLS INTERFACE s_axilite port=return

    /* Step 1 — Transpose B in hardware
     * Reads B row-by-row (sequential AXI bursts via ACP).
     * Writes B_T column-by-column into DDR scratch buffer via HP0.
     * After this B_T[j][k] = B[k][j].                                    */
    transpose(B, B_T, N);

    /* Step 2 — Tiled matrix multiply
     * Reads A and B_T in 16×16 tile bursts via ACP.
     * Accumulates in BRAM tiles — zero DDR reads during multiply.
     * Writes result tiles to C via HP0 bulk bursts.                       */
    matmul_tiled(A, B_T, C, N);
}
