// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2009 by Simon Qian                                      *
 *   SimonQian@SimonQian.com                                               *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "avrt.h"
#include "target.h"
#include "target_type.h"
#include "register.h"

#define AVR_JTAG_INS_LEN	4

static const char * const avr_core_reg_names[] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8",
	"r9", "r10", "r11", "r12", "r13", "r14", "r15", 
	"r16", "r17", "r18", "r19", "r20","r21", "r22", "r23", 
	"r24", "r25", "r26", "r27", "r28","r29", "r30", "r31",
	"sr", "pc", "sp"
};

/* forward declarations */
static int avr_target_create(struct target *target, Jim_Interp *interp);
static int avr_init_target(struct command_context *cmd_ctx, struct target *target);

static int avr_arch_state(struct target *target);
static int avr_poll(struct target *target);
static int avr_halt(struct target *target);
static int avr_resume(struct target *target, int current, target_addr_t address,
		int handle_breakpoints, int debug_execution);
static int avr_step(struct target *target, int current, target_addr_t address,
		int handle_breakpoints);

static int avr_assert_reset(struct target *target);
static int avr_deassert_reset(struct target *target);

/* IR and DR functions */
static int mcu_write_ir(struct jtag_tap *tap, uint8_t *ir_in, uint8_t *ir_out, int ir_len);
static int mcu_write_dr(struct jtag_tap *tap, uint8_t *dr_in, uint8_t *dr_out, int dr_len);
static int mcu_write_ir_u8(struct jtag_tap *tap, uint8_t *ir_in, uint8_t ir_out, int ir_len);
static int mcu_write_dr_u32(struct jtag_tap *tap, uint32_t *ir_in, uint32_t ir_out, int dr_len);


static int avr_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
                            int *reg_list_size,
                            enum target_register_class reg_class);

static int avr_read_memory(struct target *target, target_addr_t address,
                       uint32_t size, uint32_t count, uint8_t *buffer);

static int avr_write_memory(struct target *target, target_addr_t address,
                        uint32_t size, uint32_t count, const uint8_t *buffer);

static int avr_add_breakpoint(struct target *target, struct breakpoint *breakpoint);

static int avr_remove_breakpoint(struct target *target,
                             struct breakpoint *breakpoint);

struct target_type avr_target = {
	.name = "avr",

	.poll = avr_poll,
	.arch_state = avr_arch_state,

	.halt = avr_halt,
	.resume = avr_resume,
	.step = avr_step,

	.assert_reset = avr_assert_reset,
	.deassert_reset = avr_deassert_reset,

	.get_gdb_reg_list = avr_get_gdb_reg_list,

	.read_memory = avr_read_memory,
	.write_memory = avr_write_memory,
/*
	.bulk_write_memory = avr_bulk_write_memory,
	.checksum_memory = avr_checksum_memory,
	.blank_check_memory = avr_blank_check_memory,

	.run_algorithm = avr_run_algorithm,
*/
	.add_breakpoint = avr_add_breakpoint,
	.remove_breakpoint = avr_remove_breakpoint,

/*
	.add_watchpoint = avr_add_watchpoint,
	.remove_watchpoint = avr_remove_watchpoint,
*/
	.target_create = avr_target_create,
	.init_target = avr_init_target,
};


// struct avr_alloc_reg_list{
// 	struct reg gp_regs[AVR_NUM_GP_REGS];
// 	uint8_t gp_reg_vals[AVR_NUM_GP_REGS];

// 	struct reg pc;
// 	struct reg sp;
// 	struct reg status;
	
// };

static inline struct avr_common *
target_to_avr(struct target *target)
{
	return (struct avr_common *)target->arch_info;
}

static void avr_reg_init(struct avr_core_reg_cache* reg_cache, struct reg *reg, int number, int bits, uint8_t *value){
	reg->name = avr_core_reg_names[i];
	reg->size = AVR_REG_GEN_BITS;
	reg->value = value;
	reg->dirty = false;
	reg->valid = false;
	reg->hidden = false;
	reg->type = NULL;
	reg->arch_info = NULL;
	reg->group = NULL;
	reg->number = number;
	reg->exist = true;

	reg->caller_save = false; // ???

	reg_cache->reg_list[number] = reg;
}

static void avr_reg_cache_init(struct avr_core_reg_cache* reg_cache)
{
	struct reg *gp_regs = reg_cache->gp_regs;

	for(int i=0; i<AVR_NUM_GP_REGS; ++i){
		
		uint8_t *value = reg_cache->gp_reg_vals + i * AVR_REG_GEN_BYTES;
		avr_reg_init(&gp_regs[i], i, AVR_REG_GEN_BITS, value);
		// gp_regs[i].name = avr_core_reg_names[i];
		// gp_regs[i].size = AVR_REG_GEN_BITS;
		// gp_regs[i].value = reg_cache->gp_reg_vals + i * AVR_REG_GEN_BYTES;
		// gp_regs[i].dirty = false;
		// gp_regs[i].valid = false;
		// gp_regs[i].hidden = false;
		// gp_regs[i].type = NULL;
		// gp_regs[i].arch_info = NULL;
		// gp_regs[i].group = NULL;
		// gp_regs[i].number = i;
		// gp_regs[i].exist = true;

		// gp_regs[i].caller_save = false; // ???

	}

	avr_reg_init(reg_cache, &reg_cache->pc, AVR_REG_PC, AVR_REG_PC_BITS, calloc(1, AVR_REG_PC_BYTES));
	avr_reg_init(reg_cache, &reg_cache->sp, AVR_REG_SP, AVR_REG_SP_BITS, calloc(1, AVR_REG_SP_BYTES));
	avr_reg_init(reg_cache, &reg_cache->status, AVR_REG_STATUS, AVR_REG_STATUS_BITS, calloc(1, AVR_REG_STATUS_BYTES));
}

static int avr_target_create(struct target *target, Jim_Interp *interp)
{
	struct avr_common *avr = calloc(1, sizeof(struct avr_common));

	avr->jtag_info.tap = target->tap;
	target->arch_info = avr;
	avr_reg_cache_init(&avr->reg_cache);

	return ERROR_OK;
}

static int avr_init_target(struct command_context *cmd_ctx, struct target *target)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int avr_arch_state(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int avr_poll(struct target *target)
{
	if ((target->state == TARGET_RUNNING) || (target->state == TARGET_DEBUG_RUNNING))
		target->state = TARGET_HALTED;

	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int avr_halt(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	LOG_DEBUG("target->state: %s",
		target_state_name(target));

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN){
		LOG_WARNING("target was in unknown state when halt was requested");
	}


	target->debug_reason = DBG_REASON_DBGRQ;

	
	
	if (mcu_execute_queue() != ERROR_OK)
			return ERROR_FAIL;
	
	return ERROR_OK;
}

static int avr_resume(struct target *target, int current, target_addr_t address,
		int handle_breakpoints, int debug_execution)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int avr_step(struct target *target, int current, target_addr_t address, int handle_breakpoints)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int avr_assert_reset(struct target *target)
{
	struct avr_common *avr = target_to_avr(target);

	LOG_DEBUG("%s", __func__);
	target->state = TARGET_RESET;

	

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_AVR_RESET);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 1, AVR_JTAG_REG_RESET_LEN);

	return ERROR_OK;
}

static int avr_deassert_reset(struct target *target)
{
	LOG_DEBUG("%s", __func__);

	target->state = TARGET_RUNNING;

	
	return ERROR_OK;
}

int avr_jtag_senddat(struct jtag_tap *tap, uint32_t *dr_in, uint32_t dr_out,
		int len)
{
	return mcu_write_dr_u32(tap, dr_in, dr_out, len);
}

int avr_jtag_sendinstr(struct jtag_tap *tap, uint8_t *ir_in, uint8_t ir_out)
{
	return mcu_write_ir_u8(tap, ir_in, ir_out, AVR_JTAG_INS_LEN);
}

/* IR and DR functions */
static int mcu_write_ir(struct jtag_tap *tap, uint8_t *ir_in, uint8_t *ir_out,
		int ir_len)
{
	if (!tap) {
		LOG_ERROR("invalid tap");
		return ERROR_FAIL;
	}
	if ((unsigned int)ir_len != tap->ir_length) {
		LOG_ERROR("invalid ir_len");
		return ERROR_FAIL;
	}

	{
		jtag_add_plain_ir_scan(tap->ir_length, ir_out, ir_in, TAP_IDLE);
	}

	return ERROR_OK;
}

static int mcu_write_dr(struct jtag_tap *tap, uint8_t *dr_in, uint8_t *dr_out,
		int dr_len)
{
	if (!tap) {
		LOG_ERROR("invalid tap");
		return ERROR_FAIL;
	}

	{
		jtag_add_plain_dr_scan(dr_len, dr_out, dr_in, TAP_IDLE);
	}

	return ERROR_OK;
}

static int mcu_write_ir_u8(struct jtag_tap *tap, uint8_t *ir_in,
		uint8_t ir_out, int ir_len)
{
	if (ir_len > 8) {
		LOG_ERROR("ir_len overflow, maximum is 8");
		return ERROR_FAIL;
	}

	mcu_write_ir(tap, ir_in, &ir_out, ir_len);

	return ERROR_OK;
}

static int mcu_write_dr_u32(struct jtag_tap *tap, uint32_t *dr_in,
		uint32_t dr_out, int dr_len)
{
	if (dr_len > 32) {
		LOG_ERROR("dr_len overflow, maximum is 32");
		return ERROR_FAIL;
	}

	mcu_write_dr(tap, (uint8_t *)dr_in, (uint8_t *)&dr_out, dr_len);

	return ERROR_OK;
}

int mcu_execute_queue(void)
{
	return jtag_execute_queue();
}

static int avr_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
                            int *reg_list_size,
                            enum target_register_class reg_class)
{
	struct avr_common *avrCommon = target_to_avr(target);
	struct avr_core_reg_cache *reg_cache = &avrCommon->reg_cache;

	int size = (reg_class == REG_CLASS_ALL) ? AVR_NUM_REGS : AVR_NUM_GP_REGS;

	
	*reg_list = calloc(size, sizeof(struct reg*));
	if (!reg_list) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	*reg_list_size = size;

	for(int i=0; i<size; ++i){
		(*reg_list)[i] = reg_cache->reg_list[i];
	}

	return ERROR_OK;
}

static int avr_read_memory(struct target *target, target_addr_t address,
                       uint32_t size, uint32_t count, uint8_t *buffer)
{
	// struct avr_common *avrCommon = target_to_avr(target);

	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* sanitize arguments */
	if (((size != 4) && (size != 2) && (size != 1)) || (count == 0) || !(buffer))
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (((size == 4) && (address & 0x3u)) || ((size == 2) && (address & 0x1u)))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	// TODO

	return ERROR_OK;
}

static int avr_write_memory(struct target *target, target_addr_t address,
                        uint32_t size, uint32_t count, const uint8_t *buffer)
{

	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* sanitize arguments */
	if (((size != 4) && (size != 2) && (size != 1)) || (count == 0) || !(buffer))
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (((size == 4) && (address & 0x3u)) || ((size == 2) && (address & 0x1u)))
		return ERROR_TARGET_UNALIGNED_ACCESS;


	// TODO

	return ERROR_OK;
}

static int avr_add_breakpoint(struct target *target, struct breakpoint *breakpoint)
{
	// struct avr_common *avrCommon = target_to_avr(target);

	// TODO

	return ERROR_OK;
}

static int avr_remove_breakpoint(struct target *target,
                             struct breakpoint *breakpoint)
{
	// struct avr_common *avrCommon = target_to_avr(target);

	// TODO
}

static int avr_read_ocd_reg(struct avr_common *avr, uint8_t which_reg, uint16_t *val_out)
{
	struct jtag_tap *tap = avr->jtag_info.tap;
	int res = 0;
	
	// set to access OCD register command
	uint8_t rx_dummy8 = 0;
	res = avr_jtag_sendinstr(tap, &rx_dummy8, AVR_JTAG_INS_ACCESS_OCD_REG);
	if(res != ERROR_OK){
		return res;
	}

	// latch the register we want to access (reg address followed by 0)
	uint32_t rx_dummy32 = 0;
	uint32_t tx_data = which_reg;
	res = avr_jtag_senddat(tap, &rx_dummy32, tx_data, 5);
	if(res != ERROR_OK){
		return res;
	}

	// read the actual data (16 bit dummy data, then reg address, then 0 (read)) 
	rx_dummy32 = 0;
	tx_data =  (which_reg << 16);
	res = avr_jtag_senddat(tap, &rx_dummy32, tx_data, 21);
	if(res != ERROR_OK){
		return res;
	}

	if (mcu_execute_queue() != ERROR_OK)
		return ERROR_FAIL;

	*val_out = rx_dummy32 & 0xFFFF;

	return ERROR_OK;
}

static int avr_write_ocd_reg(struct avr_common *avr, uint8_t which_reg, uint16_t val_out)
{
	struct jtag_tap *tap = avr->jtag_info.tap;
	int res = 0;
	
	// set to access OCD register command
	uint8_t rx_dummy8 = 0;
	res = avr_jtag_sendinstr(tap, &rx_dummy8, AVR_JTAG_INS_ACCESS_OCD_REG);
	if(res != ERROR_OK){
		return res;
	}

	// set register we want to access and write
	uint32_t rx_dummy32 = 0;
	uint32_t tx_data = (1 << 20) | (which_reg << 16) | val_out;
	res = avr_jtag_senddat(tap, &rx_dummy32, tx_data, 5);
	if(res != ERROR_OK){
		return res;
	}

	return ERROR_OK;
}

static int avr_exec_ins(struct avr_common *avr, uint32_t ins)
{
	struct jtag_tap *tap = avr->jtag_info.tap;
	int res = 0;
	// set to access OCD register command
	uint8_t rx_dummy8 = 0;
	res = avr_jtag_sendinstr(tap, &rx_dummy8, AVR_JTAG_INS_EXEC_INS);
	if(res != ERROR_OK){
		return res;
	}

	uint32_t dummy = 0;
	res = avr_jtag_senddat(tap, &dummy, ins, 16);
	if(res != ERROR_OK){
		return res;
	}

	return ERROR_OK;
}