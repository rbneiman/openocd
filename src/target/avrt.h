/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   Copyright (C) 2009 by Simon Qian                                      *
 *   SimonQian@SimonQian.com                                               *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_AVRT_H
#define OPENOCD_TARGET_AVRT_H

#include <jtag/jtag.h>
#include "register.h"

/* AVR_JTAG_Instructions */
#define AVR_JTAG_INS_LEN                                        4
/* Public Instructions: */
#define AVR_JTAG_INS_EXTEST                                     0x00
#define AVR_JTAG_INS_IDCODE                                     0x01
#define AVR_JTAG_INS_SAMPLE_PRELOAD                             0x02
#define AVR_JTAG_INS_BYPASS                                     0x0F
/* AVR Specified Public Instructions: */
#define AVR_JTAG_INS_AVR_RESET                                  0x0C
#define AVR_JTAG_INS_PROG_ENABLE                                0x04
#define AVR_JTAG_INS_PROG_COMMANDS                              0x05
#define AVR_JTAG_INS_PROG_PAGELOAD                              0x06
#define AVR_JTAG_INS_PROG_PAGEREAD                              0x07
/* AVR Private Instructions */
#define AVR_JTAG_INS_FORCE_BREAK                                0x08
#define AVR_JTAG_INS_RUN                                        0x09
#define AVR_JTAG_INS_EXEC_INS                                   0x0A
#define AVR_JTAG_INS_ACCESS_OCD_REG                             0x0B

/* Data Registers: */
#define AVR_JTAG_REG_BYPASS_LEN                                 1
#define AVR_JTAG_REG_DEVICEID_LEN                               32

#define AVR_JTAG_REG_RESET_LEN                                  1
#define AVR_JTAG_REG_JTAGID_LEN                                 32
#define AVR_JTAG_REG_PROGRAMMING_ENABLE_LEN                     16
#define AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN                    15
#define AVR_JTAG_REG_FLASH_DATA_BYTE_LEN                        16

/* AVR OCD Registers */
#define AVR_OCDREG_PSB0                                         0
#define AVR_OCDREG_PSB1                                         1
#define AVR_OCDREG_PDMSB                                        2
#define AVR_OCDREG_PDSB                                         3
#define AVR_OCDREG_BRK_CTL                                      8 
#define AVR_OCDREG_BRK_STATUS                                   9
#define AVR_OCDREG_RD_BACK                                      12
#define AVR_OCDREG_OCD_CTL_STATUS                               13

// 32 general
// + status + PC + SP 
#define AVR_NUM_GP_REGS 32
#define	AVR_NUM_REGS	36
#define AVR_REG_GEN_BITS 8
#define AVR_REG_GEN_BYTES 1
#define AVR_REG_STATUS 33
#define AVR_REG_STATUS_BITS 8
#define AVR_REG_STATUS_BYTES 1
#define AVR_REG_SP 34
#define AVR_REG_SP_BITS 16
#define AVR_REG_SP_BYTES 2
#define AVR_REG_PC 35
#define AVR_REG_PC_BITS 16
#define AVR_REG_PC_BYTES 2

struct mcu_jtag {
	struct jtag_tap *tap;
};

struct avr_core_reg_cache{
	struct reg *reg_list[AVR_NUM_REGS];
	struct reg gp_regs[AVR_NUM_GP_REGS];
	uint8_t gp_reg_vals[AVR_NUM_GP_REGS * AVR_REG_GEN_BYTES];
	struct reg pc;
	struct reg sp;
	struct reg status;
};

struct avr_common {
	struct mcu_jtag jtag_info;

	struct avr_core_reg_cache reg_cache;
};

int mcu_execute_queue(void);
int avr_jtag_sendinstr(struct jtag_tap *tap, uint8_t *ir_in, uint8_t ir_out);
int avr_jtag_senddat(struct jtag_tap *tap, uint32_t *dr_in, uint32_t dr_out,
		int len);

#endif /* OPENOCD_TARGET_AVRT_H */
