/* nat-os — Bluetooth stage 1: the controller, and one HCI command.
 *
 * See bt.h for why there is no host stack here. This file is the call0 driver:
 * it brings the radio up, hands the controller its configuration, registers for
 * HCI events, and sends HCI_Reset. If the controller answers its own reset,
 * everything underneath -- the blob, the OSI shims, the ABI bridge, the PHY,
 * the interrupt routing -- is working, and that is the whole point of stage 1.
 *
 * [next_moves/13 step 1]
 */

#include <stdint.h>
#include "bt.h"
#include "uart.h"
#include "task.h"
#include "window.h"
#include "heap.h"
#include "watchdog.h"

/* ---- the controller's configuration ----------------------------------------
 *
 * Transcribed from esp_bt.h's esp_bt_controller_config_t, field for field and
 * in order. The ORDER is load-bearing and the blob checks only the last field:
 * `magic`. A member out of place puts a plausible value where another was
 * expected and the magic still lands correctly, so nothing diagnoses it -- the
 * same hazard the WiFi OSI table taught this project (UM-NATOS-038 §5.6),
 * where 63 positions differed and the size gave nothing away.
 *
 * Two values are deliberately NOT the SDK defaults:
 *
 *   ble_max_conn   1, not 3. Each connection's state is heap, and this board
 *                  has 22 KB of it. A scanner and an advertiser need none.
 *   mesh_adv_size  0, not 100. The mesh duplicate-scan cache is heap too, and
 *                  scan_duplicate_mode says normal advertisements only.
 */
typedef struct {
    uint16_t controller_task_stack_size;
    uint8_t  controller_task_prio;
    uint8_t  hci_uart_no;
    uint32_t hci_uart_baudrate;
    uint8_t  scan_duplicate_mode;
    uint8_t  scan_duplicate_type;
    uint16_t normal_adv_size;
    uint16_t mesh_adv_size;
    uint16_t send_adv_reserved_size;
    uint32_t controller_debug_flag;
    uint8_t  mode;
    uint8_t  ble_max_conn;
    uint8_t  bt_max_acl_conn;
    uint8_t  bt_sco_datapath;
    uint8_t  auto_latency;              /* bool in the header */
    uint8_t  bt_legacy_auth_vs_evt;
    uint8_t  bt_max_sync_conn;
    uint8_t  ble_sca;
    uint8_t  pcm_role;
    uint8_t  pcm_polar;
    uint8_t  hli;
    uint16_t dup_list_refresh_period;
    uint32_t magic;
} bt_cfg_t;

#define BT_CFG_MAGIC   0x20221207u      /* ESP_BT_CONTROLLER_CONFIG_MAGIC_VAL */
#define BT_MODE_BLE    0x01u

static bt_cfg_t g_cfg = {
    .controller_task_stack_size = 4096u,
    .controller_task_prio       = 23u,
    .hci_uart_no                = 1u,       /* unused: this is VHCI, not UART */
    .hci_uart_baudrate          = 921600u,
    .scan_duplicate_mode        = 0u,       /* normal advertisements only */
    .scan_duplicate_type        = 0u,
    .normal_adv_size            = 20u,
    .mesh_adv_size              = 0u,
    .send_adv_reserved_size     = 1000u,
    .controller_debug_flag      = 0u,
    .mode                       = BT_MODE_BLE,
    .ble_max_conn               = 1u,
    .bt_max_acl_conn            = 0u,
    .bt_sco_datapath            = 0u,
    .auto_latency               = 0u,
    .bt_legacy_auth_vs_evt      = 0u,
    .bt_max_sync_conn           = 0u,
    .ble_sca                    = 1u,
    .pcm_role                   = 0u,
    .pcm_polar                  = 0u,
    .hli                        = 0u,
    .dup_list_refresh_period    = 0u,
    .magic                      = BT_CFG_MAGIC,
};

/* ---- the blob's entry points, and the windowed helpers beside them ---------
 *
 * Every one of these is WINDOWED. None may be called directly from this file;
 * phy_stack_call() runs them on the 6 KB private stack the PHY bring-up already
 * needed, because the controller nests deeper than a 2 KB task stack allows and
 * the symptom of getting that wrong is a fault inside a window spill rather
 * than anything that looks like a stack problem (window.h). */
extern int esp_bt_controller_init(bt_cfg_t *cfg);
extern int esp_bt_controller_enable(uint32_t mode);
extern int esp_bt_controller_get_status(void);

extern uint32_t bt_vhci_register(void);
extern uint32_t bt_vhci_can_send(void);
extern uint32_t bt_vhci_send(uint32_t data, uint32_t len);

/* The windowed side's buffers, read as data. */
extern uint8_t  bt_vhci_evt[64];
extern uint32_t bt_vhci_evt_len;
extern uint32_t bt_vhci_events;
extern uint32_t bt_vhci_ready;
extern uint32_t bt_vhci_dropped;
extern uint32_t bt_shim_stub_calls[8];
extern uint32_t bt_queue_bytes;
extern uint32_t bt_queues_made;
extern uint32_t bt_queues_failed;

extern int bt_host_phy_enable(void);
extern uint32_t bt_host_intr_allocs(void);
extern uint32_t bt_host_xt_handlers(void);
extern uint32_t bt_host_task_reached(void);
extern uint32_t bt_host_task_running(void);
extern uint32_t bt_host_task_returned(void);
extern uint32_t bt_host_mallocs(void);
extern uint32_t bt_host_frees(void);
extern uint32_t bt_host_bytes_live(void);
extern uint32_t bt_host_biggest_ask(void);
extern uint32_t bt_host_refused(void);
extern uint32_t bt_host_logs(void);
extern uint32_t bt_host_crit_overflows(void);
extern void bt_host_phy_report(void);

static bt_state_t g_state;
static int g_init_rc, g_enable_rc, g_status;
static uint32_t g_heap_before, g_heap_after;

bt_state_t bt_state(void) { return g_state; }

const char *bt_state_text(bt_state_t s)
{
    switch (s) {
    case BT_IDLE:          return "not attempted";
    case BT_PHY_FAILED:    return "the radio would not calibrate";
    case BT_INIT_FAILED:   return "the controller refused its configuration";
    case BT_ENABLE_FAILED: return "initialised but would not enable";
    case BT_NO_VHCI:       return "enabled, but HCI never became ready";
    case BT_NO_REPLY:      return "HCI_Reset sent, nothing came back";
    case BT_READY:         return "the controller answered its own reset";
    default:               return "?";
    }
}

uint32_t bt_last_event(uint8_t *out)
{
    uint32_t n = bt_vhci_evt_len;
    if (!out || n == 0u) {
        return 0;
    }
    if (n > 64u) {
        n = 64u;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = bt_vhci_evt[i];
    }
    return n;
}

/* ---- HCI ------------------------------------------------------------------
 *
 * One command: HCI_Reset, opcode 0x0C03, no parameters. The packet is
 *
 *     01        H4 type: command
 *     03 0C     opcode, little-endian
 *     00        parameter length
 *
 * and the answer is a Command Complete event (0x04 0x0E ...) carrying the same
 * opcode and a status byte. Nothing else in Bluetooth is simpler, which is why
 * it is the one stage 1 uses: anything that comes back proves the whole stack
 * underneath. */
static const uint8_t HCI_RESET[4] = { 0x01u, 0x03u, 0x0Cu, 0x00u };

static int hci_send(const uint8_t *pkt, uint32_t len)
{
    /* The controller says when it can take a packet. Bounded: a controller that
     * never becomes ready is a failure to report, not a reason to spin. */
    /* rom_call4, not phy_stack_call: this runs on the bring-up task, which owns
     * its stack, and the loop below SLEEPS between attempts. Sleeping with an
     * sp on the shared _phy_stack is the defect this whole file was rewritten
     * to avoid -- see bt_boot_task. */
    for (uint32_t i = 0; i < 200u; i++) {
        if (rom_call4((uint32_t)&bt_vhci_can_send, 0, 0, 0, 0)) {
            rom_call4((uint32_t)&bt_vhci_send, (uint32_t)pkt, len, 0, 0);
            return 1;
        }
        task_sleep(1u);
    }
    return 0;
}

/* ---- why the bring-up runs on its own task ---------------------------------
 *
 * The first version of this called esp_bt_controller_init through
 * phy_stack_call, the same way the PHY is called, and the board froze solid
 * every time -- no ticks, no task switches, no output, until the hang detector
 * reset it thirty seconds later. The shim trace (-BTTrace) ended on:
 *
 *     . xTaskCreatePinnedToCore      the controller creates its own task
 *     . esp_coex_version_get
 *     . xQueueGenericSend            it posts work to that task
 *     . xQueueSemaphoreTake          and waits for the answer
 *
 * which is not a deadlock in the semaphore. It is esp_bt_controller_init
 * BLOCKING, which is a perfectly ordinary thing for it to do and which
 * phy_stack_call cannot survive:
 *
 *   - the wait reaches wait_on() in wifi_osi_impl.c, which calls task_sleep();
 *   - task_sleep() switches away from a task whose stack pointer is on the
 *     shared 6 KB _phy_stack;
 *   - phyinit.c PINS the scheduler across its own phy_stack_call for exactly
 *     that reason, and says so: "nothing is ever saved with an sp on
 *     _phy_stack". The pin is not an optimisation, it is the precondition.
 *
 * So a blocking call and phy_stack_call are mutually exclusive, and the PHY
 * only gets away with it because register_chipv7_phy never blocks.
 *
 * The answer is a task that OWNS its stack. Blocking is then legal: the
 * scheduler saves an sp that belongs to the task it belongs to, every other
 * task keeps running, and the hang detector keeps being fed by real switches
 * instead of needing its window widened. rom_call4() enters the windowed blob
 * on the current stack with a correct base frame -- the same bridge
 * bt_task_tramp uses for the controller's own task, and for the same reason.
 *
 * 6 KB, from measurement rather than taste: the PHY's deepest nest used 1,296
 * bytes of its 6,144 (phy_stack_used, printed by `bt`), and the controller
 * nests further. The high-water mark is reported so this number can stop being
 * a guess. */
#define BT_BOOT_WORDS 1536u             /* 6,144 bytes */

static uint32_t g_boot_stack[BT_BOOT_WORDS];
static volatile uint32_t g_boot_done;

/* The task id, kept so the report can ask the kernel how deep it actually went.
 *
 * task_create_with_stack() lays down its own fill pattern and
 * task_stack_headroom() measures against it, so there is no second instrument
 * here. An earlier draft filled this buffer itself and then handed it to the
 * kernel, which re-filled it -- the "used" figure would have been the whole
 * stack every time, and it would have looked like a real measurement. */
static int g_boot_task = -1;

static void bt_boot_task(void)
{
    uart_puts("   handing the controller its configuration\n");
    g_init_rc = (int)rom_call4((uint32_t)&esp_bt_controller_init,
                               (uint32_t)&g_cfg, 0, 0, 0);
    if (g_init_rc != 0) {
        g_state = BT_INIT_FAILED;
        g_boot_done = 1u;
        return;
    }

    uart_puts("   enabling BLE\n");
    g_enable_rc = (int)rom_call4((uint32_t)&esp_bt_controller_enable,
                                 BT_MODE_BLE, 0, 0, 0);
    if (g_enable_rc != 0) {
        g_state = BT_ENABLE_FAILED;
        g_boot_done = 1u;
        return;
    }
    g_status = (int)rom_call4((uint32_t)&esp_bt_controller_get_status,
                              0, 0, 0, 0);

    rom_call4((uint32_t)&bt_vhci_register, 0, 0, 0, 0);
    g_heap_after = heap_largest_free();

    uart_puts("   sending HCI_Reset\n");
    if (!hci_send(HCI_RESET, sizeof HCI_RESET)) {
        g_state = BT_NO_VHCI;
        g_boot_done = 1u;
        return;
    }

    /* The answer arrives on the controller's own task, through the VHCI
     * callback. Bounded, and generous: 500 ms is far beyond the microseconds a
     * reset takes and far short of a person's patience. */
    for (uint32_t i = 0; i < 50u && bt_vhci_events == 0u; i++) {
        task_sleep(1u);
    }
    g_state = (bt_vhci_events != 0u) ? BT_READY : BT_NO_REPLY;
    g_boot_done = 1u;
}

bt_state_t bt_init(void)
{
    if (g_state != BT_IDLE) {
        return g_state;         /* report rather than repeat */
    }

    g_heap_before = heap_largest_free();

    /* The PHY first, and this one DOES go through phy_stack_call: it needs the
     * deep stack and it never blocks, which is the combination that mechanism
     * is for. */
    uart_puts("   bringing the radio up\n");
    if (bt_host_phy_enable() != 0) {
        return g_state = BT_PHY_FAILED;
    }

    g_boot_task = task_create_with_stack("btboot", bt_boot_task,
                                         g_boot_stack, BT_BOOT_WORDS);
    if (g_boot_task < 0) {
        uart_puts("   no task slot for the bring-up\n");
        return g_state = BT_INIT_FAILED;
    }

    /* Wait for it, on this task's own stack, where sleeping is safe.
     *
     * The hang detector is left alone deliberately. Both tasks now block on
     * real sleeps, so the scheduler switches between distinct tasks the whole
     * way through and feeds it normally -- the 30-second window the previous
     * version needed was a symptom of running the blob where it could not
     * block, not a requirement of Bluetooth. */
    for (uint32_t i = 0; i < 1500u && !g_boot_done; i++) {
        task_sleep(1u);
    }
    if (!g_boot_done) {
        uart_puts("   bring-up did not finish in 15 s\n");
    }
    return g_state;
}

void bt_report(void)
{
    uart_puts("   state: ");
    uart_puts(bt_state_text(g_state));
    uart_puts("\n");

    bt_host_phy_report();

    uart_puts("   controller: init rc=");
    uart_put_dec((uint32_t)g_init_rc);
    uart_puts(" enable rc=");
    uart_put_dec((uint32_t)g_enable_rc);
    uart_puts(" status=");
    uart_put_dec((uint32_t)g_status);
    uart_puts("  (0 = ok, 2 = enabled)\n");

    /* What the controller took, which is the number that decides whether
     * Bluetooth fits on this board at all. */
    uart_puts("   heap: largest free ");
    uart_put_dec(g_heap_before);
    uart_puts(" -> ");
    uart_put_dec(g_heap_after);
    uart_puts(" B, controller holds ");
    uart_put_dec(bt_host_bytes_live());
    uart_puts(" B in ");
    uart_put_dec(bt_host_mallocs() - bt_host_frees());
    uart_puts(" blocks; biggest single ask ");
    uart_put_dec(bt_host_biggest_ask());
    uart_puts(" B, refused ");
    uart_put_dec(bt_host_refused());
    uart_puts("\n");

    uart_puts("   queues: ");
    uart_put_dec(bt_queues_made);
    uart_puts(" made, ");
    uart_put_dec(bt_queue_bytes);
    uart_puts(" B asked for, ");
    uart_put_dec(bt_queues_failed);
    uart_puts(" refused\n");

    uart_puts("   bring-up stack: ");
    if (g_boot_task < 0) {
        uart_puts("never created");
    } else {
        uart_put_dec(task_stack_headroom(g_boot_task) * 4u);
        uart_puts(" of ");
        uart_put_dec(BT_BOOT_WORDS * 4u);
        uart_puts(" B still untouched");
    }
    uart_puts("\n");

    uart_puts("   controller task: reached ");
    uart_put_dec(bt_host_task_reached());
    uart_puts(", ran ");
    uart_put_dec(bt_host_task_running());
    uart_puts(", returned ");
    uart_put_dec(bt_host_task_returned());
    uart_puts("\n");

    uart_puts("   host calls: interrupts ");
    uart_put_dec(bt_host_intr_allocs());
    uart_puts(" + ");
    uart_put_dec(bt_host_xt_handlers());
    uart_puts(" by line");
    uart_puts("  logs ");
    uart_put_dec(bt_host_logs());
    uart_puts("  nested criticals ");
    uart_put_dec(bt_host_crit_overflows());
    uart_puts("\n   stubs hit: nvs ");
    uart_put_dec(bt_shim_stub_calls[0]);
    uart_puts(" aes ");
    uart_put_dec(bt_shim_stub_calls[1]);
    uart_puts(" vfs ");
    uart_put_dec(bt_shim_stub_calls[2]);
    uart_puts(" ipc ");
    uart_put_dec(bt_shim_stub_calls[3]);
    uart_puts(" mesh ");
    uart_put_dec(bt_shim_stub_calls[4]);
    uart_puts(" coex ");
    uart_put_dec(bt_shim_stub_calls[5]);
    uart_puts(" misc ");
    uart_put_dec(bt_shim_stub_calls[7]);
    uart_puts("\n");

    uart_puts("   vhci: events ");
    uart_put_dec(bt_vhci_events);
    uart_puts("  send-ready ");
    uart_put_dec(bt_vhci_ready);
    uart_puts("  dropped ");
    uart_put_dec(bt_vhci_dropped);
    uart_puts("\n");

    if (bt_vhci_evt_len) {
        uart_puts("   last event:");
        for (uint32_t i = 0; i < bt_vhci_evt_len && i < 16u; i++) {
            uart_puts(" ");
            uart_put_hex(bt_vhci_evt[i]);
        }
        /* A Command Complete for HCI_Reset is 04 0E 04 01 03 0C 00: event,
         * complete, 4 bytes, one command allowed, opcode 0x0C03, status 0. */
        if (bt_vhci_evt_len >= 7u && bt_vhci_evt[0] == 0x04u
            && bt_vhci_evt[1] == 0x0Eu && bt_vhci_evt[4] == 0x03u
            && bt_vhci_evt[5] == 0x0Cu) {
            uart_puts(bt_vhci_evt[6] == 0u
                      ? "\n   that is Command Complete for HCI_Reset, status ok\n"
                      : "\n   Command Complete for HCI_Reset, NON-ZERO status\n");
        } else {
            uart_puts("\n");
        }
    }
}
