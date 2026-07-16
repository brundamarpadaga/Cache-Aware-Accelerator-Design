#include "ap_int.h"
#include "hls_stream.h"

/*Tile size
 * T=16 means each tile is 16×16 floats = 1KB of BRAM per tile.
 * Three tiles (a_tile, b_tile, c_tile) = 3KB total BRAM.
 * XC7Z020 has ~600KB BRAM so this is well within budget.
 * Reuse factor = T = 16 — each DDR read is reused 16 times before eviction.
*/
#define MAX_N  512

#define TILE        4    // compute sub-tile — controls unrolled MAC count (DSP budget, unchanged)
#define FETCH_TILE  8    // DDR fetch tile — controls how much data comes per round-trip
                          // FETCH_TILE must be a multiple of TILE; N must be a multiple of FETCH_TILE

/*Transpose B into B_T
 * Reads B row by row (sequential, burst-friendly via ACP).
 * Writes B_T column by column (strided writes via HP0).
 * After this, B_T[j][k] = B[k][j] — column j of B is now row j of B_T.
 * This makes B_T access sequential in the matmul kernel.
*/
#define T_TILE 16   // tune independently of matmul's TILE

void transpose(float *B, float *B_T, int N) {
    #pragma HLS INLINE off

    float tile_buf[T_TILE][T_TILE];

    for (int ti = 0; ti < N; ti += T_TILE) {
        for (int tj = 0; tj < N; tj += T_TILE) {

            /* Load — burst read T_TILE contiguous elements per row of B.
             * i is the outer (row) index, j is the fast/inner index —
             * matches B's natural row-major layout. */
            for (int i = 0; i < T_TILE; i++) {
                for (int j = 0; j < T_TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    tile_buf[i][j] = B[(ti + i) * N + (tj + j)];
                }
            }

            /* Store — burst write T_TILE contiguous elements per row of B_T.
             * j is now the outer (row) index into B_T, i is the fast/inner
             * index — this is the transpose, but written out so the DDR
             * write address is still sequential within each row. */
            for (int j = 0; j < T_TILE; j++) {
                for (int i = 0; i < T_TILE; i++) {
                    #pragma HLS PIPELINE II=1
                    B_T[(tj + j) * N + (ti + i)] = tile_buf[i][j];
                }
            }
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

    float a_tile[FETCH_TILE][FETCH_TILE];
    float b_tile[FETCH_TILE][FETCH_TILE];
    float c_tile[FETCH_TILE][FETCH_TILE];

    #pragma HLS ARRAY_PARTITION variable=a_tile complete dim=2
    #pragma HLS ARRAY_PARTITION variable=b_tile complete dim=2
    #pragma HLS ARRAY_PARTITION variable=c_tile complete dim=2

    for (int ti = 0; ti < N; ti += FETCH_TILE) {
        for (int tj = 0; tj < N; tj += FETCH_TILE) {

            // Zero the output super-tile
            for (int i = 0; i < FETCH_TILE; i++)
                for (int j = 0; j < FETCH_TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    c_tile[i][j] = 0.0f;
                }

            for (int tk = 0; tk < N; tk += FETCH_TILE) {

                // One DDR round-trip fetches FETCH_TILE x FETCH_TILE elements —
                // 4x the data per burst compared to TILE=4's old per-round-trip fetch.
                for (int i = 0; i < FETCH_TILE; i++)
                    for (int k = 0; k < FETCH_TILE; k++) {
                        #pragma HLS PIPELINE II=1
                        a_tile[i][k] = A[(ti + i) * N + (tk + k)];
                    }

                for (int j = 0; j < FETCH_TILE; j++)
                    for (int k = 0; k < FETCH_TILE; k++) {
                        #pragma HLS PIPELINE II=1
                        b_tile[j][k] = B_T[(tj + j) * N + (tk + k)];
                    }

                // Consume the fetched FETCH_TILE block using the SAME small
                // TILE-sized unrolled engine as before — DSP count unchanged.
                // si/sj/sk are NOT unrolled: they select which TILE-sized
                // sub-region of the already-on-chip tile the engine is
                // working on. Address only changes once per TILE-cycle burst,
                // not every cycle, so this does not reintroduce the
                // per-cycle accumulator mux from the README's attempt 2.
                for (int si = 0; si < FETCH_TILE; si += TILE) {
                    for (int sj = 0; sj < FETCH_TILE; sj += TILE) {
                        for (int sk = 0; sk < FETCH_TILE; sk += TILE) {
							#pragma HLS LOOP_FLATTEN off

                            for (int i = 0; i < TILE; i++) {
                                #pragma HLS PIPELINE II=1
                                for (int j = 0; j < TILE; j++) {
                                    #pragma HLS UNROLL
                                    float sum = 0.0f;
                                    for (int k = 0; k < TILE; k++) {
                                        #pragma HLS UNROLL
                                        sum += a_tile[si + i][sk + k] * b_tile[sj + j][sk + k];
                                    }
                                    c_tile[si + i][sj + j] += sum;
                                }
                            }
                        }
                    }
                }
            } // end tk

            // Write the completed output super-tile to DDR
            for (int i = 0; i < FETCH_TILE; i++)
                for (int j = 0; j < FETCH_TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    C[(ti + i) * N + (tj + j)] = c_tile[i][j];
                }
        }
    }
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
    #pragma HLS INTERFACE m_axi port=A   bundle=ACP offset=slave depth=MAX_N*MAX_N \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=B   bundle=ACP offset=slave depth=MAX_N*MAX_N \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=B_T bundle=ACP offset=slave depth=MAX_N*MAX_N \
        num_read_outstanding=16 max_read_burst_length=16
    #pragma HLS INTERFACE m_axi port=C   bundle=HP0 offset=slave depth=MAX_N*MAX_N \
        num_write_outstanding=16 max_write_burst_length=16
    #pragma HLS INTERFACE s_axilite port=N
    #pragma HLS INTERFACE s_axilite port=return

    transpose(B, B_T, N);
    matmul_tiled(A, B_T, C, N);
}
