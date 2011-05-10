/**
 * \file
 * <!--
 * This file is part of BeRTOS.
 *
 * Bertos is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As a special exception, you may use this file as part of a free software
 * library without restriction.  Specifically, if other files instantiate
 * templates or use macros or inline functions from this file, or you compile
 * this file and link it with other files to produce an executable, this
 * file does not by itself cause the resulting executable to be covered by
 * the GNU General Public License.  This exception does not however
 * invalidate any other reasons why the executable file might be covered by
 * the GNU General Public License.
 *
 * Copyright 2011 Develer S.r.l. (http://www.develer.com/)
 *
 * -->
 *
 * \brief Micron MT29F serial NAND driver for SAM3's static memory controller.
 *
 * \author Stefano Fedrigo <aleph@develer.com>
 */

#include "mt29f_sam3.h"
#include <drv/mt29f.h>
#include <cfg/log.h>
#include <cfg/macros.h>
#include <io/sam3.h>
#include <drv/timer.h>
#include <cpu/power.h> /* cpu_relax() */
#include <cpu/types.h>

#include <string.h> /* memcpy, memset */


/*
 * PIO definitions.
 */
#define MT29F_PIN_CE        BV(6)
#define MT29F_PIN_RB        BV(2)
#define MT29F_PINS_PORTA    (MT29F_PIN_CE | MT29F_PIN_RB)
#define MT29F_PERIPH_PORTA  PIO_PERIPH_B

#define MT29F_PIN_OE        BV(19)
#define MT29F_PIN_WE        BV(20)
#define MT29F_PIN_IO        0x0000FFFF
#define MT29F_PINS_PORTC    (MT29F_PIN_OE | MT29F_PIN_WE | MT29F_PIN_IO)
#define MT29F_PERIPH_PORTC  PIO_PERIPH_A

#define MT29F_PIN_CLE       BV(9)
#define MT29F_PIN_ALE       BV(8)
#define MT29F_PINS_PORTD    (MT29F_PIN_CLE | MT29F_PIN_ALE)
#define MT29F_PERIPH_PORTD  PIO_PERIPH_A


/*
 * Wait for edge transition of READY/BUSY NAND
 * signal.
 * Return true for edge detection, false in case of timeout.
 */
bool mt29f_waitReadyBusy(UNUSED_ARG(Mt29f *, chip), time_t timeout)
{
	time_t start = timer_clock();

	while (!(SMC_SR & SMC_SR_RB_EDGE0))
	{
		cpu_relax();
		if (timer_clock() - start > timeout)
		{
			LOG_INFO("mt29f: R/B timeout\n");
			return false;
		}
	}

	return true;
}

/*
 * Wait for transfer to complete until timeout.
 * If transfer completes return true, false in case of timeout.
 */
bool mt29f_waitTransferComplete(UNUSED_ARG(Mt29f *, chip), time_t timeout)
{
	time_t start = timer_clock();

	while (!(SMC_SR & SMC_SR_XFRDONE))
	{
		cpu_relax();
		if (timer_clock() - start > timeout)
		{
			LOG_INFO("mt29f: xfer complete timeout\n");
			return false;
		}
	}

	return true;
}


/*
 * Send command to NAND and wait for completion.
 */
void mt29f_sendCommand(Mt29f *chip,
		uint32_t cmd1, uint32_t cmd2,
		int num_cycles, uint32_t cycle0, uint32_t cycle1234)
{
	reg32_t *cmd_addr;
	uint32_t cmd_val;

	while (HWREG(NFC_CMD_BASE_ADDR + NFC_CMD_NFCCMD) & 0x8000000);

	if (num_cycles == 5)
		SMC_ADDR = cycle0;

	cmd_val = NFC_CMD_NFCCMD
		| ((chip->chip_select << NFC_CMD_CSID_SHIFT) & NFC_CMD_CSID_MASK)
		| ((num_cycles << NFC_CMD_ACYCLE_SHIFT) & NFC_CMD_ACYCLE_MASK)
		| cmd1 << 2
		| cmd2 << 10;

	// Check for commands transferring data
	if (cmd1 == MT29F_CMD_WRITE_1 || cmd1 == MT29F_CMD_READ_1 || cmd1 == MT29F_CMD_READID)
		cmd_val |= NFC_CMD_NFCEN;

	// Check for commands writing data
	if (cmd1 == MT29F_CMD_WRITE_1)
		cmd_val |= NFC_CMD_NFCWR;

	// Check for two command cycles
	if (cmd2)
		cmd_val |= NFC_CMD_VCMD2;

	cmd_addr = (reg32_t *)(NFC_CMD_BASE_ADDR + cmd_val);
	*cmd_addr = cycle1234;

    while (!(SMC_SR & SMC_SR_CMDDONE));
}


/*
 * Get NAND chip status register.
 *
 * NOTE: this is global between different chip selects, so returns
 * the status register of the last used NAND chip.
 */
uint8_t mt29f_getChipStatus(UNUSED_ARG(Mt29f *, chip))
{
	return (uint8_t)HWREG(NFC_CMD_BASE_ADDR);
}


void *mt29f_dataBuffer(UNUSED_ARG(Mt29f *, chip))
{
	return (void *)NFC_SRAM_BASE_ADDR;
}


bool mt29f_checkEcc(Mt29f *chip)
{
	uint32_t sr1 = SMC_ECC_SR1;
	if (sr1)
	{
		LOG_INFO("ECC error, ECC_SR1=0x%lx\n", sr1);
		chip->status |= MT29F_ERR_ECC;
		return false;
	}
	else
		return true;
}


/*
 * Compute ECC on data in a buffer.
 *
 * \param chip      nand context
 * \param buf       buffer containing data
 * \param size      size of data buffer
 * \param ecc       pointer to buffer where computed ECC is stored
 * \param ecc_size  max size for ecc buffer
 */
void mt29f_computeEcc(UNUSED_ARG(Mt29f *, chip),
		UNUSED_ARG(const void *, buf), UNUSED_ARG(size_t, size), uint32_t *ecc, size_t ecc_size)
{
	size_t i;
	for (i = 0; i < ecc_size; i++)
		ecc[i] = *((reg32_t *)(SMC_BASE + SMC_ECC_PR0_OFF) + i);
}


void mt29f_hwInit(UNUSED_ARG(Mt29f *, chip))
{
	// FIXME: Parameters specific for MT29F8G08AAD

	// PIO init
	pmc_periphEnable(PIOA_ID);
	pmc_periphEnable(PIOC_ID);
	pmc_periphEnable(PIOD_ID);

	PIO_PERIPH_SEL(PIOA_BASE, MT29F_PINS_PORTA, MT29F_PERIPH_PORTA);
	PIOA_PDR = MT29F_PINS_PORTA;
	PIOA_PUER = MT29F_PINS_PORTA;

	PIO_PERIPH_SEL(PIOC_BASE, MT29F_PINS_PORTC, MT29F_PERIPH_PORTC);
	PIOC_PDR = MT29F_PINS_PORTC;
	PIOC_PUER = MT29F_PINS_PORTC;

	PIO_PERIPH_SEL(PIOD_BASE, MT29F_PINS_PORTD, MT29F_PERIPH_PORTD);
	PIOD_PDR = MT29F_PINS_PORTD;
	PIOD_PUER = MT29F_PINS_PORTD;

    pmc_periphEnable(SMC_SDRAMC_ID);

	// SMC init
    SMC_SETUP0 = SMC_SETUP_NWE_SETUP(0)
		| SMC_SETUP_NCS_WR_SETUP(0)
		| SMC_SETUP_NRD_SETUP(0)
		| SMC_SETUP_NCS_RD_SETUP(0);

    SMC_PULSE0 = SMC_PULSE_NWE_PULSE(2)
		| SMC_PULSE_NCS_WR_PULSE(3)
		| SMC_PULSE_NRD_PULSE(2)
		| SMC_PULSE_NCS_RD_PULSE(3);

    SMC_CYCLE0 = SMC_CYCLE_NWE_CYCLE(3)
		| SMC_CYCLE_NRD_CYCLE(3);

    SMC_TIMINGS0 = SMC_TIMINGS_TCLR(1)
		| SMC_TIMINGS_TADL(6)
		| SMC_TIMINGS_TAR(4)
		| SMC_TIMINGS_TRR(2)
		| SMC_TIMINGS_TWB(9)
		| SMC_TIMINGS_RBNSEL(7)
		| SMC_TIMINGS_NFSEL;

    SMC_MODE0 = SMC_MODE_READ_MODE
		| SMC_MODE_WRITE_MODE;

	SMC_CFG = SMC_CFG_PAGESIZE_PS2048_64
		| SMC_CFG_EDGECTRL
		| SMC_CFG_DTOMUL_X1048576
		| SMC_CFG_DTOCYC(0xF)
		| SMC_CFG_WSPARE
		| SMC_CFG_RSPARE;

	// Disable SMC interrupts, reset and enable NFC controller
	SMC_IDR = ~0;
	SMC_CTRL = 0;
	SMC_CTRL = SMC_CTRL_NFCEN;

	// Enable ECC, 1 ECC per 256 bytes
	SMC_ECC_CTRL = SMC_ECC_CTRL_SWRST;
	SMC_ECC_MD = SMC_ECC_MD_ECC_PAGESIZE_PS2048_64 | SMC_ECC_MD_TYPCORREC_C256B;
}
