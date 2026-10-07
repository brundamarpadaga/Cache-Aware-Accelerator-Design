/**
 * @file   freertos_main.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @brief  FreeRTOS dual-core entry point for Zynq-7020.
 *
 * @details
 * Core 0 - FreeRTOS scheduler:
 *   - TaskHWMatmul : starts matmul_0 accelerator, blocks until ap_done,
 *                    probes Core 0 L1 PMU and shared L2 bracketing each run,
 *                    signals TaskPrefill after each size completes
 *   - TaskPrefill  : pre-fills staging buffers (MAT_A_NEXT / MAT_B_NEXT)
 *                    while the accelerator processes the current size;
 *                    TaskHWMatmul swaps active<->staging before next size
 *
 * Core 1 - bare-metal SW matmul (core1_entry.c):
 *   - Woken from WFE by Core 0 after FreeRTOS scheduler starts
 *   - Runs full NxN SW matmul sweep with its own L1 PMU and L2 probing
 *   - Signals Core 0 when done via shared DDR flags
 *
 * Double-buffer memory map:
 *   Active  : MAT_A 0x10000000 / MAT_B 0x10400000 (accelerator reads these)
 *   Staging : MAT_A_NEXT 0x14000000 / MAT_B_NEXT 0x14400000 (TaskPrefill writes here)
 *   TaskHWMatmul swaps the pointer pair after each size before next rep starts.
 *   MAT_B_T and MAT_C are single-buffered (written by accelerator, not prefilled).
 *
 * Build notes:
 *   - Set BSP OS to freertos10_xilinx in Vitis platform settings
 *   - Add MATMUL_MODE=2 to preprocessor symbols for HW accelerator path
 *   - ap_done interrupt from matmul_0 must be connected to PL-PS IRQ in
 *     the Vivado block design (IRQ_F2P[0]) for semaphore wakeup to work.
 *     Without this wiring, fall back to polling (see MATMUL_USE_INTERRUPT).
 *
 * @note Portions of this file were developed with assistance from
 *       Claude AI (Anthropic) - task structure, interrupt-driven semaphore
 *       wakeup pattern, Core 1 SEV/WFE handshake, and double-buffer prefill
 *       architecture.
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, FreeRTOS 10, arm-none-eabi-gcc
 */

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xscugic.h"
#include "xtime_l.h"
#include "pmu.h"

#ifdef XPAR_MATMUL_0_DEVICE_ID
#include "xmatmul.h"
XMatmul matmul_hw;
#endif


/* Active matrix buffers - accelerator reads from these */
#define MAT_A_BASE      0x10000000UL
#define MAT_B_BASE      0x10400000UL
#define MAT_C_BASE      0x10800000UL
#define MAT_B_T_BASE    0x10C00000UL

/* Staging buffers - TaskPrefill writes here while accelerator runs active buffers.
 * Placed at 0x14xxxxxx to avoid Core 1's SW region at 0x11xxxxxx. */
#define MAT_A_NEXT_BASE 0x14000000UL
#define MAT_B_NEXT_BASE 0x14400000UL

/* Core 1 communication region - uncached DDR so both cores see writes
 * without explicit flushing. */
#define CORE1_COMM_BASE     0x10F00000UL
#define CORE1_CMD           (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x00))
#define CORE1_STATUS        (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x04))
#define CORE1_ELAPSED_US    (*(volatile uint64_t *)(CORE1_COMM_BASE + 0x08))

#define CORE1_CMD_IDLE      0x00000000UL
#define CORE1_CMD_RUN       0xDEAD0001UL
#define CORE1_STATUS_BUSY   0x00000000UL
#define CORE1_STATUS_DONE   0xCAFE0001UL

/* Set MATMUL_USE_INTERRUPT=1 in preprocessor symbols when ap_done is
 * wired to IRQ_F2P[0] in the block design. */
#ifndef MATMUL_USE_INTERRUPT
#define MATMUL_USE_INTERRUPT 0
#endif

#if MATMUL_USE_INTERRUPT
static SemaphoreHandle_t xMatmulDoneSem = NULL;
static XScuGic xInterruptController;

static void matmul_done_isr(void *param)
{
    (void)param;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xMatmulDoneSem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void interrupt_init(void)
{
    XScuGic_Config *cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    XScuGic_CfgInitialize(&xInterruptController, cfg, cfg->CpuBaseAddress);
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
        (Xil_ExceptionHandler)XScuGic_InterruptHandler, &xInterruptController);
    Xil_ExceptionEnable();

    /* IRQ_F2P[0] maps to GIC interrupt 61 on Zynq */
    XScuGic_Connect(&xInterruptController, 61,
        (Xil_InterruptHandler)matmul_done_isr, NULL);
    XScuGic_Enable(&xInterruptController, 61);
}
#endif /* MATMUL_USE_INTERRUPT */

/* Benchmark parameters - must match core1_entry.c */
static const uint32_t SIZES[] = { 32U, 64U, 128U, 256U, 512U };
#define NUM_SIZES  (sizeof(SIZES) / sizeof(SIZES[0]))
#define REPEATS    5U

/* Semaphores for TaskHWMatmul <-> TaskPrefill synchronisation.
 * xPrefillStart: TaskHWMatmul gives, TaskPrefill takes (go fill next size)
 * xPrefillDone : TaskPrefill gives, TaskHWMatmul takes (staging ready to swap) */
static SemaphoreHandle_t xPrefillStart = NULL;
static SemaphoreHandle_t xPrefillDone  = NULL;

/* Next size index - written by TaskHWMatmul, read by TaskPrefill */
static volatile uint32_t next_size_index = 0U;

/* Task: HW accelerator + L1 PMU + L2 probe on Core 0 */
#ifdef XPAR_MATMUL_0_DEVICE_ID
static void TaskHWMatmul(void *pvParameters)
{
    (void)pvParameters;
    xil_printf("[Core0] TaskHWMatmul started\r\n");

    /* Active buffer pointers - swapped with staging after each size */
    UINTPTR active_a = MAT_A_BASE;
    UINTPTR active_b = MAT_B_BASE;
    UINTPTR staging_a = MAT_A_NEXT_BASE;
    UINTPTR staging_b = MAT_B_NEXT_BASE;

    for (uint32_t si = 0; si < NUM_SIZES; si++) {
        uint32_t N     = SIZES[si];
        uint32_t N2    = N * N;
        uint32_t bytes = N2 * sizeof(float);
        volatile float *A = (volatile float *)active_a;
        volatile float *B = (volatile float *)active_b;
        volatile float *C = (volatile float *)MAT_C_BASE;

        /* Fill active buffers for size[0]; subsequent sizes are pre-filled
         * by TaskPrefill into the staging region and swapped in below. */
        if (si == 0U) {
            for (uint32_t i = 0; i < N2; i++) A[i] = 1.0f + (float)i * 0.001f;
            for (uint32_t i = 0; i < N2; i++) B[i] = 2.0f + (float)i * 0.001f;
        }

        /* Signal TaskPrefill to pre-fill staging for the next size */
        if (si + 1U < NUM_SIZES) {
            next_size_index = si + 1U;
            xSemaphoreGive(xPrefillStart);
        }

        for (uint32_t rep = 0; rep < REPEATS; rep++) {
            Xil_DCacheFlushRange(active_a, bytes);
            Xil_DCacheFlushRange(active_b, bytes);
            for (uint32_t i = 0; i < N2; i++) C[i] = 0.0f;
            Xil_DCacheFlushRange((UINTPTR)MAT_C_BASE, bytes);
            Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, bytes);

            XMatmul_Set_A(&matmul_hw,   active_a);
            XMatmul_Set_B(&matmul_hw,   active_b);
            XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);
            XMatmul_Set_C(&matmul_hw,   MAT_C_BASE);
            XMatmul_Set_N(&matmul_hw,   N);

            /* Reset Core 0 L1 PMU and shared L2 immediately before start -
             * captures only accelerator execution, not setup overhead. */
            pmu_reset_counters();
            l2_reset();

            XTime t0, t1;
            XTime_GetTime(&t0);
            XMatmul_Start(&matmul_hw);

#if MATMUL_USE_INTERRUPT
            xSemaphoreTake(xMatmulDoneSem, portMAX_DELAY);
#else
            while (!XMatmul_IsDone(&matmul_hw))
                taskYIELD();
#endif

            XTime_GetTime(&t1);

            /* Read PMU and L2 immediately after ap_done - before any cache ops */
            pmu_counts_t pmu;
            l2_counts_t  l2;
            pmu_read_all(&pmu);
            l2_read(&l2);

            Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, bytes);

            uint64_t us = (uint64_t)(t1 - t0) * 1000000ULL
                          / (uint64_t)COUNTS_PER_SECOND;

            /* L2 hit rate: DRHIT/DRREQ. Core 0 is mostly idle during
             * accelerator execution so L2 reflects primarily ACP reads.
             * Core 0 L1 access count should be near-zero confirming idle. */
            uint32_t l2_rate_pct = (l2.drreq > 0U)
                ? (uint32_t)((l2.drhit * 100ULL) / l2.drreq) : 0U;

            taskENTER_CRITICAL();
            uart_lock_acquire();
            xil_printf("MATMUL,%lu,%lu,"
                       "L1acc=%lu,L1miss=%lu,"
                       "L2req=%lu,L2hit=%lu,L2pct=%lu\r\n",
                       (unsigned long)N,
                       (unsigned long)us,
                       (unsigned long)pmu.l1d_access,
                       (unsigned long)pmu.l1d_miss,
                       (unsigned long)l2.drreq,
                       (unsigned long)l2.drhit,
                       (unsigned long)l2_rate_pct);
            uart_lock_release();
            taskEXIT_CRITICAL();
        }

        /* Wait for TaskPrefill to finish staging the next size, then swap */
        if (si + 1U < NUM_SIZES) {
            xSemaphoreTake(xPrefillDone, portMAX_DELAY);

            UINTPTR tmp = active_a;
            active_a  = staging_a;
            staging_a = tmp;

            tmp       = active_b;
            active_b  = staging_b;
            staging_b = tmp;
        }
    }

    xil_printf("[Core0] TaskHWMatmul complete\r\n");
    vTaskDelete(NULL);
}
#endif /* XPAR_MATMUL_0_DEVICE_ID */

/**
 * @brief Pre-fill staging matrix buffers while the accelerator runs.
 *
 * Waits for TaskHWMatmul to signal which size to prepare, fills
 * MAT_A_NEXT / MAT_B_NEXT (staging), then signals back that staging
 * is ready. TaskHWMatmul swaps active<->staging before the next size.
 * Core 0 is otherwise free during accelerator execution.
 */
static void TaskPrefill(void *pvParameters)
{
    (void)pvParameters;
    xil_printf("[Core0] TaskPrefill started\r\n");

    for (;;) {
        /* Block until TaskHWMatmul signals a new size to prefill */
        if (xSemaphoreTake(xPrefillStart, portMAX_DELAY) != pdTRUE)
            continue;

        uint32_t si = next_size_index;
        if (si >= NUM_SIZES) {
            /* No more sizes - task is done */
            xil_printf("[Core0] TaskPrefill done\r\n");
            vTaskDelete(NULL);
        }

        uint32_t N     = SIZES[si];
        uint32_t N2    = N * N;
        uint32_t bytes = N2 * sizeof(float);
        volatile float *A_next = (volatile float *)MAT_A_NEXT_BASE;
        volatile float *B_next = (volatile float *)MAT_B_NEXT_BASE;

        for (uint32_t i = 0; i < N2; i++) A_next[i] = 1.0f + (float)i * 0.001f;
        for (uint32_t i = 0; i < N2; i++) B_next[i] = 2.0f + (float)i * 0.001f;

        /* Flush staging to DDR so the accelerator sees clean data after swap */
        Xil_DCacheFlushRange((UINTPTR)MAT_A_NEXT_BASE, bytes);
        Xil_DCacheFlushRange((UINTPTR)MAT_B_NEXT_BASE, bytes);

        taskENTER_CRITICAL();
        uart_lock_acquire();
        xil_printf("[Core0] TaskPrefill ready for N=%lu\r\n", (unsigned long)N);
        uart_lock_release();
        taskEXIT_CRITICAL();
        xSemaphoreGive(xPrefillDone);
    }
}

int main(void)
{
    xil_printf("\r\nTiled MatMul: FreeRTOS Dual-Core - Zybo Z7-20\r\n");

    pmu_init();
    l2_init();

#ifdef XPAR_MATMUL_0_DEVICE_ID
    XMatmul_Config *cfg = XMatmul_LookupConfig(XPAR_MATMUL_0_DEVICE_ID);
    if (!cfg) {
        xil_printf("ERROR: matmul_0 not found - rebuild platform from XSA\r\n");
        while (1) {}
    }
    XMatmul_CfgInitialize(&matmul_hw, cfg);
    xil_printf("matmul_0 base: 0x%08lX\r\n",
               (unsigned long)XPAR_MATMUL_0_S_AXI_CONTROL_BASEADDR);

#if MATMUL_USE_INTERRUPT
    xMatmulDoneSem = xSemaphoreCreateBinary();
    interrupt_init();
#endif
#endif

    xPrefillStart = xSemaphoreCreateBinary();
    xPrefillDone  = xSemaphoreCreateBinary();

    /* Signal Core 1 to start. Core 1 is already running (loaded by the
     * system project debugger on ps7_cortexa9_1) and polling CORE1_CMD.
     * Flush to DDR so Core 1's cache-invalidating poll sees the flag. */
    CORE1_CMD    = CORE1_CMD_IDLE;
    CORE1_STATUS = CORE1_STATUS_BUSY;
    *(volatile uint32_t *)(UART_LOCK_ADDR) = 0U;  /* zero before Core 1 starts */
    CORE1_CMD    = CORE1_CMD_RUN;
    Xil_DCacheFlushRange((UINTPTR)CORE1_COMM_BASE, 20UL);  /* includes UART_LOCK */
    __asm__ volatile("sev" ::: "memory");

    xil_printf("[Core0] Core1 signalled - starting FreeRTOS scheduler\r\n");

#ifdef XPAR_MATMUL_0_DEVICE_ID
    xTaskCreate(TaskHWMatmul, "HWMatmul", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
#endif
    xTaskCreate(TaskPrefill,  "Prefill",  2048, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    while (1) {}
    return 0;
}
