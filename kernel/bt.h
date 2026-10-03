/* nat-os — Bluetooth, stage 1: bring the controller up and talk HCI to it.
 *
 * ---- what this is and is not -----------------------------------------------
 *
 * The ESP32's Bluetooth controller is a binary blob (libbtdm_app.a and
 * friends). Above a controller, a normal system runs a HOST stack -- Bluedroid
 * or NimBLE, tens of thousands of lines -- which is where GATT, pairing and
 * profiles live. nat-os runs neither.
 *
 * It does not have to. The controller exposes a VIRTUAL HCI interface: the same
 * command and event stream a host would push over a UART, as function calls.
 * Writing HCI directly is a few hundred lines and reaches everything a scanner
 * or an advertiser needs:
 *
 *     HCI_Reset                        0x0C03
 *     LE_Set_Advertising_Data          0x2008   be discoverable
 *     LE_Set_Advertise_Enable          0x200A
 *     LE_Set_Scan_Parameters           0x200B   see what is nearby
 *     LE_Set_Scan_Enable               0x200C
 *     LE Advertising Report (event)    0x3E/0x02
 *
 * What that leaves out is connections, pairing and GATT. Those need a host
 * stack, and this is honest about not being one.
 *
 * ---- the measurements that said it would fit -------------------------------
 *
 *     controller iram      39,686 B     free in a blob-free build   61,119 B
 *     controller dram      16,921 B     free                        37,994 B
 *     heap at runtime      unknown      free                        22,392 B
 *
 * The first two are link-time facts. The third is what stage 1 finds out, which
 * is why bt_report() prints the controller's live heap use: if Bluetooth does
 * not fit on this board, that number is how it says so.
 *
 * [next_moves/13 step 1]
 */

#ifndef NATOS_BT_H
#define NATOS_BT_H

#include <stdint.h>

typedef enum {
    BT_IDLE = 0,        /* nothing attempted                                */
    BT_PHY_FAILED,      /* the radio would not calibrate                    */
    BT_INIT_FAILED,     /* esp_bt_controller_init refused                   */
    BT_ENABLE_FAILED,   /* init took, enable did not                        */
    BT_NO_VHCI,         /* enabled, but the HCI interface never came ready  */
    BT_NO_REPLY,        /* HCI_Reset sent, no Command Complete came back    */
    BT_READY,           /* the controller answered its own reset            */
} bt_state_t;

/* Runs the whole bring-up: PHY, controller init, enable, then one HCI_Reset.
 * Returns the state it reached. Safe to call twice; the second call reports
 * rather than repeats. */
bt_state_t bt_init(void);

bt_state_t bt_state(void);
const char *bt_state_text(bt_state_t s);

/* Everything the bring-up learned, for the shell: which stage was reached, the
 * controller's own status, how much heap it took, how many of each host service
 * it called, and the bytes of the last HCI event. */
void bt_report(void);

/* The last HCI event received, for a caller that wants to parse it. Returns the
 * length, 0 if none; `out` must have room for 64 bytes. */
uint32_t bt_last_event(uint8_t *out);

#endif /* NATOS_BT_H */
