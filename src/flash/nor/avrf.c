// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2009 by Simon Qian                                      *
 *   SimonQian@SimonQian.com                                               *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <target/avrt.h>

struct avrf_type {
	char name[15];
	uint16_t chip_id;
	int flash_page_size;
	int flash_page_num;
	int eeprom_page_size;
	int eeprom_page_num;
};

struct avrf_flash_bank {
	int ppage_size;
	bool probed;
};

static const struct avrf_type avft_chips_info[] = {
/*	name, chip_id,	flash_page_size, flash_page_num,
 *			eeprom_page_size, eeprom_page_num
 */
	{"atmega128", 0x9702, 256, 512, 8, 512},
	{"atmega128rfa1", 0xa701, 128, 512, 8, 512},
	{"atmega256rfr2", 0xa802, 256, 1024, 8, 1024},
	{"at90can128", 0x9781, 256, 512, 8, 512},
	{"at90usb128", 0x9782, 256, 512, 8, 512},
	{"atmega164p", 0x940a, 128, 128, 4, 128},
	{"atmega324p", 0x9508, 128, 256, 4, 256},
	{"atmega324pa", 0x9511, 128, 256, 4, 256},
	{"atmega644p", 0x960a, 256, 256, 8, 256},
	{"atmega1284p", 0x9705, 256, 512, 8, 512},
	{"atmega32u4", 0x9587, 128, 256, 4, 256},
};

/* avr program functions */
static int avr_jtag_reset(struct avr_common *avr, uint32_t reset)
{
	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_AVR_RESET);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, reset, AVR_JTAG_REG_RESET_LEN);

	return ERROR_OK;
}

static int avr_jtag_read_jtagid(struct avr_common *avr, uint32_t *id)
{
	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_IDCODE);
	avr_jtag_senddat(avr->jtag_info.tap, id, 0, AVR_JTAG_REG_JTAGID_LEN);

	return ERROR_OK;
}

static int avr_jtagprg_enterprogmode(struct avr_common *avr)
{
	avr_jtag_reset(avr, 1);

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_ENABLE);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0xA370, AVR_JTAG_REG_PROGRAMMING_ENABLE_LEN);

	return ERROR_OK;
}

static int avr_jtagprg_leaveprogmode(struct avr_common *avr)
{
	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_COMMANDS);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x2300, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3300, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_ENABLE);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0, AVR_JTAG_REG_PROGRAMMING_ENABLE_LEN);

	avr_jtag_reset(avr, 0);

	return ERROR_OK;
}

static int avr_jtagprg_chiperase(struct avr_common *avr)
{
	uint32_t poll_value;

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_COMMANDS);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x2380, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3180, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3380, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3380, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	do {
		poll_value = 0;
		avr_jtag_senddat(avr->jtag_info.tap,
			&poll_value,
			0x3380,
			AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
		if (mcu_execute_queue() != ERROR_OK)
			return ERROR_FAIL;
		LOG_DEBUG("poll_value = 0x%04" PRIx32 "", poll_value);
	} while (!(poll_value & 0x0200));

	return ERROR_OK;
}

static int avr_jtagprg_writeflashpage(struct avr_common *avr,
	const bool ext_addressing,
	const uint8_t *page_buf,
	uint32_t buf_size,
	uint32_t addr,
	uint32_t page_size)
{
	uint32_t poll_value;

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_COMMANDS);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x2310, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	/* load extended high byte */
	if (ext_addressing)
		avr_jtag_senddat(avr->jtag_info.tap,
			NULL,
			0x0b00 | ((addr >> 17) & 0xFF),
			AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	/* load addr high byte */
	avr_jtag_senddat(avr->jtag_info.tap,
		NULL,
		0x0700 | ((addr >> 9) & 0xFF),
		AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	/* load addr low byte */
	avr_jtag_senddat(avr->jtag_info.tap,
		NULL,
		0x0300 | ((addr >> 1) & 0xFF),
		AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_PAGELOAD);

	for (uint32_t i = 0; i < page_size; i++) {
		if (i < buf_size)
			avr_jtag_senddat(avr->jtag_info.tap, NULL, page_buf[i], 8);
		else
			avr_jtag_senddat(avr->jtag_info.tap, NULL, 0xFF, 8);
	}

	avr_jtag_sendinstr(avr->jtag_info.tap, NULL, AVR_JTAG_INS_PROG_COMMANDS);

	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3700, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3500, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3700, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
	avr_jtag_senddat(avr->jtag_info.tap, NULL, 0x3700, AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);

	do {
		poll_value = 0;
		avr_jtag_senddat(avr->jtag_info.tap,
			&poll_value,
			0x3700,
			AVR_JTAG_REG_PROGRAMMING_COMMAND_LEN);
		if (mcu_execute_queue() != ERROR_OK)
			return ERROR_FAIL;
		LOG_DEBUG("poll_value = 0x%04" PRIx32 "", poll_value);
	} while (!(poll_value & 0x0200));

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(avrf_flash_bank_command)
{
	struct avrf_flash_bank *avrf_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	avrf_info = malloc(sizeof(struct avrf_flash_bank));
	bank->driver_priv = avrf_info;

	avrf_info->probed = false;

	return ERROR_OK;
}

static int avrf_erase(struct flash_bank *bank, unsigned int first,
		unsigned int last)
{
	struct target *target = bank->target;
	struct avr_common *avr = target->arch_info;
	int status;

	LOG_DEBUG("%s", __func__);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	status = avr_jtagprg_enterprogmode(avr);
	if (status != ERROR_OK)
		return status;

	status = avr_jtagprg_chiperase(avr);
	if (status != ERROR_OK)
		return status;

	return avr_jtagprg_leaveprogmode(avr);
}

static int avrf_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	struct avr_common *avr = target->arch_info;
	uint32_t cur_size, cur_buffer_size, page_size;
	bool ext_addressing;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	page_size = bank->sectors[0].size;
	if ((offset % page_size) != 0) {
		LOG_WARNING("offset 0x%" PRIx32 " breaks required %" PRIu32 "-byte alignment",
			offset,
			page_size);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	LOG_DEBUG("offset is 0x%08" PRIx32 "", offset);
	LOG_DEBUG("count is %" PRIu32 "", count);

	if (avr_jtagprg_enterprogmode(avr) != ERROR_OK)
		return ERROR_FAIL;

	if (bank->size > 0x20000)
		ext_addressing = true;
	else
		ext_addressing = false;

	cur_size = 0;
	while (count > 0) {
		if (count > page_size)
			cur_buffer_size = page_size;
		else
			cur_buffer_size = count;
		avr_jtagprg_writeflashpage(avr,
			ext_addressing,
			buffer + cur_size,
			cur_buffer_size,
			offset + cur_size,
			page_size);
		count -= cur_buffer_size;
		cur_size += cur_buffer_size;

		keep_alive();
	}

	return avr_jtagprg_leaveprogmode(avr);
}

#define EXTRACT_MFG(X)  (((X) & 0xffe) >> 1)
#define EXTRACT_PART(X) (((X) & 0xffff000) >> 12)
#define EXTRACT_VER(X)  (((X) & 0xf0000000) >> 28)

static int avrf_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct avrf_flash_bank *avrf_info = bank->driver_priv;
	struct avr_common *avr = target->arch_info;
	const struct avrf_type *avr_info = NULL;
	uint32_t device_id;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	avrf_info->probed = false;

	avr_jtag_read_jtagid(avr, &device_id);
	if (mcu_execute_queue() != ERROR_OK)
		return ERROR_FAIL;

	LOG_INFO("device id = 0x%08" PRIx32 "", device_id);
	if (EXTRACT_MFG(device_id) != 0x1F)
		LOG_ERROR("0x%" PRIx32 " is invalid Manufacturer for avr, 0x%X is expected",
			EXTRACT_MFG(device_id),
			0x1F);

	for (size_t i = 0; i < ARRAY_SIZE(avft_chips_info); i++) {
		if (avft_chips_info[i].chip_id == EXTRACT_PART(device_id)) {
			avr_info = &avft_chips_info[i];
			LOG_INFO("target device is %s", avr_info->name);
			break;
		}
	}

	if (avr_info) {
		free(bank->sectors);

		/* chip found */
		bank->base = 0x00000000;
		bank->size = (avr_info->flash_page_size * avr_info->flash_page_num);
		bank->num_sectors = avr_info->flash_page_num;
		bank->sectors = malloc(sizeof(struct flash_sector) * avr_info->flash_page_num);

		for (int i = 0; i < avr_info->flash_page_num; i++) {
			bank->sectors[i].offset = i * avr_info->flash_page_size;
			bank->sectors[i].size = avr_info->flash_page_size;
			bank->sectors[i].is_erased = -1;
			bank->sectors[i].is_protected = -1;
		}

		avrf_info->probed = true;
		return ERROR_OK;
	} else {
		/* chip not supported */
		LOG_ERROR("0x%" PRIx32 " is not support for avr", EXTRACT_PART(device_id));

		avrf_info->probed = true;
		return ERROR_FAIL;
	}
}

static int avrf_auto_probe(struct flash_bank *bank)
{
	struct avrf_flash_bank *avrf_info = bank->driver_priv;
	if (avrf_info->probed)
		return ERROR_OK;
	return avrf_probe(bank);
}

static int avrf_info(struct flash_bank *bank, struct command_invocation *cmd)
{
	struct target *target = bank->target;
	struct avr_common *avr = target->arch_info;
	const struct avrf_type *avr_info = NULL;
	uint32_t device_id;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	avr_jtag_read_jtagid(avr, &device_id);
	if (mcu_execute_queue() != ERROR_OK)
		return ERROR_FAIL;

	LOG_INFO("device id = 0x%08" PRIx32 "", device_id);
	if (EXTRACT_MFG(device_id) != 0x1F)
		LOG_ERROR("0x%" PRIx32 " is invalid Manufacturer for avr, 0x%X is expected",
			EXTRACT_MFG(device_id),
			0x1F);

	for (size_t i = 0; i < ARRAY_SIZE(avft_chips_info); i++) {
		if (avft_chips_info[i].chip_id == EXTRACT_PART(device_id)) {
			avr_info = &avft_chips_info[i];
			LOG_INFO("target device is %s", avr_info->name);

			break;
		}
	}

	if (avr_info) {
		/* chip found */
		command_print_sameline(cmd, "%s - Rev: 0x%" PRIx32 "", avr_info->name,
			EXTRACT_VER(device_id));
		return ERROR_OK;
	} else {
		/* chip not supported */
		command_print_sameline(cmd, "Cannot identify target as a avr\n");
		return ERROR_FLASH_OPERATION_FAILED;
	}
}

static int avrf_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct avr_common *avr = target->arch_info;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((avr_jtagprg_enterprogmode(avr) != ERROR_OK)
	    || (avr_jtagprg_chiperase(avr) != ERROR_OK)
	    || (avr_jtagprg_leaveprogmode(avr) != ERROR_OK))
		return ERROR_FAIL;

	return ERROR_OK;
}

COMMAND_HANDLER(avrf_handle_mass_erase_command)
{
	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;

	if (avrf_mass_erase(bank) == ERROR_OK)
		command_print(CMD, "avr mass erase complete");
	else
		command_print(CMD, "avr mass erase failed");

	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static const struct command_registration avrf_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.usage = "<bank>",
		.handler = avrf_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.help = "erase entire device",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration avrf_command_handlers[] = {
	{
		.name = "avrf",
		.mode = COMMAND_ANY,
		.help = "AVR flash command group",
		.usage = "",
		.chain = avrf_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

const struct flash_driver avr_flash = {
	.name = "avr",
	.commands = avrf_command_handlers,
	.flash_bank_command = avrf_flash_bank_command,
	.erase = avrf_erase,
	.write = avrf_write,
	.read = default_flash_read,
	.probe = avrf_probe,
	.auto_probe = avrf_auto_probe,
	.erase_check = default_flash_blank_check,
	.info = avrf_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
