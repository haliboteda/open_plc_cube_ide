// porttool_handover.c
//
// Handover to the standalone bring-up entries - see porttool_handover.h.

#include "porttool_handover.h"
#include "porttool.h"       /* PORTTOOL_ENABLE */

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#include "KNX/knx_test.h"
#include "CAN/can_test.h"
#include "RS485/rs485_test.h"
#include "PWM/pwm_test.h"
#include "RS232/rs232_test.h"
#include "SD/sd_test.h"
#include "SDRAM/sdram_test.h"
#include "bringup_test.h"

/* `port` groups the targets that exercise the same piece of hardware, so
 * pt.caps can report one port with several variants instead of fourteen
 * unrelated buttons. Entries sharing a port must agree on blk/term/loop. */
typedef struct {
    const char *name;
    const char *port;
    const char *board;   /* which PCB - see porttool.h PORTTOOL_BOARD_* */
    const char *blk;
    const char *term;
    porttool_loop_t loop;
    void      (*run)(void);
    const char *what;

    /* Where pt.caps mentions this entry. Hiding one from caps keeps it off the
     * panel; it stays reachable by typing pt.handover either way.
     *
     * *** As of 2026-09-13 every entry is HANDOVER_NOT_IN_CAPS. *** The panel
     * offers none of them: production drives sessions and pt.run targets, both
     * of which report numbers a PC can judge, while an entry that takes the
     * board and prints prose until somebody resets it cannot be judged at all
     * (DECISIONS.md 38, 40). These stay as bench tools for whoever is holding
     * a scope, reachable by typing the command.
     *
     * The other two values are kept because that decision is about what
     * production needs, not about what the mechanism can do - putting an entry
     * back on a caps row is a one-word change if a reason ever appears. */
    enum {
        /* Its own "OK port=... kind=handover" row. The default. */
        HANDOVER_OWN_ROW = 0,

        /* On the row the port already has - a session for can/knx/rs485, a
         * kind=run row for sd/sdram. Two caps rows with the same port= would
         * give the panel two entries for one piece of hardware and no way to
         * tell them apart. */
        HANDOVER_ON_PORT_ROW,

        /* Nowhere. For an entry that must not become a button. */
        HANDOVER_NOT_IN_CAPS,
    } caps_place;
} handover_t;

static const handover_t targets[] = {
    { "bringup",     "bringup", PORTTOOL_BOARD_WHOLE, "-", "-",       PORTTOOL_LOOP_NONE, BringUp_Test_Run,                "DIN, relays, analog in/out and temperature together, with a key menu" , HANDOVER_NOT_IN_CAPS },
    { "can",         "can", PORTTOOL_BOARD_UPPER, "C", "C07,C08", PORTTOOL_LOOP_LINK, CAN_Test_Run,                    "five phases: report, internal loopback, external loopback, listen, normal" , HANDOVER_NOT_IN_CAPS },
    { "can.soak",    "can", PORTTOOL_BOARD_UPPER, "C", "C07,C08", PORTTOOL_LOOP_LINK, CAN_Test_Soak_Run,               "one normal-mode session that runs until reset" , HANDOVER_NOT_IN_CAPS },
    { "can.scope",   "can", PORTTOOL_BOARD_UPPER, "C", "C07,C08", PORTTOOL_LOOP_LINK, CAN_Test_Scope_Run,              "square wave then back-to-back frames, for a scope on PB9" , HANDOVER_NOT_IN_CAPS },
    { "can.echo",    "can", PORTTOOL_BOARD_UPPER, "C", "C07,C08", PORTTOOL_LOOP_LINK, CAN_Test_Echo_Run,               "replies to every frame with its payload incremented" , HANDOVER_NOT_IN_CAPS },
    { "knx",         "knx", PORTTOOL_BOARD_UPPER, "C", "C03,C04", PORTTOOL_LOOP_LINK, KNX_Test_Run,                    "TP1 bit timing, raw and bit-inverted decode of every burst" , HANDOVER_NOT_IN_CAPS },
    /* Offered beside their own session rather than as ports of their own: one
     * piece of hardware, one card on the panel (DECISIONS.md 17). CAN got a
     * session on 2026-09-08, so its four deep entries moved here too. */
    { "rs485",       "rs485", PORTTOOL_BOARD_UPPER, "C", "C10,C11", PORTTOOL_LOOP_LINK, RS485_Test_Run,                  "pin-level check, periodic banner, echo of whatever arrives", HANDOVER_NOT_IN_CAPS },
    /* Nowhere in caps. Entering it takes the command loop away, so the only
     * way back is the reset button - a button for that on the panel would be a
     * button that kills the panel. Typing pt.handover rs232 still works, which
     * is a deliberate act rather than a click. */
    { "rs232",       "rs232", PORTTOOL_BOARD_UPPER, "C", "C05,C06", PORTTOOL_LOOP_LINK, RS232_Test_Run,                  "echo every byte back to the terminal", HANDOVER_NOT_IN_CAPS },
    { "pwm",         "pwm", PORTTOOL_BOARD_LOWER, "A", "A08",     PORTTOOL_LOOP_CTRL, PWM_Test_Run,                    "breathing LED on Digital Out 6" , HANDOVER_NOT_IN_CAPS },
    { "sd.info",     "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6",      PORTTOOL_LOOP_NONE, SD_Test_Info,                    "card type, capacity, and live detect-pin state" , HANDOVER_NOT_IN_CAPS },
    { "sd.integrity.soak", "sd", PORTTOOL_BOARD_BRIDGE, "-", "J6",     PORTTOOL_LOOP_NONE, SD_Test_FileIntegrity,           "write 4 KiB through FatFs, read back, compare and CRC, forever" , HANDOVER_NOT_IN_CAPS },
    { "sdram.capacity",  "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6",    PORTTOOL_LOOP_NONE, SDRAM_Test_Capacity,             "capacity and address wrap-around" , HANDOVER_NOT_IN_CAPS },
    { "sdram.retention.soak", "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6", PORTTOOL_LOOP_NONE, SDRAM_Test_Retention,          "write/wait/read-back cycles over a long run" , HANDOVER_NOT_IN_CAPS },
    { "sdram.crc.soak",       "sdram", PORTTOOL_BOARD_BRIDGE, "-", "U6",    PORTTOOL_LOOP_NONE, SDRAM_Test_CubeProgrammerVerify, "CRC32 against what CubeProgrammer wrote, reprinted every second forever" , HANDOVER_NOT_IN_CAPS },
};
#define TARGET_COUNT (sizeof(targets) / sizeof(targets[0]))

/* True when entry `i` is the first of its port group, so each group is
 * reported once no matter how the table is ordered. */
static int first_of_port(uint32_t i)
{
    for (uint32_t j = 0; j < i; j++) {
        if (strcmp(targets[j].port, targets[i].port) == 0) {
            return 0;
        }
    }
    return 1;
}

uint32_t PortTool_HandoverTargetsFor(const char *port, char *out, uint32_t out_len)
{
    uint32_t n = 0, count = 0;

    if (out_len == 0U) {
        return 0;
    }
    out[0] = '\0';

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (targets[i].caps_place != HANDOVER_ON_PORT_ROW ||
            strcmp(targets[i].port, port) != 0) {
            continue;
        }
        int w = snprintf(out + n, out_len - n, "%s%s",
                         (count > 0U) ? "," : "", targets[i].name);
        if (w < 0 || (uint32_t)w >= out_len - n) {
            break;
        }
        n += (uint32_t)w;
        count++;
    }
    return count;
}

uint32_t PortTool_HandoverCaps(int emit)
{
    char list[96];
    uint32_t lines = 0;

    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (targets[i].caps_place != HANDOVER_OWN_ROW || !first_of_port(i)) {
            continue;
        }
        lines++;
        if (!emit) {
            continue;
        }

        uint32_t n = 0;
        list[0] = '\0';
        for (uint32_t j = 0; j < TARGET_COUNT && n < sizeof(list); j++) {
            if (strcmp(targets[j].port, targets[i].port) != 0) {
                continue;
            }
            int w = snprintf(list + n, sizeof(list) - n, "%s%s",
                             (n > 0U) ? "," : "", targets[j].name);
            if (w < 0) { break; }
            n += (uint32_t)w;
        }

        printf("OK port=%s board=%s kind=handover blk=%s term=%s channels=1 "
               "loop=%s targets=%s\r\n",
               targets[i].port, targets[i].board, targets[i].blk, targets[i].term,
               PortTool_LoopName(targets[i].loop), list);
    }

    return lines;
}

void PortTool_HandoverList(void)
{
    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        printf("OK handover=%-16s %s\r\n", targets[i].name, targets[i].what);
    }
}

int PortTool_Handover(const char *name)
{
    for (uint32_t i = 0; i < TARGET_COUNT; i++) {
        if (strcmp(targets[i].name, name) != 0) {
            continue;
        }

        printf("OK handing over to %s - this does not come back, reset the board "
               "to return to the port tool\r\n", targets[i].name);

        targets[i].run();

        /* Most of these never return. PWM_Test_Run does when its timer refuses
         * to start, and saying so beats leaving a dead prompt. */
        printf("OK %s returned - back at the port tool\r\n", targets[i].name);
        return 1;
    }
    return 0;
}

#endif /* PORTTOOL_ENABLE */
