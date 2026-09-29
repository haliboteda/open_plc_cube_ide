/*
 * calib_area.h -- on-flash format of the per-board AI/AO calibration.
 *
 * This file owns the format. The Arduino core
 * (libraries/OpenPLC_Ports/src/openplc_calib.h) and the fixture tool
 * (internal/calarea/calarea.go) mirror it; P2 checks the three agree.
 * Byte table and meaning of the coefficients:
 * $PROD/docs/modules/M1/SECTOR-15.md, "校准值区的格式".
 *
 * Written by the fixture over JLINK, never by the firmware. The bootloader
 * only carries it across a metadata reclaim (bootloader_state.c).
 */

#ifndef IAPSERVER_CALIB_AREA_H_
#define IAPSERVER_CALIB_AREA_H_

#include <stdint.h>

#define CALIB_AREA_ADDR   0x081E0000UL
#define CALIB_AREA_SIZE   (8U * 1024U)

#define CALIB_MAGIC       0x4C41434FUL   /* "OCAL" */
#define CALIB_VERSION     1U
#define CALIB_CHANNELS    4U

typedef enum {
	CALIB_CH_AI1 = 0,   /* mV */
	CALIB_CH_AI2,       /* mA */
	CALIB_CH_AO1,       /* mA */
	CALIB_CH_AO2        /* mA */
} calib_channel_id_t;

/* measured ~= gain * nominal + offset */
typedef struct {
	float gain;
	float offset;
} calib_channel_t;

typedef struct {
	uint32_t        magic;
	uint16_t        version;
	uint16_t        channels;
	uint32_t        uid[3];              /* w0 w1 w2 */
	calib_channel_t ch[CALIB_CHANNELS];
	uint32_t        crc32;               /* CRC-32 (IEEE) of every byte before it */
} calib_area_t;

_Static_assert(sizeof(calib_area_t) == 56U, "calib_area_t is the on-flash format");

#endif /* IAPSERVER_CALIB_AREA_H_ */
