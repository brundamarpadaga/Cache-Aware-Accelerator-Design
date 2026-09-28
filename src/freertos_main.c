/**
 * @file   freertos_main.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @brief  FreeRTOS dual-core entry point for Zynq-7020.
 *
 * @details
 * Core 0 - FreeRTOS scheduler:
 *   - TaskHWMatmul : starts matmul_0 accelerator, blocks on semaphore
 *                    until ap_done interrupt fires (no busy-wait)
 *   - TaskMonitor  : runs during HW accelerator execution (Core 0 is free)
 *                    currently prints a heartbeat; replace with real work
 *
 * Core 1 - bare-metal SW matmul:
 *   - Woken from WFE by Core 0 after FreeRTOS scheduler starts
 *   - Runs full NxN SW matmul sweep independently
 *   - Signals Core 0 when done via shared flag in DDR
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
 *       wakeup pattern, and Core 1 SEV/WFE handshake.
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
#include "xil_mpu.h"
#include "xscugic.h"
#include "xtime_l.h"
#include "pmu.h"
#include "dma_smoke_test.h"
#include "benchmark.h"

#ifdef XPAR_MATMUL_0_DEVICE_ID
#include "xmatmul.h"
XMatmul matmul_hw;
#endif

/* Memory map (must match benchmark.c) */
#define MAT_A_BASE    0x10000000UL
#define MAT_B_BASE    0x10400000UL
#define MAT_C_BASE    0x10800000UL
#define MAT_B_T_BASE  0x10C00000UL

/* Core 1 communication region - uncached DDR so both cores see writes
 * without explicit flushing.
 * Core 0 writes CORE1_CMD_RUN to signal Core 1 to start.
 * Core 1 writes CORE1_STATUS_DONE when its SW sweep is finished. */
#define CORE1_COMM_BASE     0x10F00000UL
#define CORE1_CMD           (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x00))
#define CORE1_STATUS        (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x04))
#define CORE1_ELAPSED_US    (*(volatile uint64_t *)(CORE1_COMM_BASE + 0x08))

#define CORE1_CMD_IDLE      0x00000000UL
#define CORE1_CMD_RUN       0xDEAD0001UL
#define CORE1_STATUS_BUSY   0x00000000UL
#define CORE1_STATUS_DONE   0xCAFE0001UL

/* Set MATMUL_USE_INTERRUPT=1 in preprocessor symbols when ap_done is
 * wired to IRQ_F2P[0] in the block design.
 * Without it, TaskHWMatmul falls back to polling (same as current main.c). */
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

/* Benchmark sizes - must match benchmark.c */
static const uint32_t SIZES[] = { 32U, 64U, 128U, 256U, 512U };
#define NUM_SIZES  (sizeof(SIZES) / sizeof(SIZES[0]))
#define REPEATS    5U

/* Task: HW accelerator on Core 0 */
#ifdef XPAR_MATMUL_0_DEVICE_ID
static void TaskHWMatmul(void *pvParameters)
{
    (void)pvParameters;
    xil_printf("[Core0] TaskHWMatmul started\r\n");

    volatile float *A = (volatile float *)MAT_A_BASE;
    volatile float *B = (volatile float *)MAT_B_BASE;
    volatile float *C = (volatile float *)MAT_C_BASE;

    for (uint32_t si = 0; si < NUM_SIZES; si++) {
        uint32_t N     = SIZES[si];
        uint32_t N2    = N * N;
        uint32_t bytes = N2 * sizeof(float);
        XTime t0, t1;

        for (uint32_t i = 0; i < N2; i++) A[i] = 1.0f + (float)i * 0.001f;
        for (uint32_t i = 0; i < N2; i++) B[i] = 2.0f + (float)i * 0.001f;

        for (uint32_t rep = 0; rep < REPEATS; rep++) {
            Xil_DCacheFlushRange((UINTPTR)MAT_A_BASE, bytes);
            Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, bytes);
            for (uint32_t i = 0; i < N2; i++) C[i] = 0.0f;
            Xil_DCacheFlushRange((UINTPTR)MAT_C_BASE, bytes);
            Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, bytes);

            XMatmul_Set_A(&matmul_hw,   MAT_A_BASE);
            XMatmul_Set_B(&matmul_hw,   MAT_B_BASE);
            XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);
            XMatmul_Set_C(&matmul_hw,   MAT_C_BASE);
            XMatmul_Set_N(&matmul_hw,   N);

            XTime_GetTime(&t0);
            XMatmul_Start(&matmul_hw);

#if MATMUL_USE_INTERRUPT
            /* Block here - Core 0 is released to run TaskMonitor while PL works */
            xSemaphoreTake(xMatmulDoneSem, portMAX_DELAY);
#else
            /* Polling fallback - yields to other tasks between checks */
            while (!XMatmul_IsDone(&matmul_hw))
                taskYIELD();
#endif

            Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, bytes);
            XTime_GetTime(&t1);

            uint64_t us = (uint64_t)(t1 - t0) * 1000000ULL
                          / (uint64_t)COUNTS_PER_SECOND;
            xil_printf("MATMUL,%lu,%lu\r\n", (unsigned long)N, (unsigned long)us);
        }
    }

    xil_printf("[Core0] TaskHWMatmul complete\r\n");
    vTaskDelete(NULL);
}
#endif /* XPAR_MATMUL_0_DEVICE_ID */

/**
 * @brief Monitor task - runs on Core 0 while PL executes the accelerator.
 *
 * Polls for Core 1 completion and serves as a placeholder for useful work
 * that can run concurrently during accelerator execution (e.g. pre-filling
 * the next matrix pair, logging, diagnostics).
 */
static void TaskMonitor(void *pvParameters)
{
    (void)pvParameters;
    uint32_t tick = 0;

    for (;;) {
        if (CORE1_STATUS == CORE1_STATUS_DONE) {
            xil_printf("[Core0] Core1 SW sweep done. Elapsed: %lu us\r\n",
                       (unsigned long)CORE1_ELAPSED_US);
            vTaskDelete(NULL);
        }

        if ((tick % 1000) == 0)
            xil_printf("[Core0] Monitor tick %lu - Core1 still running\r\n",
                       (unsigned long)tick);
        tick++;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void)
{
    xil_printf("\r\nTiled MatMul: FreeRTOS Dual-Core - Zybo Z7-20\r\n");

    pmu_init();
    l2_init();

    run_dma_smoke_tests();

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

    /* Signal Core 1 to start its SW sweep via SEV/WFE handshake */
    CORE1_CMD    = CORE1_CMD_IDLE;
    CORE1_STATUS = CORE1_STATUS_BUSY;
    __asm__ volatile("dsb" ::: "memory");
    CORE1_CMD = CORE1_CMD_RUN;
    __asm__ volatile("sev" ::: "memory");

    xil_printf("[Core0] Core1 woken - starting FreeRTOS scheduler\r\n");

#ifdef XPAR_MATMUL_0_DEVICE_ID
    xTaskCreate(TaskHWMatmul, "HWMatmul", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
#endif
    xTaskCreate(TaskMonitor, "Monitor", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();

    while (1) {}
    return 0;
}
