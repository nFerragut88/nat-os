/* nat-os -- lwIP configuration. next_moves/08 step 232.
 *
 * NO_SYS = 1: bare-metal lwIP. No threads, no sys_arch.c, no mutexes,
 * mailboxes or semaphores, and no sockets or netconn API. The port layer
 * reduces to two things -- sys_now() and a netif driver -- and that is the
 * whole reason this mode was chosen.
 *
 * NO_SYS = 0 would buy the BSD sockets API, at the cost of implementing lwIP's
 * threading primitives on top of nat-os's scheduler. That is a large surface
 * and it proves nothing that the raw API does not; it can come later if
 * anything needs it.
 *
 * What this configuration still provides is the point: IPv4, ARP, ICMP, UDP,
 * TCP and a real DHCP client -- the last of which replaces the hand-rolled
 * DISCOVER/REQUEST in kernel/net.c, whose ring-size bug at step 231 is a fair
 * argument for using a stack somebody else has already debugged.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0   /* single-threaded: no protection needed */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

/* ---- memory ------------------------------------------------------------
 *
 * MEM_LIBC_MALLOC = 0 so lwIP uses its OWN static heap rather than reaching
 * for a malloc this kernel spells differently (heap_alloc). One fewer seam,
 * and the footprint becomes a compile-time constant instead of a runtime
 * question. MEM_SIZE is the bulk of it.
 *
 * ---- and these numbers are MEASURED, not chosen ------------------------
 *
 * The first attempt used lwIP's comfortable defaults -- MEM_SIZE 16 KB and a
 * twelve-buffer pbuf pool -- and the board crashed inside
 * esp_wifi_init_internal with epc 0. Not a code fault: those two put 35 KB
 * into .bss, which pushed _bss_end up and left
 *
 *     _heap_start 0x3ffcc808  _heap_end 0x3ffd3000  =  26.6 KB
 *
 * while the WiFi driver's own measured peak is 35 KB high-water. lwIP had
 * taken the memory the driver needs, and an unchecked allocation failure
 * became a jump to NULL.
 *
 * The heap ceiling is not negotiable: 0x3ffd4000 is where the blob's .data
 * begins, and growing into that would corrupt the driver rather than starve
 * it. So lwIP shrinks instead. These are the numbers, and the reason each is
 * what it is. */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (6 * 1024)   /* was 16 K */

#define MEMP_NUM_PBUF               16
#define MEMP_NUM_UDP_PCB            4
#define MEMP_NUM_TCP_PCB            5
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            8
#define MEMP_NUM_REASSDATA          4
#define MEMP_NUM_ARP_QUEUE          4
#define PBUF_POOL_SIZE              6            /* was 12; 6 x 1536 = 9 KB */
#define PBUF_POOL_BUFSIZE           1536   /* one Ethernet frame, aligned up */

/* ---- protocols --------------------------------------------------------- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0   /* needs str* this kernel does not have */
#define LWIP_NETIF_HOSTNAME         0
#define LWIP_NETIF_API              0
#define LWIP_STATS                  0   /* saves both code and .bss */

/* DHCP needs UDP, obviously, and lwIP checks this itself -- stated here so the
 * dependency is visible rather than implied. */
#if LWIP_DHCP && !LWIP_UDP
#error "LWIP_DHCP requires LWIP_UDP"
#endif

/* ---- TCP sizing --------------------------------------------------------
 * TCP_MSS 1460 is the usual Ethernet figure (1500 MTU - 20 IP - 20 TCP). The
 * send buffer and window are deliberately modest: this is a device with tens
 * of kilobytes of RAM, not a server. */
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            8

/* ---- checksums ---------------------------------------------------------
 * All in software. The ESP32 MAC can offload some of this; doing that before
 * the plain version is known to work would be optimising an unproven path. */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* ---- debug -------------------------------------------------------------
 * Off: LWIP_DEBUG would pull in printf, which this kernel does not have. The
 * netif callbacks in kernel/netif_wifi.c report through uart_puts instead. */
#define LWIP_DEBUG                  0

/* ---- Espressif's patches ------------------------------------------------
 *
 * This lwIP is ESP-IDF's copy, not upstream: it carries Espressif additions
 * that their own lwipopts.h defines and that upstream lwIP has never heard of.
 * A stock lwipopts.h therefore does not compile -- opt.h references
 * ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND and friends as if they were always
 * defined.
 *
 * Every one is turned OFF. They are optimisations (on-demand timers rather
 * than periodic ones, a DNS variant, ND6 queueing for IPv6) and switching them
 * on would mean depending on behaviour that differs from the lwIP everyone
 * else documents. Off is the version the upstream documentation describes.
 *
 * Vendoring IDF's copy rather than upstream is deliberate: it is the tree that
 * is known to work against this exact WiFi driver. */
#define ESP_LWIP                                0
#define ESP_LWIP_ARP                            0
#define ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND      0
#define ESP_LWIP_DHCP_FINE_TIMER_START_ONCE     0
#define ESP_LWIP_DHCP_FINE_CLOSE                0
#define ESP_LWIP_IGMP_TIMERS_ONDEMAND           0
#define ESP_LWIP_DNS_TIMERS_ONDEMAND            0
#define ESP_LWIP_IP4_REASSEMBLY_TIMERS_ONDEMAND 0
#define ESP_LWIP_IP6_REASSEMBLY_TIMERS_ONDEMAND 0
#define ESP_LWIP_MLD6_TIMERS_ONDEMAND           0
#define ESP_ND6_QUEUEING                        0
#define ESP_DNS                                 0

#endif /* LWIPOPTS_H */
