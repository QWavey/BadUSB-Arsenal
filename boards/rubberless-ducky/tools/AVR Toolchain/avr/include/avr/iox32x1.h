/* Copyright (c) 2010 Atmel Corporation
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.

   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.

   * Neither the name of the copyright holders nor the names of
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE. */

/* $Id: iox32x1.h 89716 2011-02-07 13:35:46Z pablaasmo $ */

/* avr/iox32x1.h - definitions for ATxmega32X1 */

/* This file should only be included from <avr/io.h>, never directly. */

#ifndef _AVR_IO_H_
#  error "Include <avr/io.h> instead of this file."
#endif

#ifndef _AVR_IOXXX_H_
#  define _AVR_IOXXX_H_ "iox32x1.h"
#else
#  error "Attempt to include more than one <avr/ioXXX.h> file."
#endif


#ifndef _AVR_ATxmega32X1_H_
#define _AVR_ATxmega32X1_H_ 1


/* Ungrouped common registers */
#define GPIOR0  _SFR_MEM8(0x0000)  /* General Purpose IO Register 0 */
#define GPIOR1  _SFR_MEM8(0x0001)  /* General Purpose IO Register 1 */
#define GPIOR2  _SFR_MEM8(0x0002)  /* General Purpose IO Register 2 */
#define GPIOR3  _SFR_MEM8(0x0003)  /* General Purpose IO Register 3 */

#define CCP  _SFR_MEM8(0x0034)  /* Configuration Change Protection */
#define SPL  _SFR_MEM8(0x003D)  /* Stack Pointer Low */
#define SPH  _SFR_MEM8(0x003E)  /* Stack Pointer High */
#define SREG  _SFR_MEM8(0x003F)  /* Status Register */


/* C Language Only */
#if !defined (__ASSEMBLER__)

#include <stdint.h>

typedef volatile uint8_t register8_t;
typedef volatile uint16_t register16_t;
typedef volatile uint32_t register32_t;


#ifdef _WORDREGISTER
#undef _WORDREGISTER
#endif
#define _WORDREGISTER(regname)   \
    __extension__ union \
    { \
        register16_t regname; \
        struct \
        { \
            register8_t regname ## L; \
            register8_t regname ## H; \
        }; \
    }

#ifdef _DWORDREGISTER
#undef _DWORDREGISTER
#endif
#define _DWORDREGISTER(regname)  \
    __extension__ union \
    { \
        register32_t regname; \
        struct \
        { \
            register8_t regname ## 0; \
            register8_t regname ## 1; \
            register8_t regname ## 2; \
            register8_t regname ## 3; \
        }; \
    }


/*
==========================================================================
IO Module Structures
==========================================================================
*/

/*
--------------------------------------------------------------------------
BCD - Bus Connect/Disconnect on Communication Lines
--------------------------------------------------------------------------
*/

/* Bus Connect/Disconnect on Communication Lines */
typedef struct BCD_struct
{
    register8_t CTRL;  /* Control Register */
    register8_t STATUS;  /* Status Register */
} BCD_t;

/* Interrupt Polarity */
typedef enum BCD_INTPOL_enum
{
    BCD_INTPOL_CONNECT_gc = (0x0<<6),  /* Interrupt on Connect */
    BCD_INTPOL_DISCONNECT_gc = (0x1<<6),  /* Interrupt on Disconnect */
} BCD_INTPOL_t;

/* Timeout */
typedef enum BCD_TIMEOUT_enum
{
    BCD_TIMEOUT_250ms_gc = (0x00<<4),  /* 250ms +-15% */
    BCD_TIMEOUT_500ms_gc = (0x01<<4),  /* 500ms +-15% */
    BCD_TIMEOUT_1000ms_gc = (0x02<<4),  /* 1000ms +-15% */
    BCD_TIMEOUT_2000ms_gc = (0x03<<4),  /* 2000ms +-15% */
} BCD_TIMEOUT_t;

/* Mode */
typedef enum BCD_MODE_enum
{
    BCD_MODE_DISABLE_gc = (0x00<<4),  /* Disabled */
    BCD_MODE_TWOWIRE_gc = (0x01<<4),  /* 2-wire mode */
    BCD_MODE_ONEWIRE_gc = (0x02<<4),  /* 1-wire mode% */
} BCD_MODE_t;


/*
--------------------------------------------------------------------------
VREF - Voltage Reference
--------------------------------------------------------------------------
*/

/* Voltage Reference */
typedef struct VREF_struct
{
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t STATUS;  /* Status Register */
} VREF_t;


/*
--------------------------------------------------------------------------
CADC - Current ADC
--------------------------------------------------------------------------
*/

/* Current ADC */
typedef struct CADC_struct
{
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t CTRLC;  /* Control Register C */
    register8_t INTCTRL;  /* Interrupt Control Register */
    register8_t DIAGNOSIS;  /* Diagnosis Register */
    register8_t STATUS;  /* Status Register */
    register8_t PROTECTION;  /* Protection Register */
    _WORDREGISTER(INST);  /* Instantaneous Conversion Result */
    _WORDREGISTER(I2INST);  /* I2 Instantaneous Conversion Result */
    _WORDREGISTER(I2ACC);  /* I2 Accumulated Conversion Result */
    _WORDREGISTER(PHTHRES);  /* Protection Window Comparator High Threshold */
    _WORDREGISTER(PLTHRES);  /* Protection Window Comparator Low Threshold */
    _WORDREGISTER(SHTHRES);  /* Sample Mode Window Comparator High Threshold */
    _WORDREGISTER(SLTHRES);  /* Sample Mode Window Comparator Low Threshold */
} CADC_t;

/* Accumulated Decimation Ratio */
typedef enum CADC_ACDEC_enum
{
    CADC_ACDEC_512_gc = (0x00<<0),  /* 512 decimation */
    CADC_ACDEC_256_gc = (0x01<<0),  /* 256 decimation */
    CADC_ACDEC_128_gc = (0x02<<0),  /* 128 decimation */
    CADC_ACDEC_64_gc = (0x03<<0),  /* 64 decimation */
} CADC_ACDEC_t;

/* Instantaneous Decimation Ratio */
typedef enum CADC_ICDEC_enum
{
    CADC_ICDEC_256_gc = (0x0<<3),  /* 256 decimation */
    CADC_ICDEC_128_gc = (0x1<<3),  /* 128 decimation */
} CADC_ICDEC_t;

/* Pin Diagnosis Configuration */
typedef enum CADC_PINDM_enum
{
    CADC_PINDM_DIAG_OFF_gc = (0x00<<0),  /* No diagnosis enabled */
    CADC_PINDM_DIAG_PI_gc = (0x01<<0),  /* Pull-up on PI enabled */
    CADC_PINDM_DIAG_NI_gc = (0x02<<0),  /* Pull-up on NI enabled */
    CADC_PINDM_DIAG_PI_NI_gc = (0x03<<0),  /* Pull-up on PI and NI enabled */
} CADC_PINDM_t;

/* Clock Select */
typedef enum CADC_CLKSEL_enum
{
    CADC_CLKSEL_37500Hz_gc = (0x0<<6),  /* CADC clock set to 125kHz */
    CADC_CLKSEL_150000Hz_gc = (0x1<<6),  /* CADC clock set to 150kHz */
} CADC_CLKSEL_t;

/* CADC Sampling Intervall Configuration */
typedef enum CADC_SAI_enum
{
    CADC_SAI_250ms_gc = (0x00<<0),  /* 250ms sampling intervall */
    CADC_SAI_500ms_gc = (0x01<<0),  /* 500ms sampling intervall */
    CADC_SAI_1s_gc = (0x02<<0),  /* 1s sampling intervall */
    CADC_SAI_2s_gc = (0x03<<0),  /* 2s sampling intervall */
} CADC_SAI_t;


/*
--------------------------------------------------------------------------
CLK - Clock System
--------------------------------------------------------------------------
*/

/* Clock System */
typedef struct CLK_struct
{
    register8_t CLKPSR;  /* Clock Prescaler Register */
    register8_t CLKSLR;  /* Prescaler Settings Lock Register */
} CLK_t;

/* Clock Prescaler Division Factor  */
typedef enum CLKDIV_enum
{
    CLKDIV_CLKPS_DIV1_gc = (0x00<<0),  /* No division */
    CLKDIV_CLKPS_DIV2_gc = (0x01<<0),  /* Divide system oscillator with 2 */
    CLKDIV_CLKPS_DIV4_gc = (0x02<<0),  /* Divide system oscillator with 4 */
    CLKDIV_CKLPS_DIV8_gc = (0x03<<0),  /* Divide system oscillator with 8 */
} CLKDIV_t;


/*
--------------------------------------------------------------------------
COMTR - Communication Timout Reset
--------------------------------------------------------------------------
*/

/* Communication Timeout */
typedef struct COMTR_struct
{
    register8_t CTRL;  /* Status Register */
} COMTR_t;


/*
--------------------------------------------------------------------------
CPROT - DAC-Based Current Protection
--------------------------------------------------------------------------
*/

/* DAC-Based Current Protection */
typedef struct CPROT_struct
{
    register8_t CTRL;  /* Control Register */
    register8_t SCTIMING;  /* Short Circuit Protection Timing Register */
    register8_t OCTIMING;  /* Over Current Protection Timing Register */
    register8_t SCLEVEL;  /* Short Circuit Detection Level Register */
    register8_t DOCLEVEL;  /* Discharge Over Current Detection Level Register */
    register8_t COCLEVEL;  /* Charge Over Current Detection Level Register */
    register8_t OCREG;  /* Offset Correction Register */
} CPROT_t;


/* CCP signatures */
typedef enum CCP_enum
{
    CCP_SPM_gc = (0x9D<<0),  /* SPM Instruction Protection */
    CCP_IOREG_gc = (0xD8<<0),  /* IO Register Protection */
} CCP_t;


/*
--------------------------------------------------------------------------
CRC - Cyclic Redundancy Checker
--------------------------------------------------------------------------
*/

/* Cyclic Redundancy Checker */
typedef struct CRC_struct
{
    register8_t CTRL;  /* Control Register */
    register8_t STATUS;  /* Status Register */
    register8_t DATAIN;  /* Data Input */
    register8_t CHECKSUM0;  /* Checksum byte 0 */
    register8_t CHECKSUM1;  /* Checksum byte 1 */
    register8_t CHECKSUM2;  /* Checksum byte 2 */
    register8_t CHECKSUM3;  /* Checksum byte 3 */
} CRC_t;

/* Reset */
typedef enum CRC_RESET_enum
{
    CRC_RESET_NO_gc = (0x00<<6),  /* No Reset */
    CRC_RESET_RESET0_gc = (0x02<<6),  /* Reset CRC with CHECKSUM to all zeros */
    CRC_RESET_RESET1_gc = (0x03<<6),  /* Reset CRC with CHECKSUM to all ones */
} CRC_RESET_t;

/* Input Source */
typedef enum CRC_SOURCE_enum
{
    CRC_SOURCE_DISABLE_gc = (0x00<<0),  /* Disabled */
    CRC_SOURCE_IO_gc = (0x01<<0),  /* I/O Interface */
    CRC_SOURCE_FLASH_gc = (0x02<<0),  /* Flash */
} CRC_SOURCE_t;


/*
--------------------------------------------------------------------------
FET - FET controller
--------------------------------------------------------------------------
*/

/* FET controller */
typedef struct FET_struct
{
    register8_t CTRL;  /* FET Control Register */
    register8_t FETACTIONA;  /* FET Event Action Register A */
    register8_t reserved_0x02;
    register8_t reserved_0x03;
    register8_t reserved_0x04;
    register8_t FETACTIONE;  /* FET Event Action Register G */
    register8_t FETACTIONF;  /* FET Event Action Register F */
    register8_t FETACTIONG;  /* FET Event Action Register E */
    register8_t reserved_0x08;
    register8_t reserved_0x09;
    register8_t reserved_0x0A;
    register8_t reserved_0x0B;
    register8_t reserved_0x0C;
    register8_t reserved_0x0E;
    register8_t INTMASKC;  /* Protection Interrupt Control Register D */
    register8_t INTMASKD;  /* Interrupt Maks Register C */
    register8_t reserved_0x12;
    register8_t reserved_0x13;
    register8_t STATUSA;  /* Protection Status Register A */
    register8_t reserved_0x15;
    register8_t STATUSC;  /* Protection Status Register B */
    register8_t STATUSD;  /* Protection Status Register B */
} FET_t;



/*
--------------------------------------------------------------------------
NVM - Non Volatile Memory Controller
--------------------------------------------------------------------------
*/

/* Non-volatile Memory Controller */
typedef struct NVM_struct
{
    register8_t ADDR0;  /* Address Register 0 */
    register8_t ADDR1;  /* Address Register 1 */
    register8_t ADDR2;  /* Address Register 2 */
    register8_t reserved_0x03;
    register8_t DATA0;  /* Data Register 0 */
    register8_t DATA1;  /* Data Register 1 */
    register8_t DATA2;  /* Data Register 2 */
    register8_t reserved_0x07;
    register8_t reserved_0x08;
    register8_t reserved_0x09;
    register8_t CMD;  /* Command */
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t INTCTRL;  /* Interrupt Control */
    register8_t reserved_0x0E;
    register8_t STATUS;  /* Status */
    register8_t LOCKBITS;  /* Lock Bits */
    register8_t reserved_0x11;
    register8_t reserved_0x12;
    register8_t reserved_0x13;
    register8_t reserved_0x14;
    register8_t reserved_0x15;
    register8_t reserved_0x16;
    register8_t reserved_0x17;
    register8_t reserved_0x18;
    register8_t reserved_0x19;
} NVM_t;

/* NVM Command */
typedef enum NVM_CMD_enum
{
    NVM_CMD_NO_OPERATION_gc = (0x00<<0),  /* Noop/Ordinary LPM */
    NVM_CMD_READ_USER_SIG_ROW_gc = (0x01<<0),  /* Read user signature row */
    NVM_CMD_READ_CALIB_ROW_gc = (0x02<<0),  /* Read calibration row */
    NVM_CMD_READ_EEPROM_gc = (0x06<<0),  /* Read EEPROM */
    NVM_CMD_READ_FUSE_gc = (0x07<<0),  /* Read fuse byte */
    NVM_CMD_WRITE_LOCK_BITS_gc = (0x08<<0),  /* Write lock bits */
    NVM_CMD_ERASE_USER_SIG_ROW_gc = (0x18<<0),  /* Erase user signature row */
    NVM_CMD_WRITE_USER_SIG_ROW_gc = (0x1A<<0),  /* Write user signature row */
    NVM_CMD_ERASE_APP_gc = (0x20<<0),  /* Erase Application Section */
    NVM_CMD_ERASE_APP_PAGE_gc = (0x22<<0),  /* Erase Application Section page */
    NVM_CMD_LOAD_FLASH_BUFFER_gc = (0x23<<0),  /* Load Flash page buffer */
    NVM_CMD_WRITE_APP_PAGE_gc = (0x24<<0),  /* Write Application Section page */
    NVM_CMD_ERASE_WRITE_APP_PAGE_gc = (0x25<<0),  /* Erase-and-write Application Section page */
    NVM_CMD_ERASE_FLASH_BUFFER_gc = (0x26<<0),  /* Erase/flush Flash page buffer */
    NVM_CMD_ERASE_BOOT_PAGE_gc = (0x2A<<0),  /* Erase Boot Section page */
    NVM_CMD_ERASE_FLASH_PAGE_gc = (0x2B<<0),  /* Erase Flash Page */
    NVM_CMD_WRITE_BOOT_PAGE_gc = (0x2C<<0),  /* Write Boot Section page */
    NVM_CMD_ERASE_WRITE_BOOT_PAGE_gc = (0x2D<<0),  /* Erase-and-write Boot Section page */
    NVM_CMD_WRITE_FLASH_PAGE_gc = (0x2E<<0),  /* Write Flash Page */
    NVM_CMD_ERASE_WRITE_FLASH_PAGE_gc = (0x2F<<0),  /* Erase-and-write Flash Page */
    NVM_CMD_ERASE_EEPROM_gc = (0x30<<0),  /* Erase EEPROM */
    NVM_CMD_ERASE_EEPROM_PAGE_gc = (0x32<<0),  /* Erase EEPROM page */
    NVM_CMD_LOAD_EEPROM_BUFFER_gc = (0x33<<0),  /* Load EEPROM page buffer */
    NVM_CMD_WRITE_EEPROM_PAGE_gc = (0x34<<0),  /* Write EEPROM page */
    NVM_CMD_ERASE_WRITE_EEPROM_PAGE_gc = (0x35<<0),  /* Erase-and-write EEPROM page */
    NVM_CMD_ERASE_EEPROM_BUFFER_gc = (0x36<<0),  /* Erase/flush EEPROM page buffer */
    NVM_CMD_APP_CRC_gc = (0x38<<0),  /* Application section CRC */
    NVM_CMD_BOOT_CRC_gc = (0x39<<0),  /*  Boot Section CRC */
    NVM_CMD_FLASH_RANGE_CRC_gc = (0x3A<<0),  /* Flash Range CRC */
    NVM_CMD_CHIP_ERASE_gc = (0x40<<0),  /* Erase Chip */
    NVM_CMD_READ_NVM_gc = (0x43<<0),  /* Read NVM */
    NVM_CMD_WRITE_FUSE_gc = (0x4C<<0),  /* Write Fuse byte */
    NVM_CMD_ERASE_BOOT_gc = (0x68<<0),  /* Erase Boot Section */
    NVM_CMD_FLASH_CRC_gc = (0x78<<0),  /* Flash CRC */
} NVM_CMD_t;

/* SPM ready interrupt level */
typedef enum NVM_SPMLVL_enum
{
    NVM_SPMLVL_OFF_gc = (0x00<<2),  /* Interrupt disabled */
    NVM_SPMLVL_LO_gc = (0x01<<2),  /* Low level */
    NVM_SPMLVL_MED_gc = (0x02<<2),  /* Medium level */
    NVM_SPMLVL_HI_gc = (0x03<<2),  /* High level */
} NVM_SPMLVL_t;

/* EEPROM ready interrupt level */
typedef enum NVM_EELVL_enum
{
    NVM_EELVL_OFF_gc = (0x00<<0),  /* Interrupt disabled */
    NVM_EELVL_LO_gc = (0x01<<0),  /* Low level */
    NVM_EELVL_MED_gc = (0x02<<0),  /* Medium level */
    NVM_EELVL_HI_gc = (0x03<<0),  /* High level */
} NVM_EELVL_t;

/* Boot lock bits - boot setcion */
typedef enum NVM_BLBB_enum
{
    NVM_BLBB_RWLOCK_gc = (0x00<<6),  /* Read and write not allowed */
    NVM_BLBB_RLOCK_gc = (0x01<<6),  /* Read not allowed */
    NVM_BLBB_WLOCK_gc = (0x02<<6),  /* Write not allowed */
    NVM_BLBB_NOLOCK_gc = (0x03<<6),  /* No locks */
} NVM_BLBB_t;

/* Boot lock bits - application section */
typedef enum NVM_BLBA_enum
{
    NVM_BLBA_RWLOCK_gc = (0x00<<4),  /* Read and write not allowed */
    NVM_BLBA_RLOCK_gc = (0x01<<4),  /* Read not allowed */
    NVM_BLBA_WLOCK_gc = (0x02<<4),  /* Write not allowed */
    NVM_BLBA_NOLOCK_gc = (0x03<<4),  /* No locks */
} NVM_BLBA_t;

/* Boot lock bits - application table section */
typedef enum NVM_BLBAT_enum
{
    NVM_BLBAT_RWLOCK_gc = (0x00<<2),  /* Read and write not allowed */
    NVM_BLBAT_RLOCK_gc = (0x01<<2),  /* Read not allowed */
    NVM_BLBAT_WLOCK_gc = (0x02<<2),  /* Write not allowed */
    NVM_BLBAT_NOLOCK_gc = (0x03<<2),  /* No locks */
} NVM_BLBAT_t;

/* Lock bits */
typedef enum NVM_LB_enum
{
    NVM_LB_RWLOCK_gc = (0x00<<0),  /* Read and write not allowed */
    NVM_LB_WLOCK_gc = (0x02<<0),  /* Write not allowed */
    NVM_LB_NOLOCK_gc = (0x03<<0),  /* No locks */
} NVM_LB_t;


/*
--------------------------------------------------------------------------
PMIC - Programmable Multi-level Interrupt Controller
--------------------------------------------------------------------------
*/

/* Programmable Multi-level Interrupt Controller */
typedef struct PMIC_struct
{
    register8_t STATUS;  /* Status Register */
    register8_t INTPRI;  /* Interrupt Priority */
    register8_t CTRL;  /* Control Register */
} PMIC_t;


/*
--------------------------------------------------------------------------
LDO - LDO Regulator
--------------------------------------------------------------------------
*/

/* LDO Regulator */
typedef struct LDO_struct
{
    register8_t STATUS;  /* Status Register */
    register8_t CTRL;  /* Control Register */
} LDO_t;


/*
--------------------------------------------------------------------------
SLEEP - Sleep Controller
--------------------------------------------------------------------------
*/

/* Sleep Controller */
typedef struct SLEEP_struct
{
    register8_t CTRL;  /* Control Register */
} SLEEP_t;

/* Sleep Mode */
typedef enum SLEEP_SMODE_enum
{
    SLEEP_SMODE_IDLE_gc = (0x00<<1),  /* Idle mode */
    SLEEP_SMODE_PSAVE_gc = (0x01<<1),  /* Power-save Mode */
    SLEEP_SMODE_PDOWN_gc = (0x02<<1),  /* Power-down Mode */
    SLEEP_SMODE_POFF_gc = (0x03<<1),  /* Power-off Mode */
} SLEEP_SMODE_t;


/*
--------------------------------------------------------------------------
RST - Reset
--------------------------------------------------------------------------
*/

/* Reset */
typedef struct RST_struct
{
    register8_t STATUS;  /* Status Register */
    register8_t CTRL;  /* Control Register */
} RST_t;


/*
--------------------------------------------------------------------------
USART - Universal Asynchronous Receiver-Transmitter
--------------------------------------------------------------------------
*/

/* Universal Synchronous/Asynchronous Receiver/Transmitter */
typedef struct USART_struct
{
    register8_t DATA;  /* Data Register */
    register8_t STATUS;  /* Status Register */
    register8_t CTRLD;  /* Control Register D */
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t CTRLC;  /* Control Register C */
    register8_t BAUDCTRLA;  /* Baud Rate Control Register A */
    register8_t BAUDCTRLB;  /* Baud Rate Control Register B */
} USART_t;

/* Receive Complete Interrupt level */
typedef enum USART_RXCINTLVL_enum
{
    USART_RXCINTLVL_OFF_gc = (0x00<<4),  /* Interrupt Disabled */
    USART_RXCINTLVL_LO_gc = (0x01<<4),  /* Low Level */
    USART_RXCINTLVL_MED_gc = (0x02<<4),  /* Medium Level */
    USART_RXCINTLVL_HI_gc = (0x03<<4),  /* High Level */
} USART_RXCINTLVL_t;

/* Transmit Complete Interrupt level */
typedef enum USART_TXCINTLVL_enum
{
    USART_TXCINTLVL_OFF_gc = (0x00<<2),  /* Interrupt Disabled */
    USART_TXCINTLVL_LO_gc = (0x01<<2),  /* Low Level */
    USART_TXCINTLVL_MED_gc = (0x02<<2),  /* Medium Level */
    USART_TXCINTLVL_HI_gc = (0x03<<2),  /* High Level */
} USART_TXCINTLVL_t;

/* Data Register Empty Interrupt level */
typedef enum USART_DREINTLVL_enum
{
    USART_DREINTLVL_OFF_gc = (0x00<<0),  /* Interrupt Disabled */
    USART_DREINTLVL_LO_gc = (0x01<<0),  /* Low Level */
    USART_DREINTLVL_MED_gc = (0x02<<0),  /* Medium Level */
    USART_DREINTLVL_HI_gc = (0x03<<0),  /* High Level */
} USART_DREINTLVL_t;

/* Character Size */
typedef enum USART_CHSIZE_enum
{
    USART_CHSIZE_5BIT_gc = (0x00<<0),  /* Character size: 5 bit */
    USART_CHSIZE_6BIT_gc = (0x01<<0),  /* Character size: 6 bit */
    USART_CHSIZE_7BIT_gc = (0x02<<0),  /* Character size: 7 bit */
    USART_CHSIZE_8BIT_gc = (0x03<<0),  /* Character size: 8 bit */
    USART_CHSIZE_9BIT_gc = (0x07<<0),  /* Character size: 9 bit */
} USART_CHSIZE_t;

/* Communication Mode */
typedef enum USART_CMODE_enum
{
    USART_CMODE_ASYNCHRONOUS_gc = (0x00<<6),  /* Asynchronous Mode */
} USART_CMODE_t;

/* Parity Mode */
typedef enum USART_PMODE_enum
{
    USART_PMODE_DISABLED_gc = (0x00<<4),  /* No Parity */
    USART_PMODE_EVEN_gc = (0x02<<4),  /* Even Parity */
    USART_PMODE_ODD_gc = (0x03<<4),  /* Odd Parity */
} USART_PMODE_t;


/*
--------------------------------------------------------------------------
TIM8_16 - Timer 8/16 bit
--------------------------------------------------------------------------
*/

/* Timer 8/16 bit */
typedef struct TIM8_16_struct
{
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t INTCTRL;  /* Interrupt Control Register */
    register8_t STATUS;  /* Status Register */
    register8_t reserved_0x04;
    register8_t reserved_0x05;
    register8_t reserved_0x06;
    register8_t reserved_0x07;
    register8_t COUNTL;  /* Count Register Low Byte */
    register8_t COUNTH;  /* Count Register High Byte */
    register8_t OCRA;  /* Output Compare Register A */
    register8_t OCRB;  /* Output Compare Register B */
    register8_t reserved_0x0C;
    register8_t reserved_0x0D;
    register8_t reserved_0x0E;
    register8_t reserved_0x0F;
} TIM8_16_t;


/*
--------------------------------------------------------------------------
TIMPRESC - Timer Prescaler
--------------------------------------------------------------------------
*/

/* Timer Prescaler */
typedef struct TIMPRESC_struct
{
    register8_t PRESC;  /* Prescaler Register */
} TIMPRESC_t;


/*
--------------------------------------------------------------------------
TWI - Two-Wire Interface
--------------------------------------------------------------------------
*/

/*  */
typedef struct TWI_SLAVE_struct
{
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t STATUS;  /* Status Register */
    register8_t ADDR;  /* Address Register */
    register8_t DATA;  /* Data Register */
    register8_t ADDRMASK;  /* Address Mask Register */
} TWI_SLAVE_t;

/*
--------------------------------------------------------------------------
TWI - Two-Wire Interface
--------------------------------------------------------------------------
*/

/* Two-Wire Interface */
typedef struct TWI_struct
{
    register8_t CTRL;  /* TWI Common Control Register */
    TWI_MASTER_t MASTER;  /* TWI master module */
    TWI_SLAVE_t SLAVE;  /* TWI slave module */
} TWI_t;

/* Slave Interrupt Level */
typedef enum TWI_SLAVE_INTLVL_enum
{
    TWI_SLAVE_INTLVL_OFF_gc = (0x00<<6),  /* Interrupt Disabled */
    TWI_SLAVE_INTLVL_LO_gc = (0x01<<6),  /* Low Level */
    TWI_SLAVE_INTLVL_MED_gc = (0x02<<6),  /* Medium Level */
    TWI_SLAVE_INTLVL_HI_gc = (0x03<<6),  /* High Level */
} TWI_SLAVE_INTLVL_t;

/* Slave Command */
typedef enum TWI_SLAVE_CMD_enum
{
    TWI_SLAVE_CMD_NOACT_gc = (0x00<<0),  /* No Action */
    TWI_SLAVE_CMD_COMPTRANS_gc = (0x02<<0),  /* Used To Complete a Transaction */
    TWI_SLAVE_CMD_RESPONSE_gc = (0x03<<0),  /* Used in Response to Address/Data Interrupt */
} TWI_SLAVE_CMD_t;


/*
--------------------------------------------------------------------------
VADC - Voltage ADC
--------------------------------------------------------------------------
*/

/* Voltage ADC */
typedef struct VADC_struct
{
    register8_t CTRLA;  /* Control Register A */
    register8_t CTRLB;  /* Control Register B */
    register8_t DIAGNOSIS;  /* Diagnosis Register */
    register8_t SCANSETUP;  /* Scan Setup Register */
    register8_t PROTSETUPA;  /* Protection Register A */
    register8_t PROTSETUPB;  /* Protection Register B */
    register8_t STATUS;  /* Status Register */
    register8_t reserved_0x07;
    register8_t reserved_0x08;
    register8_t reserved_0x09;
    register8_t reserved_0x0A;
    register8_t reserved_0x0B;
    register8_t reserved_0x0C;
    register8_t reserved_0x0D;
    register8_t reserved_0x0E;
    register8_t reserved_0x0F;
    _WORDREGISTER(BATTRES);  /* BATT-GND Conversion Result */
    _WORDREGISTER(VTEMPRES);  /* VTEMP-GND Conversion Result */
    _WORDREGISTER(ADC0RES);  /* ADC0-SGND Conversion Result */
    _WORDREGISTER(ADC1RES);  /* ADC1-SGND Conversion Result */
    _WORDREGISTER(PVNVRES);  /* PV-NV Conversion Result */
    register8_t reserved_0x1A;
    register8_t reserved_0x1B;
    register8_t reserved_0x1C;
    register8_t reserved_0x1D;
    register8_t reserved_0x1E;
    register8_t reserved_0x1F;
    register8_t reserved_0x20;
    register8_t reserved_0x21;
    register8_t reserved_0x22;
    register8_t reserved_0x23;
    register8_t reserved_0x24;
    register8_t reserved_0x25;
    register8_t reserved_0x26;
    register8_t reserved_0x27;
    register8_t reserved_0x28;
    register8_t reserved_0x29;
    register8_t reserved_0x2A;
    register8_t reserved_0x2B;
    register8_t reserved_0x2C;
    register8_t reserved_0x2D;
    register8_t reserved_0x2E;
    register8_t reserved_0x2F;
    register8_t reserved_0x30;
    register8_t reserved_0x31;
    register8_t reserved_0x32;
    register8_t reserved_0x33;
    register8_t reserved_0x34;
    register8_t reserved_0x35;
    register8_t reserved_0x36;
    register8_t reserved_0x37;
    register8_t reserved_0x38;
    register8_t reserved_0x39;
    register8_t reserved_0x3A;
    register8_t reserved_0x3B;
    register8_t reserved_0x3C;
    register8_t reserved_0x3D;
    register8_t reserved_0x3E;
    register8_t reserved_0x3F;
    _WORDREGISTER(VTEMPTH);  /* Internal Temperature Sensor High Threshold Level */
    _WORDREGISTER(VTEMPTL);  /* Internal Temperature Sensor Low Threshold Level */
    _WORDREGISTER(ADC0HT);  /* ADC0-SGND High Threshold Level */
    _WORDREGISTER(ADC0LT);  /* ADC0-SGND Low Threshold Level */
    _WORDREGISTER(ADC1HT);  /* ADC1-SGND High Threshold Level */
    _WORDREGISTER(ADC1LT);  /* ADC1-SGND Low Threshold Level */
    _WORDREGISTER(PVNVHT);  /* PV-NV High Threshold Level */
    _WORDREGISTER(PVNVLT);  /* PV-NV Low Threshold Level */
} VADC_t;

/* Decimation Ratio */
typedef enum VADC_DECIR_enum
{
    VADC_DECIR_DECIR_512x_gc = (0x00<<0),  /* 512x decimation */
    VADC_DECIR_DECIR_256x_gc = (0x01<<0),  /* 256x decimation */
    VADC_DECIR_DECIR_128x_gc = (0x02<<0),  /* 128x decimation */
} VADC_DECIR_t;

/* PV Diagnosis Configuration */
typedef enum VADC_PVDIAG_enum
{
    VADC_PVDIAG_PVDIAG_OFF_gc = (0x00<<2),  /* No diagnosis enabled */
    VADC_PVDIAG_PVDIAG_PD_gc = (0x01<<2),  /* Pull down enabled */
} VADC_PVDIAG_t;

/* NV Diagnosis Configuration */
typedef enum VADC_NVDIAG_enum
{
    VADC_NVDIAG_NVDIAG_OFF_gc = (0x00<<0),  /* No diagnosis enabled */
    VADC_NVDIAG_NVDIAG_PU_gc = (0x02<<0),  /* Pull-up enabled */
} VADC_NVDIAG_t;


/*
--------------------------------------------------------------------------
WDT - Watch-Dog Timer
--------------------------------------------------------------------------
*/

/* Watch-Dog Timer */
typedef struct WDT_struct
{
    register8_t CTRL;  /* Control */
    register8_t WINCTRL;  /* Windowed Mode Control */
    register8_t STATUS;  /* Status */
} WDT_t;

/* Period setting */
typedef enum WDT_PER_enum
{
    WDT_PER_8CLK_gc = (0x00<<2),  /* 8 cycles (8ms) */
    WDT_PER_16CLK_gc = (0x01<<2),  /* 16 cycles (16ms) */
    WDT_PER_32CLK_gc = (0x02<<2),  /* 32 cycles (32ms) */
    WDT_PER_64CLK_gc = (0x03<<2),  /* 64 cycles (64ms) */
    WDT_PER_125CLK_gc = (0x04<<2),  /* 125 cycles (0.125s) */
    WDT_PER_250CLK_gc = (0x05<<2),  /* 250 cycles (0.25s) */
    WDT_PER_500CLK_gc = (0x06<<2),  /* 500 cycles (0.5s) */
    WDT_PER_1KCLK_gc = (0x07<<2),  /* 1K cycles (1s) */
    WDT_PER_2KCLK_gc = (0x08<<2),  /* 2K cycles (2s) */
    WDT_PER_4KCLK_gc = (0x09<<2),  /* 4K cycles (4s) */
    WDT_PER_8KCLK_gc = (0x0A<<2),  /* 8K cycles (8s) */
} WDT_PER_t;

/* Closed window period */
typedef enum WDT_WPER_enum
{
    WDT_WPER_8CLK_gc = (0x00<<2),  /* 8 cycles (8ms ) */
    WDT_WPER_16CLK_gc = (0x01<<2),  /* 16 cycles (16ms ) */
    WDT_WPER_32CLK_gc = (0x02<<2),  /* 32 cycles (32ms ) */
    WDT_WPER_64CLK_gc = (0x03<<2),  /* 64 cycles (64ms ) */
    WDT_WPER_125CLK_gc = (0x04<<2),  /* 125 cycles (0.125s ) */
    WDT_WPER_250CLK_gc = (0x05<<2),  /* 250 cycles (0.25s ) */
    WDT_WPER_500CLK_gc = (0x06<<2),  /* 500 cycles (0.5s ) */
    WDT_WPER_1KCLK_gc = (0x07<<2),  /* 1K cycles (1s ) */
    WDT_WPER_2KCLK_gc = (0x08<<2),  /* 2K cycles (2s ) */
    WDT_WPER_4KCLK_gc = (0x09<<2),  /* 4K cycles (4s ) */
    WDT_WPER_8KCLK_gc = (0x0A<<2),  /* 8K cycles (8s ) */
} WDT_WPER_t;


/*
--------------------------------------------------------------------------
XMEGAX1_FUSES - Fuses and Lockbits
--------------------------------------------------------------------------
*/

/* Lock Bits */
typedef struct NVM_LOCKBITS_struct
{
    register8_t LOCKBITS;  /* Lock Bits */
} NVM_LOCKBITS_t;

/*
--------------------------------------------------------------------------
XMEGAX1_FUSES - Fuses and Lockbits
--------------------------------------------------------------------------
*/

/* Fuses */
typedef struct NVM_FUSES_struct
{
    register8_t FUSEBYTE0;  /* NVM fuse */
    register8_t FUSEBYTE1;  /* Watchdog Configuration */
    register8_t FUSEBYTE2;  /* Reset Configuration */
    register8_t reserved_0x03;
    register8_t FUSEBYTE4;  /* Start-up Configuration */
    register8_t FUSEBYTE5;  /* Non-Volatile Memory Fuse Byte 5 */
} NVM_FUSES_t;

/* Boot Loader Section Reset Vector */
typedef enum BOOTRST_enum
{
    BOOTRST_BOOTLDR_gc = (0x00<<6),  /* Boot Loader Reset */
    BOOTRST_APPLICATION_gc = (0x01<<6),  /* Application Reset */
} BOOTRST_t;

/* Watchdog (Window) Timeout Period */
typedef enum WD_enum
{
    WD_8CLK_gc = (0x00<<4),  /* 8 cycles (8ms ) */
    WD_16CLK_gc = (0x01<<4),  /* 16 cycles (16ms ) */
    WD_32CLK_gc = (0x02<<4),  /* 32 cycles (32ms ) */
    WD_64CLK_gc = (0x03<<4),  /* 64 cycles (64ms ) */
    WD_128CLK_gc = (0x04<<4),  /* 128 cycles (0.125s ) */
    WD_256CLK_gc = (0x05<<4),  /* 256 cycles (0.25s ) */
    WD_512CLK_gc = (0x06<<4),  /* 512 cycles (0.5s ) */
    WD_1KCLK_gc = (0x07<<4),  /* 1K cycles (1s ) */
    WD_2KCLK_gc = (0x08<<4),  /* 2K cycles (2s ) */
    WD_4KCLK_gc = (0x09<<4),  /* 4K cycles (4s ) */
    WD_8KCLK_gc = (0x0A<<4),  /* 8K cycles (8s ) */
} WD_t;

/* Start-up Time */
typedef enum SUT_enum
{
    SUT_4MS_gc = (0x02<<2),  /* 4 ms */
    SUT_64MS_gc = (0x01<<2),  /* 64 ms */
    SUT_512MS_gc = (0x00<<2),  /* 512 ms */
} SUT_t;

/* Autoload Configuration */
typedef enum AUTOLOAD_enum
{
    AUTOLOAD_BOOTSEL_gc = (0x01<<3),  /* Use Boot For User Configuration */
    AUTOLOAD_APPSEL_gc = (0x00<<3),  /* Use Application For User Configuration */
} AUTOLOAD_t;


/*
--------------------------------------------------------------------------
XMEGAX1_SIGROW - Signature Row
--------------------------------------------------------------------------
*/

/* Production Signatures */
typedef struct NVM_PROD_SIGNATURES_struct
{
    register8_t reserved_0x00;
    register8_t reserved_0x01;
    register8_t reserved_0x02;
    register8_t reserved_0x03;
    register8_t reserved_0x04;
    register8_t reserved_0x05;
    register8_t reserved_0x06;
    register8_t reserved_0x07;
    register8_t LOTNUM0;  /* Lot Number Byte 0, ASCII */
    register8_t LOTNUM1;  /* Lot Number Byte 1, ASCII */
    register8_t LOTNUM2;  /* Lot Number Byte 2, ASCII */
    register8_t LOTNUM3;  /* Lot Number Byte 3, ASCII */
    register8_t LOTNUM4;  /* Lot Number Byte 4, ASCII */
    register8_t LOTNUM5;  /* Lot Number Byte 5, ASCII */
    register8_t reserved_0x0E;
    register8_t reserved_0x0F;
    register8_t WAFNUM;  /* Wafer Number */
    register8_t reserved_0x11;
    register8_t COORDX0;  /* Wafer Coordinate X Byte 0 */
    register8_t COORDX1;  /* Wafer Coordinate X Byte 1 */
    register8_t COORDY0;  /* Wafer Coordinate Y Byte 0 */
    register8_t COORDY1;  /* Wafer Coordinate Y Byte 1 */
    register8_t VADCOFFCELL0;  /* VADC OFFSET CELL0 */
    register8_t VADCOFFADC0;  /* VADC OFFSET ADC0 */
    register8_t VADCOFFADC1;  /* VADC OFFSET ADC1 */
    register8_t VADCOFFBATT;  /* VADC OFFSET BATT */
    register8_t VADCGAINCELL0H;  /* VADC GAIN CELL0 High Byte */
    register8_t VADCGAINCELL0L;  /* VADC GAIN CELL0 Low Byte */
    register8_t VADCGAINADC0H;  /* VADC GAIN ADC0 High Byte */
    register8_t VADCGAINADC0L;  /* VADC GAIN ADC0 Low Byte */
    register8_t VADCGAINADC1H;  /* VADC GAIN ADC1 High Byte */
    register8_t VADCGAINADC1L;  /* VADC GAIN ADC1 Low Byte */
    register8_t VADCGAINBATTH;  /* VADC GAIN BATT High Byte */
    register8_t VADCGAINBATTL;  /* VADC GAIN BATT Low Byte */
    register8_t VADCGAINBATTH1;  /* VADC GAIN BATT High Byte 1 */
    register8_t VADCGAINBATTL1;  /* VADC GAIN BATT Low Byte 2 */
    register8_t VADCCALVTEMPH;  /* VADC CALIBRATION VTEMP High Byte */
    register8_t VADCCALVTEMPL;  /* VADC CALIBRATION VTEMP Low Byte */
} NVM_PROD_SIGNATURES_t;


/*
--------------------------------------------------------------------------
XOCD - On-Chip Debug System
--------------------------------------------------------------------------
*/

/* On-Chip Debug System */
typedef struct OCD_struct
{
    register8_t OCDR0;  /* OCD Register 0 */
    register8_t OCDR1;  /* OCD Register 1 */
} OCD_t;


/*
--------------------------------------------------------------------------
PORT - I/O Port Configuration
--------------------------------------------------------------------------
*/

/* I/O Ports */
typedef struct PORT_struct
{
    register8_t DIR;  /* I/O Port Data Direction */
    register8_t reserved_0x01;
    register8_t reserved_0x02;
    register8_t reserved_0x03;
    register8_t OUT;  /* I/O Port Output */
    register8_t reserved_0x05;
    register8_t reserved_0x06;
    register8_t reserved_0x07;
    register8_t IN;  /* I/O port Input */
    register8_t INTCTRL;  /* Interrupt Control Register */
    register8_t INT0MASK;  /* Port Interrupt 0 Mask */
    register8_t INT1MASK;  /* Port Interrupt 1 Mask */
    register8_t INTFLAGS;  /* Interrupt Flag Register */
    register8_t reserved_0x0D;
    register8_t reserved_0x0E;
    register8_t reserved_0x0F;
    register8_t PIN0CTRL;  /* Pin 0 Control Register */
    register8_t PIN1CTRL;  /* Pin 1 Control Register */
    register8_t PIN2CTRL;  /* Pin 2 Control Register */
    register8_t reserved_0x13;
    register8_t reserved_0x14;
    register8_t reserved_0x15;
    register8_t reserved_0x16;
    register8_t reserved_0x17;
} PORT_t;

/* Port Interrupt 0 Level */
typedef enum PORT_INT0LVL_enum
{
    PORT_INT0LVL_OFF_gc = (0x00<<0),  /* Interrupt Disabled */
    PORT_INT0LVL_LO_gc = (0x01<<0),  /* Low Level */
    PORT_INT0LVL_MED_gc = (0x02<<0),  /* Medium Level */
    PORT_INT0LVL_HI_gc = (0x03<<0),  /* High Level */
} PORT_INT0LVL_t;

/* Port Interrupt 1 Level */
typedef enum PORT_INT1LVL_enum
{
    PORT_INT1LVL_OFF_gc = (0x00<<2),  /* Interrupt Disabled */
    PORT_INT1LVL_LO_gc = (0x01<<2),  /* Low Level */
    PORT_INT1LVL_MED_gc = (0x02<<2),  /* Medium Level */
    PORT_INT1LVL_HI_gc = (0x03<<2),  /* High Level */
} PORT_INT1LVL_t;

/* Output/Pull Configuration */
typedef enum PORT_OPC_enum
{
    PORT_OPC_TOTEM_gc = (0x00<<3),  /* Totempole */
    PORT_OPC_BUSKEEPER_gc = (0x01<<3),  /* Totempole w/ Bus keeper on Input and Output */
    PORT_OPC_PULLDOWN_gc = (0x02<<3),  /* Totempole w/ Pull-down on Input */
    PORT_OPC_PULLUP_gc = (0x03<<3),  /* Totempole w/ Pull-up on Input */
    PORT_OPC_WIREDOR_gc = (0x04<<3),  /* Wired OR */
    PORT_OPC_WIREDAND_gc = (0x05<<3),  /* Wired AND */
    PORT_OPC_WIREDORPULL_gc = (0x06<<3),  /* Wired OR w/ Pull-down */
    PORT_OPC_WIREDANDPULL_gc = (0x07<<3),  /* Wired AND w/ Pull-up */
} PORT_OPC_t;

/* Input/Sense Configuration */
typedef enum PORT_ISC_enum
{
    PORT_ISC_BOTHEDGES_gc = (0x00<<0),  /* Sense Both Edges */
    PORT_ISC_RISING_gc = (0x01<<0),  /* Sense Rising Edge */
    PORT_ISC_FALLING_gc = (0x02<<0),  /* Sense Falling Edge */
    PORT_ISC_LEVEL_gc = (0x03<<0),  /* Sense Level (Transparent For Events) */
    PORT_ISC_INPUT_DISABLE_gc = (0x07<<0),  /* Disable Digital Input Buffer */
} PORT_ISC_t;


/*
--------------------------------------------------------------------------
VPORT - Virtual Ports
--------------------------------------------------------------------------
*/

/* Virtual Port */
typedef struct VPORT_struct
{
    register8_t DIR;  /* I/O Port Data Direction */
    register8_t OUT;  /* I/O Port Output */
    register8_t IN;  /* I/O Port Input */
    register8_t INTFLAGS;  /* Interrupt Flag Register */
} VPORT_t;



/*
==========================================================================
IO Module Instances. Mapped to memory.
==========================================================================
*/

#define VPORT0    (*(VPORT_t *) 0x0010)  /* Virtual Port 0 */
#define VPORT2    (*(VPORT_t *) 0x0018)  /* Virtual Port 2 */
#define OCD    (*(OCD_t *) 0x002E)  /* On-Chip Debug System */
#define CLK    (*(CLK_t *) 0x0040)  /* Clock System */
#define RST    (*(RST_t *) 0x0044)  /* Reset Controller */
#define SLEEP    (*(SLEEP_t *) 0x0048)  /* Sleep Controller */
#define COMTR    (*(COMTR_t *) 0x0078)  /* Communication Timeout Reset */
#define WDT    (*(WDT_t *) 0x0080)  /* Watch-Dog Timer */
#define PMIC    (*(PMIC_t *) 0x00A0)  /* Programmable Interrupt Controller */
#define CRC    (*(CRC_t *) 0x00D0)  /* CRC Module */
#define NVM    (*(NVM_t *) 0x01C0)  /* Non Volatile Memory Controller */
#define VADC    (*(VADC_t *) 0x0200)  /* Voltage ADC */
#define CADC    (*(CADC_t *) 0x0280)  /* Current ADC */
#define VREF    (*(VREF_t *) 0x02A0)  /* Voltage Reference */
#define LDO    (*(LDO_t *) 0x02A8)  /* LDO Regulator */
#define CPROT    (*(CPROT_t *) 0x02AC)  /* DAC-Based Current Protection */
#define FET    (*(FET_t *) 0x02BC)  /* FET controller */
#define TIMER    (*(TIM8_16_t *) 0x0400)  /* Timer 8/16 bit */
#define TIMPRESC    (*(TIMPRESC_t *) 0x0420)  /* Timer Prescaler */
#define WUT    (*(_t *) 0x0440)  /* Wake-up Timer */
#define TWIC    (*(TWI_t *) 0x0480)  /* Two-Wire Interface C */
#define BCD    (*(BCD_t *) 0x0500)  /* Bus Connect/Disconnect */
#define PORTA    (*(PORT_t *) 0x0600)  /* Port A */
#define PORTC    (*(PORT_t *) 0x0640)  /* Port C */
#define USARTC    (*(USART_t *) 0x08A0)  /* Universal Asynchronous Receiver-Transmitter C */


#endif /* !defined (__ASSEMBLER__) */


/* ========== Flattened fully qualified IO register names ========== */

/* GPIO - General Purpose IO Registers */
#define GPIO_GPIOR0  _SFR_MEM8(0x0000)
#define GPIO_GPIOR1  _SFR_MEM8(0x0001)
#define GPIO_GPIOR2  _SFR_MEM8(0x0002)
#define GPIO_GPIOR3  _SFR_MEM8(0x0003)

/* VPORT0 - Virtual Port 0 */
#define VPORT0_DIR  _SFR_MEM8(0x0010)
#define VPORT0_OUT  _SFR_MEM8(0x0011)
#define VPORT0_IN  _SFR_MEM8(0x0012)
#define VPORT0_INTFLAGS  _SFR_MEM8(0x0013)

/* VPORT2 - Virtual Port 2 */
#define VPORT2_DIR  _SFR_MEM8(0x0018)
#define VPORT2_OUT  _SFR_MEM8(0x0019)
#define VPORT2_IN  _SFR_MEM8(0x001A)
#define VPORT2_INTFLAGS  _SFR_MEM8(0x001B)

/* OCD - On-Chip Debug System */
#define OCD_OCDR0  _SFR_MEM8(0x002E)
#define OCD_OCDR1  _SFR_MEM8(0x002F)

/* CPU - CPU Registers */
#define CPU_CCP  _SFR_MEM8(0x0034)
#define CPU_SPL  _SFR_MEM8(0x003D)
#define CPU_SPH  _SFR_MEM8(0x003E)
#define CPU_SREG  _SFR_MEM8(0x003F)

/* CLK - Clock System */
#define CLK_CLKPSR  _SFR_MEM8(0x0040)
#define CLK_CLKSLR  _SFR_MEM8(0x0041)

/* RST - Reset Controller */
#define RST_STATUS  _SFR_MEM8(0x0044)
#define RST_CTRL  _SFR_MEM8(0x0045)

/* SLEEP - Sleep Controller */
#define SLEEP_CTRL  _SFR_MEM8(0x0048)

/* COMTR - Communication Timeout Reset */
#define COMTR_CTRL  _SFR_MEM8(0x0078)

/* WDT - Watch-Dog Timer */
#define WDT_CTRL  _SFR_MEM8(0x0080)
#define WDT_WINCTRL  _SFR_MEM8(0x0081)
#define WDT_STATUS  _SFR_MEM8(0x0082)

/* PMIC - Programmable Interrupt Controller */
#define PMIC_STATUS  _SFR_MEM8(0x00A0)
#define PMIC_INTPRI  _SFR_MEM8(0x00A1)
#define PMIC_CTRL  _SFR_MEM8(0x00A2)

/* CRC - CRC Module */
#define CRC_CTRL  _SFR_MEM8(0x00D0)
#define CRC_STATUS  _SFR_MEM8(0x00D1)
#define CRC_DATAIN  _SFR_MEM8(0x00D3)
#define CRC_CHECKSUM0  _SFR_MEM8(0x00D4)
#define CRC_CHECKSUM1  _SFR_MEM8(0x00D5)
#define CRC_CHECKSUM2  _SFR_MEM8(0x00D6)
#define CRC_CHECKSUM3  _SFR_MEM8(0x00D7)

/* NVM - Non Volatile Memory Controller */
#define NVM_ADDR0  _SFR_MEM8(0x01C0)
#define NVM_ADDR1  _SFR_MEM8(0x01C1)
#define NVM_ADDR2  _SFR_MEM8(0x01C2)
#define NVM_DATA0  _SFR_MEM8(0x01C4)
#define NVM_DATA1  _SFR_MEM8(0x01C5)
#define NVM_DATA2  _SFR_MEM8(0x01C6)
#define NVM_CMD  _SFR_MEM8(0x01CA)
#define NVM_CTRLA  _SFR_MEM8(0x01CB)
#define NVM_CTRLB  _SFR_MEM8(0x01CC)
#define NVM_INTCTRL  _SFR_MEM8(0x01CD)
#define NVM_STATUS  _SFR_MEM8(0x01CF)
#define NVM_LOCKBITS  _SFR_MEM8(0x01D0)

/* VADC - Voltage ADC */
#define VADC_CTRLA  _SFR_MEM8(0x0200)
#define VADC_CTRLB  _SFR_MEM8(0x0201)
#define VADC_DIAGNOSIS  _SFR_MEM8(0x0202)
#define VADC_SCANSETUP  _SFR_MEM8(0x0203)
#define VADC_PROTSETUPA  _SFR_MEM8(0x0204)
#define VADC_PROTSETUPB  _SFR_MEM8(0x0205)
#define VADC_STATUS  _SFR_MEM8(0x0206)
#define VADC_BATTRES  _SFR_MEM16(0x0210)
#define VADC_VTEMPRES  _SFR_MEM16(0x0212)
#define VADC_ADC0RES  _SFR_MEM16(0x0214)
#define VADC_ADC1RES  _SFR_MEM16(0x0216)
#define VADC_PVNVRES  _SFR_MEM16(0x0218)
#define VADC_VTEMPTH  _SFR_MEM16(0x0240)
#define VADC_VTEMPTL  _SFR_MEM16(0x0242)
#define VADC_ADC0HT  _SFR_MEM16(0x0244)
#define VADC_ADC0LT  _SFR_MEM16(0x0246)
#define VADC_ADC1HT  _SFR_MEM16(0x0248)
#define VADC_ADC1LT  _SFR_MEM16(0x024A)
#define VADC_PVNVHT  _SFR_MEM16(0x024C)
#define VADC_PVNVLT  _SFR_MEM16(0x024E)

/* CADC - Current ADC */
#define CADC_CTRLA  _SFR_MEM8(0x0280)
#define CADC_CTRLB  _SFR_MEM8(0x0281)
#define CADC_CTRLC  _SFR_MEM8(0x0282)
#define CADC_INTCTRL  _SFR_MEM8(0x0283)
#define CADC_DIAGNOSIS  _SFR_MEM8(0x0284)
#define CADC_STATUS  _SFR_MEM8(0x0285)
#define CADC_PROTECTION  _SFR_MEM8(0x0286)
#define CADC_INST  _SFR_MEM16(0x0287)
#define CADC_I2INST  _SFR_MEM16(0x028D)
#define CADC_I2ACC  _SFR_MEM16(0x028F)
#define CADC_PHTHRES  _SFR_MEM16(0x0291)
#define CADC_PLTHRES  _SFR_MEM16(0x0293)
#define CADC_SHTHRES  _SFR_MEM16(0x0295)
#define CADC_SLTHRES  _SFR_MEM16(0x0297)

/* VREF - Voltage Reference */
#define VREF_CTRLA  _SFR_MEM8(0x02A0)
#define VREF_CTRLB  _SFR_MEM8(0x02A1)
#define VREF_STATUS  _SFR_MEM8(0x02A2)

/* LDO - LDO Regulator */
#define LDO_STATUS  _SFR_MEM8(0x02A8)
#define LDO_CTRL  _SFR_MEM8(0x02A9)

/* CPROT - DAC-Based Current Protection */
#define CPROT_CTRL  _SFR_MEM8(0x02AC)
#define CPROT_SCTIMING  _SFR_MEM8(0x02AD)
#define CPROT_OCTIMING  _SFR_MEM8(0x02AE)
#define CPROT_SCLEVEL  _SFR_MEM8(0x02AF)
#define CPROT_DOCLEVEL  _SFR_MEM8(0x02B0)
#define CPROT_COCLEVEL  _SFR_MEM8(0x02B1)
#define CPROT_OCREG  _SFR_MEM8(0x02B2)

/* FET - FET controller */
#define FET_CTRL  _SFR_MEM8(0x02BC)
#define FET_FETACTIONA  _SFR_MEM8(0x02BD)
#define FET_FETACTIONE  _SFR_MEM8(0x02C1)
#define FET_FETACTIONF  _SFR_MEM8(0x02C2)
#define FET_FETACTIONG  _SFR_MEM8(0x02C3)
#define FET_INTMASKC  _SFR_MEM8(0x02CC)
#define FET_INTMASKD  _SFR_MEM8(0x02CD)
#define FET_STATUSA  _SFR_MEM8(0x02D0)
#define FET_STATUSC  _SFR_MEM8(0x02D2)
#define FET_STATUSD  _SFR_MEM8(0x02D3)

/* TIMER - Timer 8/16 bit */
#define TIMER_CTRLA  _SFR_MEM8(0x0400)
#define TIMER_CTRLB  _SFR_MEM8(0x0401)
#define TIMER_INTCTRL  _SFR_MEM8(0x0402)
#define TIMER_STATUS  _SFR_MEM8(0x0403)
#define TIMER_COUNTL  _SFR_MEM8(0x0408)
#define TIMER_COUNTH  _SFR_MEM8(0x0409)
#define TIMER_OCRA  _SFR_MEM8(0x040A)
#define TIMER_OCRB  _SFR_MEM8(0x040B)

/* TIMPRESC - Timer Prescaler */
#define TIMPRESC_PRESC  _SFR_MEM8(0x0420)

/* WUT - Wake-up Timer */

/* TWIC - Two-Wire Interface C */
#define TWIC_CTRL  _SFR_MEM8(0x0480)
#define TWIC_SLAVE_CTRLA  _SFR_MEM8(0x0488)
#define TWIC_SLAVE_CTRLB  _SFR_MEM8(0x0489)
#define TWIC_SLAVE_STATUS  _SFR_MEM8(0x048A)
#define TWIC_SLAVE_ADDR  _SFR_MEM8(0x048B)
#define TWIC_SLAVE_DATA  _SFR_MEM8(0x048C)
#define TWIC_SLAVE_ADDRMASK  _SFR_MEM8(0x048D)

/* BCD - Bus Connect/Disconnect */
#define BCD_CTRL  _SFR_MEM8(0x0500)
#define BCD_STATUS  _SFR_MEM8(0x0501)

/* PORTA - Port A */
#define PORTA_DIR  _SFR_MEM8(0x0600)
#define PORTA_OUT  _SFR_MEM8(0x0604)
#define PORTA_IN  _SFR_MEM8(0x0608)
#define PORTA_INTCTRL  _SFR_MEM8(0x0609)
#define PORTA_INT0MASK  _SFR_MEM8(0x060A)
#define PORTA_INT1MASK  _SFR_MEM8(0x060B)
#define PORTA_INTFLAGS  _SFR_MEM8(0x060C)
#define PORTA_PIN0CTRL  _SFR_MEM8(0x0610)
#define PORTA_PIN1CTRL  _SFR_MEM8(0x0611)
#define PORTA_PIN2CTRL  _SFR_MEM8(0x0612)

/* PORTC - Port C */
#define PORTC_DIR  _SFR_MEM8(0x0640)
#define PORTC_OUT  _SFR_MEM8(0x0644)
#define PORTC_IN  _SFR_MEM8(0x0648)
#define PORTC_INTCTRL  _SFR_MEM8(0x0649)
#define PORTC_INT0MASK  _SFR_MEM8(0x064A)
#define PORTC_INT1MASK  _SFR_MEM8(0x064B)
#define PORTC_INTFLAGS  _SFR_MEM8(0x064C)
#define PORTC_PIN0CTRL  _SFR_MEM8(0x0650)
#define PORTC_PIN1CTRL  _SFR_MEM8(0x0651)
#define PORTC_PIN2CTRL  _SFR_MEM8(0x0652)

/* USARTC - Universal Asynchronous Receiver-Transmitter C */
#define USARTC_DATA  _SFR_MEM8(0x08A0)
#define USARTC_STATUS  _SFR_MEM8(0x08A1)
#define USARTC_CTRLD  _SFR_MEM8(0x08A2)
#define USARTC_CTRLA  _SFR_MEM8(0x08A3)
#define USARTC_CTRLB  _SFR_MEM8(0x08A4)
#define USARTC_CTRLC  _SFR_MEM8(0x08A5)
#define USARTC_BAUDCTRLA  _SFR_MEM8(0x08A6)
#define USARTC_BAUDCTRLB  _SFR_MEM8(0x08A7)



/*================== Bitfield Definitions ================== */

/* BCD - Bus Connect/Disconnect on Communication Lines */
/* BCD.CTRL  bit masks and bit positions */
#define BCD_INTPOL_gm  0xC0  /* Interrupt Polarity group mask. */
#define BCD_INTPOL_gp  6  /* Interrupt Polarity group position. */
#define BCD_INTPOL0_bm  (1<<6)  /* Interrupt Polarity bit 0 mask. */
#define BCD_INTPOL0_bp  6  /* Interrupt Polarity bit 0 position. */
#define BCD_INTPOL1_bm  (1<<7)  /* Interrupt Polarity bit 1 mask. */
#define BCD_INTPOL1_bp  7  /* Interrupt Polarity bit 1 position. */

#define BCD_INTLVL_bm  0x20  /* Interrupt Level bit mask. */
#define BCD_INTLVL_bp  5  /* Interrupt Level bit position. */

#define BCD_TIMEOUT_bm  0x10  /* Timeout bit mask. */
#define BCD_TIMEOUT_bp  4  /* Timeout bit position. */

#define BCD_MODE_bm  0x10  /* Timeout bit mask. */
#define BCD_MODE_bp  4  /* Timeout bit position. */


/* BCD.STATUS  bit masks and bit positions */
#define BCD_CDIF_bm  0x01  /* Connect/Disconnect Interrupt Flag bit mask. */
#define BCD_CDIF_bp  0  /* Connect/Disconnect Interrupt Flag bit position. */


/* VREF - Voltage Reference */
/* VREF.CTRLA  bit masks and bit positions */
#define VREF_SDE_bm  0x04  /* Short Detector Enabled bit mask. */
#define VREF_SDE_bp  2  /* Short Detector Enabled bit position. */

#define VREF_SDINTLVL_gm  0x03  /* Short Detector Interrupt Level group mask. */
#define VREF_SDINTLVL_gp  0  /* Short Detector Interrupt Level group position. */
#define VREF_SDINTLVL0_bm  (1<<0)  /* Short Detector Interrupt Level bit 0 mask. */
#define VREF_SDINTLVL0_bp  0  /* Short Detector Interrupt Level bit 0 position. */
#define VREF_SDINTLVL1_bm  (1<<1)  /* Short Detector Interrupt Level bit 1 mask. */
#define VREF_SDINTLVL1_bp  1  /* Short Detector Interrupt Level bit 1 position. */


/* VREF.CTRLB  bit masks and bit positions */
#define VREF_VREFCFG_gm  0x03  /* VREF Configuration group mask. */
#define VREF_VREFCFG_gp  0  /* VREF Configuration group position. */
#define VREF_VREFCFG0_bm  (1<<0)  /* VREF Configuration bit 0 mask. */
#define VREF_VREFCFG0_bp  0  /* VREF Configuration bit 0 position. */
#define VREF_VREFCFG1_bm  (1<<1)  /* VREF Configuration bit 1 mask. */
#define VREF_VREFCFG1_bp  1  /* VREF Configuration bit 1 position. */


/* VREF.STATUS  bit masks and bit positions */
#define VREF_SDS_bm  0x02  /* Short Detector Status bit mask. */
#define VREF_SDS_bp  1  /* Short Detector Status bit position. */

#define VREF_SDIF_bm  0x01  /* Short Detector Interrupt Flag bit mask. */
#define VREF_SDIF_bp  0  /* Short Detector Interrupt Flag bit position. */


/* CADC - Current ADC */
/* CADC.CTRLA  bit masks and bit positions */
#define CADC_POLARITY_bm  0x02  /* Polarity Select bit mask. */
#define CADC_POLARITY_bp  1  /* Polarity Select bit position. */

#define CADC_ENABLE_bm  0x01  /* Enable bit mask. */
#define CADC_ENABLE_bp  0  /* Enable bit position. */


/* CADC.CTRLB  bit masks and bit positions */
#define CADC_CLKS_bm  0x40  /* Clock Select bit mask. */
#define CADC_CLKS_bp  6  /* Clock Select bit position. */

#define CADC_ACHEN_bm  0x20  /* Automatic Chopping Enable bit mask. */
#define CADC_ACHEN_bp  5  /* Automatic Chopping Enable bit position. */

#define CADC_ICDEC_bm  0x08  /* Instantaneous Decimation Ratio bit mask. */
#define CADC_ICDEC_bp  3  /* Instantaneous Decimation Ratio bit position. */

#define CADC_ACDEC_gm  0x03  /* Accumulated Decimation Ratio group mask. */
#define CADC_ACDEC_gp  0  /* Accumulated Decimation Ratio group position. */
#define CADC_ACDEC0_bm  (1<<0)  /* Accumulated Decimation Ratio bit 0 mask. */
#define CADC_ACDEC0_bp  0  /* Accumulated Decimation Ratio bit 0 position. */
#define CADC_ACDEC1_bm  (1<<1)  /* Accumulated Decimation Ratio bit 1 mask. */
#define CADC_ACDEC1_bp  1  /* Accumulated Decimation Ratio bit 1 position. */


/* CADC.CTRLC  bit masks and bit positions */
#define CADC_PSES_bm  0x10  /* Polarity Synchronization Edge Select bit mask. */
#define CADC_PSES_bp  4  /* Polarity Synchronization Edge Select bit position. */

#define CADC_SME_bm  0x08  /* Sampling Mode Enable bit mask. */
#define CADC_SME_bp  3  /* Sampling Mode Enable bit position. */

#define CADC_SAI_gm  0x03  /* Sampling Interval group mask. */
#define CADC_SAI_gp  0  /* Sampling Interval group position. */
#define CADC_SAI0_bm  (1<<0)  /* Sampling Interval bit 0 mask. */
#define CADC_SAI0_bp  0  /* Sampling Interval bit 0 position. */
#define CADC_SAI1_bm  (1<<1)  /* Sampling Interval bit 1 mask. */
#define CADC_SAI1_bp  1  /* Sampling Interval bit 1 position. */


/* CADC.INTCTRL  bit masks and bit positions */
#define CADC_CCINTLVL_gm  0x30  /* Conversion Complete Interrupt Level group mask. */
#define CADC_CCINTLVL_gp  4  /* Conversion Complete Interrupt Level group position. */
#define CADC_CCINTLVL0_bm  (1<<4)  /* Conversion Complete Interrupt Level bit 0 mask. */
#define CADC_CCINTLVL0_bp  4  /* Conversion Complete Interrupt Level bit 0 position. */
#define CADC_CCINTLVL1_bm  (1<<5)  /* Conversion Complete Interrupt Level bit 1 mask. */
#define CADC_CCINTLVL1_bp  5  /* Conversion Complete Interrupt Level bit 1 position. */

#define CADC_ACIE_bm  0x08  /* Accumulated Interrupt Mask bit mask. */
#define CADC_ACIE_bp  3  /* Accumulated Interrupt Mask bit position. */

#define CADC_ICIE_bm  0x04  /* Instantaneous Interrupt Mask bit mask. */
#define CADC_ICIE_bp  2  /* Instantaneous Interrupt Mask bit position. */

#define CADC_SMINTLVL_gm  0x03  /* Sample Mode Interrupt Level group mask. */
#define CADC_SMINTLVL_gp  0  /* Sample Mode Interrupt Level group position. */
#define CADC_SMINTLVL0_bm  (1<<0)  /* Sample Mode Interrupt Level bit 0 mask. */
#define CADC_SMINTLVL0_bp  0  /* Sample Mode Interrupt Level bit 0 position. */
#define CADC_SMINTLVL1_bm  (1<<1)  /* Sample Mode Interrupt Level bit 1 mask. */
#define CADC_SMINTLVL1_bp  1  /* Sample Mode Interrupt Level bit 1 position. */


/* CADC.DIAGNOSIS  bit masks and bit positions */
#define CADC_PINDM_gm  0x03  /* Pin Diagnostics Mode group mask. */
#define CADC_PINDM_gp  0  /* Pin Diagnostics Mode group position. */
#define CADC_PINDM0_bm  (1<<0)  /* Pin Diagnostics Mode bit 0 mask. */
#define CADC_PINDM0_bp  0  /* Pin Diagnostics Mode bit 0 position. */
#define CADC_PINDM1_bm  (1<<1)  /* Pin Diagnostics Mode bit 1 mask. */
#define CADC_PINDM1_bp  1  /* Pin Diagnostics Mode bit 1 position. */


/* CADC.STATUS  bit masks and bit positions */
#define CADC_ACSSS_bm  0x20  /* Accumulated Settling Sample Status bit mask. */
#define CADC_ACSSS_bp  5  /* Accumulated Settling Sample Status bit position. */

#define CADC_ICSSS_bm  0x10  /* Instantaneous Settling Sample Status bit mask. */
#define CADC_ICSSS_bp  4  /* Instantaneous Settling Sample Status bit position. */

#define CADC_RLF_bm  0x08  /* Register Locked Flag bit mask. */
#define CADC_RLF_bp  3  /* Register Locked Flag bit position. */

#define CADC_SMIF_bm  0x04  /* Instantaneous Conversion Interrupt Flag bit mask. */
#define CADC_SMIF_bp  2  /* Instantaneous Conversion Interrupt Flag bit position. */

#define CADC_ACIF_bm  0x02  /* Accumulated Conversion Interrupt Flag bit mask. */
#define CADC_ACIF_bp  1  /* Accumulated Conversion Interrupt Flag bit position. */

#define CADC_ICIF_bm  0x01  /* Instantaneous Conversion Interrupt Flag bit mask. */
#define CADC_ICIF_bp  0  /* Instantaneous Conversion Interrupt Flag bit position. */


/* CADC.PROTECTION  bit masks and bit positions */
#define CADC_WCIVS_bm  0x80  /* Window Comparator Input Value Select bit mask. */
#define CADC_WCIVS_bp  7  /* Window Comparator Input Value Select bit position. */

#define CADC_PROTCNT_gm  0x7F  /* Protection Count group mask. */
#define CADC_PROTCNT_gp  0  /* Protection Count group position. */
#define CADC_PROTCNT0_bm  (1<<0)  /* Protection Count bit 0 mask. */
#define CADC_PROTCNT0_bp  0  /* Protection Count bit 0 position. */
#define CADC_PROTCNT1_bm  (1<<1)  /* Protection Count bit 1 mask. */
#define CADC_PROTCNT1_bp  1  /* Protection Count bit 1 position. */
#define CADC_PROTCNT2_bm  (1<<2)  /* Protection Count bit 2 mask. */
#define CADC_PROTCNT2_bp  2  /* Protection Count bit 2 position. */
#define CADC_PROTCNT3_bm  (1<<3)  /* Protection Count bit 3 mask. */
#define CADC_PROTCNT3_bp  3  /* Protection Count bit 3 position. */
#define CADC_PROTCNT4_bm  (1<<4)  /* Protection Count bit 4 mask. */
#define CADC_PROTCNT4_bp  4  /* Protection Count bit 4 position. */
#define CADC_PROTCNT5_bm  (1<<5)  /* Protection Count bit 5 mask. */
#define CADC_PROTCNT5_bp  5  /* Protection Count bit 5 position. */
#define CADC_PROTCNT6_bm  (1<<6)  /* Protection Count bit 6 mask. */
#define CADC_PROTCNT6_bp  6  /* Protection Count bit 6 position. */


/* CLK - Clock System */
/* CLK.CLKPSR  bit masks and bit positions */
#define CLK_CLKPDIV_gm  0x03  /* Clock Prescale Divide Factor group mask. */
#define CLK_CLKPDIV_gp  0  /* Clock Prescale Divide Factor group position. */
#define CLK_CLKPDIV0_bm  (1<<0)  /* Clock Prescale Divide Factor bit 0 mask. */
#define CLK_CLKPDIV0_bp  0  /* Clock Prescale Divide Factor bit 0 position. */
#define CLK_CLKPDIV1_bm  (1<<1)  /* Clock Prescale Divide Factor bit 1 mask. */
#define CLK_CLKPDIV1_bp  1  /* Clock Prescale Divide Factor bit 1 position. */


/* CLK.CLKSLR  bit masks and bit positions */
#define CLK_CLKSL_bm  0x01  /* Clock Settings Lock bit mask. */
#define CLK_CLKSL_bp  0  /* Clock Settings Lock bit position. */


/* COMTR - Communication Timout Reset */
/* COMTR.CTRL  bit masks and bit positions */
#define COMTR_ENABLE_bm  0x01  /* Enable Communication Timeout Reset bit mask. */
#define COMTR_ENABLE_bp  0  /* Enable Communication Timeout Reset bit position. */


/* CPROT - DAC-Based Current Protection */
/* CPROT.CTRL  bit masks and bit positions */
#define CPROT_COCE_bm  0x01  /* Charge Over Current Protection Enable bit mask. */
#define CPROT_COCE_bp  0  /* Charge Over Current Protection Enable bit position. */

#define CPROT_DOCE_bm  0x02  /* Discharge Over Current Protection Enable bit mask. */
#define CPROT_DOCE_bp  1  /* Discharge Over Current Protection Enable bit position. */

#define CPROT_SCE_bm  0x04  /* Short Circuit protection enable bit mask. */
#define CPROT_SCE_bp  2  /* Short Circuit protection enable bit position. */

#define CPROT_PRMD_bm  0x08  /* Power Reduction Mode enable bit mask. */
#define CPROT_PRMD_bp  3  /* Power Reduction Mode enable bit position. */


/* CPROT.SCTIMING  bit masks and bit positions */
#define CPROT_VIOCNT_gm  0x0F  /* Short Circuit Protection Violation Count group mask. */
#define CPROT_VIOCNT_gp  0  /* Short Circuit Protection Violation Count group position. */
#define CPROT_VIOCNT0_bm  (1<<0)  /* Short Circuit Protection Violation Count bit 0 mask. */
#define CPROT_VIOCNT0_bp  0  /* Short Circuit Protection Violation Count bit 0 position. */
#define CPROT_VIOCNT1_bm  (1<<1)  /* Short Circuit Protection Violation Count bit 1 mask. */
#define CPROT_VIOCNT1_bp  1  /* Short Circuit Protection Violation Count bit 1 position. */
#define CPROT_VIOCNT2_bm  (1<<2)  /* Short Circuit Protection Violation Count bit 2 mask. */
#define CPROT_VIOCNT2_bp  2  /* Short Circuit Protection Violation Count bit 2 position. */
#define CPROT_VIOCNT3_bm  (1<<3)  /* Short Circuit Protection Violation Count bit 3 mask. */
#define CPROT_VIOCNT3_bp  3  /* Short Circuit Protection Violation Count bit 3 position. */

#define CPROT_TKSEL_gm  0x70  /* Short Circuit Protection Time Keeper Channel Select group mask. */
#define CPROT_TKSEL_gp  4  /* Short Circuit Protection Time Keeper Channel Select group position. */
#define CPROT_TKSEL0_bm  (1<<4)  /* Short Circuit Protection Time Keeper Channel Select bit 0 mask. */
#define CPROT_TKSEL0_bp  4  /* Short Circuit Protection Time Keeper Channel Select bit 0 position. */
#define CPROT_TKSEL1_bm  (1<<5)  /* Short Circuit Protection Time Keeper Channel Select bit 1 mask. */
#define CPROT_TKSEL1_bp  5  /* Short Circuit Protection Time Keeper Channel Select bit 1 position. */
#define CPROT_TKSEL2_bm  (1<<6)  /* Short Circuit Protection Time Keeper Channel Select bit 2 mask. */
#define CPROT_TKSEL2_bp  6  /* Short Circuit Protection Time Keeper Channel Select bit 2 position. */


/* CPROT.OCTIMING  bit masks and bit positions */
/* CPROT_VIOCNT_gm  Predefined. */
/* CPROT_VIOCNT_gp  Predefined. */
/* CPROT_VIOCNT0_bm  Predefined. */
/* CPROT_VIOCNT0_bp  Predefined. */
/* CPROT_VIOCNT1_bm  Predefined. */
/* CPROT_VIOCNT1_bp  Predefined. */
/* CPROT_VIOCNT2_bm  Predefined. */
/* CPROT_VIOCNT2_bp  Predefined. */
/* CPROT_VIOCNT3_bm  Predefined. */
/* CPROT_VIOCNT3_bp  Predefined. */

/* CPROT_TKSEL_gm  Predefined. */
/* CPROT_TKSEL_gp  Predefined. */
/* CPROT_TKSEL0_bm  Predefined. */
/* CPROT_TKSEL0_bp  Predefined. */
/* CPROT_TKSEL1_bm  Predefined. */
/* CPROT_TKSEL1_bp  Predefined. */
/* CPROT_TKSEL2_bm  Predefined. */
/* CPROT_TKSEL2_bp  Predefined. */


/* CPROT.SCLEVEL  bit masks and bit positions */
#define CPROT_DETLVL_gm  0x3F  /* Short Circuit Detection Level group mask. */
#define CPROT_DETLVL_gp  0  /* Short Circuit Detection Level group position. */
#define CPROT_DETLVL0_bm  (1<<0)  /* Short Circuit Detection Level bit 0 mask. */
#define CPROT_DETLVL0_bp  0  /* Short Circuit Detection Level bit 0 position. */
#define CPROT_DETLVL1_bm  (1<<1)  /* Short Circuit Detection Level bit 1 mask. */
#define CPROT_DETLVL1_bp  1  /* Short Circuit Detection Level bit 1 position. */
#define CPROT_DETLVL2_bm  (1<<2)  /* Short Circuit Detection Level bit 2 mask. */
#define CPROT_DETLVL2_bp  2  /* Short Circuit Detection Level bit 2 position. */
#define CPROT_DETLVL3_bm  (1<<3)  /* Short Circuit Detection Level bit 3 mask. */
#define CPROT_DETLVL3_bp  3  /* Short Circuit Detection Level bit 3 position. */
#define CPROT_DETLVL4_bm  (1<<4)  /* Short Circuit Detection Level bit 4 mask. */
#define CPROT_DETLVL4_bp  4  /* Short Circuit Detection Level bit 4 position. */
#define CPROT_DETLVL5_bm  (1<<5)  /* Short Circuit Detection Level bit 5 mask. */
#define CPROT_DETLVL5_bp  5  /* Short Circuit Detection Level bit 5 position. */


/* CPROT.DOCLEVEL  bit masks and bit positions */
/* CPROT_DETLVL_gm  Predefined. */
/* CPROT_DETLVL_gp  Predefined. */
/* CPROT_DETLVL0_bm  Predefined. */
/* CPROT_DETLVL0_bp  Predefined. */
/* CPROT_DETLVL1_bm  Predefined. */
/* CPROT_DETLVL1_bp  Predefined. */
/* CPROT_DETLVL2_bm  Predefined. */
/* CPROT_DETLVL2_bp  Predefined. */
/* CPROT_DETLVL3_bm  Predefined. */
/* CPROT_DETLVL3_bp  Predefined. */
/* CPROT_DETLVL4_bm  Predefined. */
/* CPROT_DETLVL4_bp  Predefined. */
/* CPROT_DETLVL5_bm  Predefined. */
/* CPROT_DETLVL5_bp  Predefined. */


/* CPROT.COCLEVEL  bit masks and bit positions */
/* CPROT_DETLVL_gm  Predefined. */
/* CPROT_DETLVL_gp  Predefined. */
/* CPROT_DETLVL0_bm  Predefined. */
/* CPROT_DETLVL0_bp  Predefined. */
/* CPROT_DETLVL1_bm  Predefined. */
/* CPROT_DETLVL1_bp  Predefined. */
/* CPROT_DETLVL2_bm  Predefined. */
/* CPROT_DETLVL2_bp  Predefined. */
/* CPROT_DETLVL3_bm  Predefined. */
/* CPROT_DETLVL3_bp  Predefined. */
/* CPROT_DETLVL4_bm  Predefined. */
/* CPROT_DETLVL4_bp  Predefined. */
/* CPROT_DETLVL5_bm  Predefined. */
/* CPROT_DETLVL5_bp  Predefined. */


/* CPROT.OCREG  bit masks and bit positions */
#define CPROT_OFFSET_gm  0x1F  /* Offset Value group mask. */
#define CPROT_OFFSET_gp  0  /* Offset Value group position. */
#define CPROT_OFFSET0_bm  (1<<0)  /* Offset Value bit 0 mask. */
#define CPROT_OFFSET0_bp  0  /* Offset Value bit 0 position. */
#define CPROT_OFFSET1_bm  (1<<1)  /* Offset Value bit 1 mask. */
#define CPROT_OFFSET1_bp  1  /* Offset Value bit 1 position. */
#define CPROT_OFFSET2_bm  (1<<2)  /* Offset Value bit 2 mask. */
#define CPROT_OFFSET2_bp  2  /* Offset Value bit 2 position. */
#define CPROT_OFFSET3_bm  (1<<3)  /* Offset Value bit 3 mask. */
#define CPROT_OFFSET3_bp  3  /* Offset Value bit 3 position. */
#define CPROT_OFFSET4_bm  (1<<4)  /* Offset Value bit 4 mask. */
#define CPROT_OFFSET4_bp  4  /* Offset Value bit 4 position. */


/* CPU - CPU */
/* CPU.CCP  bit masks and bit positions */
#define CPU_CCP_gm  0xFF  /* CCP signature group mask. */
#define CPU_CCP_gp  0  /* CCP signature group position. */
#define CPU_CCP0_bm  (1<<0)  /* CCP signature bit 0 mask. */
#define CPU_CCP0_bp  0  /* CCP signature bit 0 position. */
#define CPU_CCP1_bm  (1<<1)  /* CCP signature bit 1 mask. */
#define CPU_CCP1_bp  1  /* CCP signature bit 1 position. */
#define CPU_CCP2_bm  (1<<2)  /* CCP signature bit 2 mask. */
#define CPU_CCP2_bp  2  /* CCP signature bit 2 position. */
#define CPU_CCP3_bm  (1<<3)  /* CCP signature bit 3 mask. */
#define CPU_CCP3_bp  3  /* CCP signature bit 3 position. */
#define CPU_CCP4_bm  (1<<4)  /* CCP signature bit 4 mask. */
#define CPU_CCP4_bp  4  /* CCP signature bit 4 position. */
#define CPU_CCP5_bm  (1<<5)  /* CCP signature bit 5 mask. */
#define CPU_CCP5_bp  5  /* CCP signature bit 5 position. */
#define CPU_CCP6_bm  (1<<6)  /* CCP signature bit 6 mask. */
#define CPU_CCP6_bp  6  /* CCP signature bit 6 position. */
#define CPU_CCP7_bm  (1<<7)  /* CCP signature bit 7 mask. */
#define CPU_CCP7_bp  7  /* CCP signature bit 7 position. */


/* CPU.SREG  bit masks and bit positions */
#define CPU_I_bm  0x80  /* Global Interrupt Enable Flag bit mask. */
#define CPU_I_bp  7  /* Global Interrupt Enable Flag bit position. */

#define CPU_T_bm  0x40  /* Transfer Bit bit mask. */
#define CPU_T_bp  6  /* Transfer Bit bit position. */

#define CPU_H_bm  0x20  /* Half Carry Flag bit mask. */
#define CPU_H_bp  5  /* Half Carry Flag bit position. */

#define CPU_S_bm  0x10  /* N Exclusive Or V Flag bit mask. */
#define CPU_S_bp  4  /* N Exclusive Or V Flag bit position. */

#define CPU_V_bm  0x08  /* Two's Complement Overflow Flag bit mask. */
#define CPU_V_bp  3  /* Two's Complement Overflow Flag bit position. */

#define CPU_N_bm  0x04  /* Negative Flag bit mask. */
#define CPU_N_bp  2  /* Negative Flag bit position. */

#define CPU_Z_bm  0x02  /* Zero Flag bit mask. */
#define CPU_Z_bp  1  /* Zero Flag bit position. */

#define CPU_C_bm  0x01  /* Carry Flag bit mask. */
#define CPU_C_bp  0  /* Carry Flag bit position. */


/* CRC - Cyclic Redundancy Checker */
/* CRC.CTRL  bit masks and bit positions */
#define CRC_RESET_gm  0xC0  /* Reset group mask. */
#define CRC_RESET_gp  6  /* Reset group position. */
#define CRC_RESET0_bm  (1<<6)  /* Reset bit 0 mask. */
#define CRC_RESET0_bp  6  /* Reset bit 0 position. */
#define CRC_RESET1_bm  (1<<7)  /* Reset bit 1 mask. */
#define CRC_RESET1_bp  7  /* Reset bit 1 position. */

#define CRC_CRC32_bm  0x20  /* CRC Mode bit mask. */
#define CRC_CRC32_bp  5  /* CRC Mode bit position. */

#define CRC_SOURCE_gm  0x0F  /* Input Source group mask. */
#define CRC_SOURCE_gp  0  /* Input Source group position. */
#define CRC_SOURCE0_bm  (1<<0)  /* Input Source bit 0 mask. */
#define CRC_SOURCE0_bp  0  /* Input Source bit 0 position. */
#define CRC_SOURCE1_bm  (1<<1)  /* Input Source bit 1 mask. */
#define CRC_SOURCE1_bp  1  /* Input Source bit 1 position. */
#define CRC_SOURCE2_bm  (1<<2)  /* Input Source bit 2 mask. */
#define CRC_SOURCE2_bp  2  /* Input Source bit 2 position. */
#define CRC_SOURCE3_bm  (1<<3)  /* Input Source bit 3 mask. */
#define CRC_SOURCE3_bp  3  /* Input Source bit 3 position. */


/* CRC.STATUS  bit masks and bit positions */
#define CRC_ZERO_bm  0x02  /* Zero detection bit mask. */
#define CRC_ZERO_bp  1  /* Zero detection bit position. */

#define CRC_BUSY_bm  0x01  /* Busy bit mask. */
#define CRC_BUSY_bp  0  /* Busy bit position. */


/* FET - FET controller */
/* FET.CTRL  bit masks and bit positions */
#define FET_DUVREN_bm  0x04  /* DUVR enable bit mask. */
#define FET_DUVREN_bp  2  /* DUVR enable bit position. */

#define FET_DFE_bm  0x02  /* Discharge FET enable bit mask. */
#define FET_DFE_bp  1  /* Discharge FET enable bit position. */

#define FET_CFE_bm  0x01  /* Charge FET Enable bit mask. */
#define FET_CFE_bp  0  /* Charge FET Enable bit position. */


/* FET.FETACTIONA  bit masks and bit positions */
#define FET_DHCF_gm  0x0C  /* Discharge High Current Protection FET Control group mask. */
#define FET_DHCF_gp  2  /* Discharge High Current Protection FET Control group position. */
#define FET_DHCF0_bm  (1<<2)  /* Discharge High Current Protection FET Control bit 0 mask. */
#define FET_DHCF0_bp  2  /* Discharge High Current Protection FET Control bit 0 position. */
#define FET_DHCF1_bm  (1<<3)  /* Discharge High Current Protection FET Control bit 1 mask. */
#define FET_DHCF1_bp  3  /* Discharge High Current Protection FET Control bit 1 position. */

#define FET_CHCF_gm  0x03  /* Charge High Current Protection FET Control group mask. */
#define FET_CHCF_gp  0  /* Charge High Current Protection FET Control group position. */
#define FET_CHCF0_bm  (1<<0)  /* Charge High Current Protection FET Control bit 0 mask. */
#define FET_CHCF0_bp  0  /* Charge High Current Protection FET Control bit 0 position. */
#define FET_CHCF1_bm  (1<<1)  /* Charge High Current Protection FET Control bit 1 mask. */
#define FET_CHCF1_bp  1  /* Charge High Current Protection FET Control bit 1 position. */


/* FET.FETACTIONE  bit masks and bit positions */
#define FET_A1OVF_gm  0xC0  /* ADC1 Over-voltage Protection FET Control group mask. */
#define FET_A1OVF_gp  6  /* ADC1 Over-voltage Protection FET Control group position. */
#define FET_A1OVF0_bm  (1<<6)  /* ADC1 Over-voltage Protection FET Control bit 0 mask. */
#define FET_A1OVF0_bp  6  /* ADC1 Over-voltage Protection FET Control bit 0 position. */
#define FET_A1OVF1_bm  (1<<7)  /* ADC1 Over-voltage Protection FET Control bit 1 mask. */
#define FET_A1OVF1_bp  7  /* ADC1 Over-voltage Protection FET Control bit 1 position. */

#define FET_A1UVF_gm  0x30  /* ADC1 Under-voltage Protection FET Control group mask. */
#define FET_A1UVF_gp  4  /* ADC1 Under-voltage Protection FET Control group position. */
#define FET_A1UVF0_bm  (1<<4)  /* ADC1 Under-voltage Protection FET Control bit 0 mask. */
#define FET_A1UVF0_bp  4  /* ADC1 Under-voltage Protection FET Control bit 0 position. */
#define FET_A1UVF1_bm  (1<<5)  /* ADC1 Under-voltage Protection FET Control bit 1 mask. */
#define FET_A1UVF1_bp  5  /* ADC1 Under-voltage Protection FET Control bit 1 position. */

#define FET_A0OVF_gm  0x0C  /* ADC0 Over-voltage Protection FET Control group mask. */
#define FET_A0OVF_gp  2  /* ADC0 Over-voltage Protection FET Control group position. */
#define FET_A0OVF0_bm  (1<<2)  /* ADC0 Over-voltage Protection FET Control bit 0 mask. */
#define FET_A0OVF0_bp  2  /* ADC0 Over-voltage Protection FET Control bit 0 position. */
#define FET_A0OVF1_bm  (1<<3)  /* ADC0 Over-voltage Protection FET Control bit 1 mask. */
#define FET_A0OVF1_bp  3  /* ADC0 Over-voltage Protection FET Control bit 1 position. */

#define FET_A0UVF_gm  0x03  /* ADC0 Under-voltage Protection FET Control group mask. */
#define FET_A0UVF_gp  0  /* ADC0 Under-voltage Protection FET Control group position. */
#define FET_A0UVF0_bm  (1<<0)  /* ADC0 Under-voltage Protection FET Control bit 0 mask. */
#define FET_A0UVF0_bp  0  /* ADC0 Under-voltage Protection FET Control bit 0 position. */
#define FET_A0UVF1_bm  (1<<1)  /* ADC0 Under-voltage Protection FET Control bit 1 mask. */
#define FET_A0UVF1_bp  1  /* ADC0 Under-voltage Protection FET Control bit 1 position. */


/* FET.FETACTIONF  bit masks and bit positions */
#define FET_ITOVF_gm  0x0C  /* Internal Temperature Over-voltage Protection FET Control group mask. */
#define FET_ITOVF_gp  2  /* Internal Temperature Over-voltage Protection FET Control group position. */
#define FET_ITOVF0_bm  (1<<2)  /* Internal Temperature Over-voltage Protection FET Control bit 0 mask. */
#define FET_ITOVF0_bp  2  /* Internal Temperature Over-voltage Protection FET Control bit 0 position. */
#define FET_ITOVF1_bm  (1<<3)  /* Internal Temperature Over-voltage Protection FET Control bit 1 mask. */
#define FET_ITOVF1_bp  3  /* Internal Temperature Over-voltage Protection FET Control bit 1 position. */

#define FET_ITUVF_gm  0x03  /* Internal Temperature Under-voltage Protection FET Control group mask. */
#define FET_ITUVF_gp  0  /* Internal Temperature Under-voltage Protection FET Control group position. */
#define FET_ITUVF0_bm  (1<<0)  /* Internal Temperature Under-voltage Protection FET Control bit 0 mask. */
#define FET_ITUVF0_bp  0  /* Internal Temperature Under-voltage Protection FET Control bit 0 position. */
#define FET_ITUVF1_bm  (1<<1)  /* Internal Temperature Under-voltage Protection FET Control bit 1 mask. */
#define FET_ITUVF1_bp  1  /* Internal Temperature Under-voltage Protection FET Control bit 1 position. */

#define FET_COVF_gm  0xC0  /* Cell Over-voltage Protection FET Control group mask. */
#define FET_COVF_gp  6  /* Cell Over-voltage Protection FET Control group position. */
#define FET_COVF0_bm  (1<<6)  /* Cell Over-voltage Protection FET Control bit 0 mask. */
#define FET_COVF0_bp  6  /* Cell Over-voltage Protection FET Control bit 0 position. */
#define FET_COVF1_bm  (1<<7)  /* Cell Over-voltage Protection FET Control bit 1 mask. */
#define FET_COVF1_bp  7  /* Cell Over-voltage Protection FET Control bit 1 position. */

#define FET_CUVF_gm  0x30  /* Cell Under-voltage Protection FET Control group mask. */
#define FET_CUVF_gp  4  /* Cell Under-voltage Protection FET Control group position. */
#define FET_CUVF0_bm  (1<<4)  /* Cell Under-voltage Protection FET Control bit 0 mask. */
#define FET_CUVF0_bp  4  /* Cell Under-voltage Protection FET Control bit 0 position. */
#define FET_CUVF1_bm  (1<<5)  /* Cell Under-voltage Protection FET Control bit 1 mask. */
#define FET_CUVF1_bp  5  /* Cell Under-voltage Protection FET Control bit 1 position. */


/* FET.FETACTIONG  bit masks and bit positions */
#define FET_SCF_gm  0x30  /* Short Circuit Protection FET Control group mask. */
#define FET_SCF_gp  4  /* Short Circuit Protection FET Control group position. */
#define FET_SCF0_bm  (1<<4)  /* Short Circuit Protection FET Control bit 0 mask. */
#define FET_SCF0_bp  4  /* Short Circuit Protection FET Control bit 0 position. */
#define FET_SCF1_bm  (1<<5)  /* Short Circuit Protection FET Control bit 1 mask. */
#define FET_SCF1_bp  5  /* Short Circuit Protection FET Control bit 1 position. */

#define FET_DOCF_gm  0x0C  /* Discharge Over Current Protection FET Control group mask. */
#define FET_DOCF_gp  2  /* Discharge Over Current Protection FET Control group position. */
#define FET_DOCF0_bm  (1<<2)  /* Discharge Over Current Protection FET Control bit 0 mask. */
#define FET_DOCF0_bp  2  /* Discharge Over Current Protection FET Control bit 0 position. */
#define FET_DOCF1_bm  (1<<3)  /* Discharge Over Current Protection FET Control bit 1 mask. */
#define FET_DOCF1_bp  3  /* Discharge Over Current Protection FET Control bit 1 position. */

#define FET_COCF_gm  0x03  /* Charge Over Current Protection FET Control group mask. */
#define FET_COCF_gp  0  /* Charge Over Current Protection FET Control group position. */
#define FET_COCF0_bm  (1<<0)  /* Charge Over Current Protection FET Control bit 0 mask. */
#define FET_COCF0_bp  0  /* Charge Over Current Protection FET Control bit 0 position. */
#define FET_COCF1_bm  (1<<1)  /* Charge Over Current Protection FET Control bit 1 mask. */
#define FET_COCF1_bp  1  /* Charge Over Current Protection FET Control bit 1 position. */


/* FET.INTCTRLINTMASKA  bit masks and bit positions */
#define FET_INTLVL_gm  0x03  /* Protection Interrupt Level group mask. */
#define FET_INTLVL_gp  0  /* Protection Interrupt Level group position. */
#define FET_INTLVL0_bm  (1<<0)  /* Protection Interrupt Level bit 0 mask. */
#define FET_INTLVL0_bp  0  /* Protection Interrupt Level bit 0 position. */
#define FET_INTLVL1_bm  (1<<1)  /* Protection Interrupt Level bit 1 mask. */
#define FET_INTLVL1_bp  1  /* Protection Interrupt Level bit 1 position. */

#define FET_DHCIM_bm  0x02  /* Discharge High Current Protection Interrupt Mask bit mask. */
#define FET_DHCIM_bp  1  /* Discharge High Current Protection Interrupt Mask bit position. */

#define FET_CHCIM_bm  0x01  /* Charge High Current Protection Interrupt Mask bit mask. */
#define FET_CHCIM_bp  0  /* Charge High Current Protection Interrupt Mask bit position. */


/* FET.INTCTRLINTMASKA  bit masks and bit positions */
/* FET_INTLVL_gm  Predefined. */
/* FET_INTLVL_gp  Predefined. */
/* FET_INTLVL0_bm  Predefined. */
/* FET_INTLVL0_bp  Predefined. */
/* FET_INTLVL1_bm  Predefined. */
/* FET_INTLVL1_bp  Predefined. */

/* FET_DHCIM_bm  Predefined. */
/* FET_DHCIM_bp  Predefined. */

/* FET_CHCIM_bm  Predefined. */
/* FET_CHCIM_bp  Predefined. */


/* FET.INTMASKC  bit masks and bit positions */
#define FET_ITOVIM_bm  0x80  /* Internal Temperature Over-voltage Protection Interrupt Mask bit mask. */
#define FET_ITOVIM_bp  7  /* Internal Temperature Over-voltage Protection Interrupt Mask bit position. */

#define FET_ITUVIM_bm  0x40  /* Internal Temperature Under-voltage Protection Interrupt Mask bit mask. */
#define FET_ITUVIM_bp  6  /* Internal Temperature Under-voltage Protection Interrupt Mask bit position. */

#define FET_COVIM_bm  0x20  /* Cell Over-voltage Protection Interrupt Mask bit mask. */
#define FET_COVIM_bp  5  /* Cell Over-voltage Protection Interrupt Mask bit position. */

#define FET_CUVIM_bm  0x10  /* Cell Under-voltage Protection Interrupt Mask bit mask. */
#define FET_CUVIM_bp  4  /* Cell Under-voltage Protection Interrupt Mask bit position. */

#define FET_A1OVIM_bm  0x08  /* ADC1 Over-voltage Protection Interrupt Mask bit mask. */
#define FET_A1OVIM_bp  3  /* ADC1 Over-voltage Protection Interrupt Mask bit position. */

#define FET_A1UVIM_bm  0x04  /* ADC1 Under-voltage Protection Interrupt Mask bit mask. */
#define FET_A1UVIM_bp  2  /* ADC1 Under-voltage Protection Interrupt Mask bit position. */

#define FET_A0OVIM_bm  0x02  /* ADC0 Over-voltage Protection Interrupt Mask bit mask. */
#define FET_A0OVIM_bp  1  /* ADC0 Over-voltage Protection Interrupt Mask bit position. */

#define FET_A0UVIM_bm  0x01  /* ADC0 Under-voltage Protection Interrupt Mask bit mask. */
#define FET_A0UVIM_bp  0  /* ADC0 Under-voltage Protection Interrupt Mask bit position. */


/* FET.INTMASKD  bit masks and bit positions */
#define FET_SCIM_bm  0x04  /* Short Circuit Protection Interrupt Mask bit mask. */
#define FET_SCIM_bp  2  /* Short Circuit Protection Interrupt Mask bit position. */

#define FET_DOCIM_bm  0x02  /* Discharge Over Current Protection Interrupt Mask bit mask. */
#define FET_DOCIM_bp  1  /* Discharge Over Current Protection Interrupt Mask bit position. */

#define FET_COCIM_bm  0x01  /* Charge Over Current Protection Interrupt Mask bit mask. */
#define FET_COCIM_bp  0  /* Charge Over Current Protection Interrupt Mask bit position. */


/* FET.STATUSA  bit masks and bit positions */
#define FET_DHCIF_bm  0x02  /* Discharge High Current Protection Interrupt Flag bit mask. */
#define FET_DHCIF_bp  1  /* Discharge High Current Protection Interrupt Flag bit position. */

#define FET_CHCIF_bm  0x01  /* Charge High Current Protection Interrupt Flag bit mask. */
#define FET_CHCIF_bp  0  /* Charge High Current Protection Interrupt Flag bit position. */


/* FET.STATUSC  bit masks and bit positions */
#define FET_ITOVIF_bm  0x80  /* Internal Temperature Over-voltage Protection Interrupt Flag bit mask. */
#define FET_ITOVIF_bp  7  /* Internal Temperature Over-voltage Protection Interrupt Flag bit position. */

#define FET_ITUVIF_bm  0x40  /* Internal Temperature Under-voltage Protection Interrupt Flag bit mask. */
#define FET_ITUVIF_bp  6  /* Internal Temperature Under-voltage Protection Interrupt Flag bit position. */

#define FET_COVIF_bm  0x20  /* Cell Over-voltage Protection Interrupt Flag bit mask. */
#define FET_COVIF_bp  5  /* Cell Over-voltage Protection Interrupt Flag bit position. */

#define FET_CUVIF_bm  0x10  /* Cell Under-voltage Protection Interrupt Flag bit mask. */
#define FET_CUVIF_bp  4  /* Cell Under-voltage Protection Interrupt Flag bit position. */

#define FET_A1OVIF_bm  0x08  /* ADC1 Over-voltage Protection Interrupt Flag bit mask. */
#define FET_A1OVIF_bp  3  /* ADC1 Over-voltage Protection Interrupt Flag bit position. */

#define FET_A1UVIF_bm  0x04  /* ADC1 Under-voltage Protection Interrupt Flag bit mask. */
#define FET_A1UVIF_bp  2  /* ADC1 Under-voltage Protection Interrupt Flag bit position. */

#define FET_A0OVIF_bm  0x02  /* ADC0 Over-voltage Protection Interrupt Flag bit mask. */
#define FET_A0OVIF_bp  1  /* ADC0 Over-voltage Protection Interrupt Flag bit position. */

#define FET_A0UVIF_bm  0x01  /* ADC0 Under-voltage Protection Interrupt Flag bit mask. */
#define FET_A0UVIF_bp  0  /* ADC0 Under-voltage Protection Interrupt Flag bit position. */


/* FET.STATUSD  bit masks and bit positions */
#define FET_SCIF_bm  0x04  /* Short Circuit Protection Interrupt Flag bit mask. */
#define FET_SCIF_bp  2  /* Short Circuit Protection Interrupt Flag bit position. */

#define FET_DOCIF_bm  0x02  /* Discharge Over Current Protection Interrupt Flag bit mask. */
#define FET_DOCIF_bp  1  /* Discharge Over Current Protection Interrupt Flag bit position. */

#define FET_COCIF_bm  0x01  /* Charge Over Current Protection Interrupt Flag bit mask. */
#define FET_COCIF_bp  0  /* Charge Over Current Protection Interrupt Flag bit position. */


/* NVM - Non Volatile Memory Controller */
/* NVM.CMD  bit masks and bit positions */
#define NVM_CMD_gm  0x7F  /* Command group mask. */
#define NVM_CMD_gp  0  /* Command group position. */
#define NVM_CMD0_bm  (1<<0)  /* Command bit 0 mask. */
#define NVM_CMD0_bp  0  /* Command bit 0 position. */
#define NVM_CMD1_bm  (1<<1)  /* Command bit 1 mask. */
#define NVM_CMD1_bp  1  /* Command bit 1 position. */
#define NVM_CMD2_bm  (1<<2)  /* Command bit 2 mask. */
#define NVM_CMD2_bp  2  /* Command bit 2 position. */
#define NVM_CMD3_bm  (1<<3)  /* Command bit 3 mask. */
#define NVM_CMD3_bp  3  /* Command bit 3 position. */
#define NVM_CMD4_bm  (1<<4)  /* Command bit 4 mask. */
#define NVM_CMD4_bp  4  /* Command bit 4 position. */
#define NVM_CMD5_bm  (1<<5)  /* Command bit 5 mask. */
#define NVM_CMD5_bp  5  /* Command bit 5 position. */
#define NVM_CMD6_bm  (1<<6)  /* Command bit 6 mask. */
#define NVM_CMD6_bp  6  /* Command bit 6 position. */


/* NVM.CTRLA  bit masks and bit positions */
#define NVM_CMDEX_bm  0x01  /* Command Execute bit mask. */
#define NVM_CMDEX_bp  0  /* Command Execute bit position. */


/* NVM.CTRLB  bit masks and bit positions */
#define NVM_EEMAPEN_bm  0x08  /* EEPROM Mapping Enable bit mask. */
#define NVM_EEMAPEN_bp  3  /* EEPROM Mapping Enable bit position. */

#define NVM_FPRM_bm  0x04  /* Flash Power Reduction Enable bit mask. */
#define NVM_FPRM_bp  2  /* Flash Power Reduction Enable bit position. */

#define NVM_EPRM_bm  0x02  /* EEPROM Power Reduction Enable bit mask. */
#define NVM_EPRM_bp  1  /* EEPROM Power Reduction Enable bit position. */

#define NVM_SPMLOCK_bm  0x01  /* SPM Lock bit mask. */
#define NVM_SPMLOCK_bp  0  /* SPM Lock bit position. */


/* NVM.INTCTRL  bit masks and bit positions */
#define NVM_SPMLVL_gm  0x0C  /* SPM Interrupt Level group mask. */
#define NVM_SPMLVL_gp  2  /* SPM Interrupt Level group position. */
#define NVM_SPMLVL0_bm  (1<<2)  /* SPM Interrupt Level bit 0 mask. */
#define NVM_SPMLVL0_bp  2  /* SPM Interrupt Level bit 0 position. */
#define NVM_SPMLVL1_bm  (1<<3)  /* SPM Interrupt Level bit 1 mask. */
#define NVM_SPMLVL1_bp  3  /* SPM Interrupt Level bit 1 position. */

#define NVM_EELVL_gm  0x03  /* EEPROM Interrupt Level group mask. */
#define NVM_EELVL_gp  0  /* EEPROM Interrupt Level group position. */
#define NVM_EELVL0_bm  (1<<0)  /* EEPROM Interrupt Level bit 0 mask. */
#define NVM_EELVL0_bp  0  /* EEPROM Interrupt Level bit 0 position. */
#define NVM_EELVL1_bm  (1<<1)  /* EEPROM Interrupt Level bit 1 mask. */
#define NVM_EELVL1_bp  1  /* EEPROM Interrupt Level bit 1 position. */


/* NVM.STATUS  bit masks and bit positions */
#define NVM_NVMBUSY_bm  0x80  /* Non-volatile Memory Busy bit mask. */
#define NVM_NVMBUSY_bp  7  /* Non-volatile Memory Busy bit position. */

#define NVM_FBUSY_bm  0x40  /* Flash Memory Busy bit mask. */
#define NVM_FBUSY_bp  6  /* Flash Memory Busy bit position. */

#define NVM_EELOAD_bm  0x02  /* EEPROM Page Buffer Active Loading bit mask. */
#define NVM_EELOAD_bp  1  /* EEPROM Page Buffer Active Loading bit position. */

#define NVM_FLOAD_bm  0x01  /* Flash Page Buffer Active Loading bit mask. */
#define NVM_FLOAD_bp  0  /* Flash Page Buffer Active Loading bit position. */


/* NVM.LOCKBITS  bit masks and bit positions */
#define NVM_BLBB_gm  0xC0  /* Boot Lock Bits - Boot Section group mask. */
#define NVM_BLBB_gp  6  /* Boot Lock Bits - Boot Section group position. */
#define NVM_BLBB0_bm  (1<<6)  /* Boot Lock Bits - Boot Section bit 0 mask. */
#define NVM_BLBB0_bp  6  /* Boot Lock Bits - Boot Section bit 0 position. */
#define NVM_BLBB1_bm  (1<<7)  /* Boot Lock Bits - Boot Section bit 1 mask. */
#define NVM_BLBB1_bp  7  /* Boot Lock Bits - Boot Section bit 1 position. */

#define NVM_BLBA_gm  0x30  /* Boot Lock Bits - Application Section group mask. */
#define NVM_BLBA_gp  4  /* Boot Lock Bits - Application Section group position. */
#define NVM_BLBA0_bm  (1<<4)  /* Boot Lock Bits - Application Section bit 0 mask. */
#define NVM_BLBA0_bp  4  /* Boot Lock Bits - Application Section bit 0 position. */
#define NVM_BLBA1_bm  (1<<5)  /* Boot Lock Bits - Application Section bit 1 mask. */
#define NVM_BLBA1_bp  5  /* Boot Lock Bits - Application Section bit 1 position. */

#define NVM_BLBAT_gm  0x0C  /* Boot Lock Bits - Application Table group mask. */
#define NVM_BLBAT_gp  2  /* Boot Lock Bits - Application Table group position. */
#define NVM_BLBAT0_bm  (1<<2)  /* Boot Lock Bits - Application Table bit 0 mask. */
#define NVM_BLBAT0_bp  2  /* Boot Lock Bits - Application Table bit 0 position. */
#define NVM_BLBAT1_bm  (1<<3)  /* Boot Lock Bits - Application Table bit 1 mask. */
#define NVM_BLBAT1_bp  3  /* Boot Lock Bits - Application Table bit 1 position. */

#define NVM_LB_gm  0x03  /* Lock Bits group mask. */
#define NVM_LB_gp  0  /* Lock Bits group position. */
#define NVM_LB0_bm  (1<<0)  /* Lock Bits bit 0 mask. */
#define NVM_LB0_bp  0  /* Lock Bits bit 0 position. */
#define NVM_LB1_bm  (1<<1)  /* Lock Bits bit 1 mask. */
#define NVM_LB1_bp  1  /* Lock Bits bit 1 position. */


/* PMIC - Programmable Multi-level Interrupt Controller */
/* PMIC.STATUS  bit masks and bit positions */
#define PMIC_NMIEX_bm  0x80  /* Non-maskable Interrupt Executing bit mask. */
#define PMIC_NMIEX_bp  7  /* Non-maskable Interrupt Executing bit position. */

#define PMIC_HILVLEX_bm  0x04  /* High Level Interrupt Executing bit mask. */
#define PMIC_HILVLEX_bp  2  /* High Level Interrupt Executing bit position. */

#define PMIC_MEDLVLEX_bm  0x02  /* Medium Level Interrupt Executing bit mask. */
#define PMIC_MEDLVLEX_bp  1  /* Medium Level Interrupt Executing bit position. */

#define PMIC_LOLVLEX_bm  0x01  /* Low Level Interrupt Executing bit mask. */
#define PMIC_LOLVLEX_bp  0  /* Low Level Interrupt Executing bit position. */


/* PMIC.CTRL  bit masks and bit positions */
#define PMIC_RREN_bm  0x80  /* Round-Robin Priority Enable bit mask. */
#define PMIC_RREN_bp  7  /* Round-Robin Priority Enable bit position. */

#define PMIC_IVSEL_bm  0x40  /* Interrupt Vector Select bit mask. */
#define PMIC_IVSEL_bp  6  /* Interrupt Vector Select bit position. */

#define PMIC_HILVLEN_bm  0x04  /* High Level Enable bit mask. */
#define PMIC_HILVLEN_bp  2  /* High Level Enable bit position. */

#define PMIC_MEDLVLEN_bm  0x02  /* Medium Level Enable bit mask. */
#define PMIC_MEDLVLEN_bp  1  /* Medium Level Enable bit position. */

#define PMIC_LOLVLEN_bm  0x01  /* Low Level Enable bit mask. */
#define PMIC_LOLVLEN_bp  0  /* Low Level Enable bit position. */


/* LDO - LDO Regulator */
/* LDO.STATUS  bit masks and bit positions */
#define LDO_CS_bm  0x10  /* LDO Condition Status bit mask. */
#define LDO_CS_bp  4  /* LDO Condition Status bit position. */

#define LDO_WIF_bm  0x01  /* LDO Warning Interrupt Flag bit mask. */
#define LDO_WIF_bp  0  /* LDO Warning Interrupt Flag bit position. */


/* LDO.CTRL  bit masks and bit positions */
#define LDO_CONFIG_gm  0x30  /* LDO Configuration group mask. */
#define LDO_CONFIG_gp  4  /* LDO Configuration group position. */
#define LDO_CONFIG0_bm  (1<<4)  /* LDO Configuration bit 0 mask. */
#define LDO_CONFIG0_bp  4  /* LDO Configuration bit 0 position. */
#define LDO_CONFIG1_bm  (1<<5)  /* LDO Configuration bit 1 mask. */
#define LDO_CONFIG1_bp  5  /* LDO Configuration bit 1 position. */

#define LDO_INTLVL_gm  0x03  /* Interrupt Level group mask. */
#define LDO_INTLVL_gp  0  /* Interrupt Level group position. */
#define LDO_INTLVL0_bm  (1<<0)  /* Interrupt Level bit 0 mask. */
#define LDO_INTLVL0_bp  0  /* Interrupt Level bit 0 position. */
#define LDO_INTLVL1_bm  (1<<1)  /* Interrupt Level bit 1 mask. */
#define LDO_INTLVL1_bp  1  /* Interrupt Level bit 1 position. */


/* SLEEP - Sleep Controller */
/* SLEEP.CTRL  bit masks and bit positions */
#define SLEEP_SMODE_gm  0x06  /* Sleep Mode Selection group mask. */
#define SLEEP_SMODE_gp  1  /* Sleep Mode Selection group position. */
#define SLEEP_SMODE0_bm  (1<<1)  /* Sleep Mode Selection bit 0 mask. */
#define SLEEP_SMODE0_bp  1  /* Sleep Mode Selection bit 0 position. */
#define SLEEP_SMODE1_bm  (1<<2)  /* Sleep Mode Selection bit 1 mask. */
#define SLEEP_SMODE1_bp  2  /* Sleep Mode Selection bit 1 position. */

#define SLEEP_SEN_bm  0x01  /* Sleep Enable bit mask. */
#define SLEEP_SEN_bp  0  /* Sleep Enable bit position. */


/* RST - Reset */
/* RST.STATUS  bit masks and bit positions */
#define RST_COMRF_bm  0x40  /* Communication Timeout Reset Flag bit mask. */
#define RST_COMRF_bp  6  /* Communication Timeout Reset Flag bit position. */

#define RST_FWRF_bm  0x20  /* Firmware Reset Flag bit mask. */
#define RST_FWRF_bp  5  /* Firmware Reset Flag bit position. */

#define RST_PDIRF_bm  0x10  /* Programming and Debug Interface Interface Reset Flag bit mask. */
#define RST_PDIRF_bp  4  /* Programming and Debug Interface Interface Reset Flag bit position. */

#define RST_WDRF_bm  0x08  /* Watchdog Reset Flag bit mask. */
#define RST_WDRF_bp  3  /* Watchdog Reset Flag bit position. */

#define RST_BORF_bm  0x04  /* Brown-out Reset Flag bit mask. */
#define RST_BORF_bp  2  /* Brown-out Reset Flag bit position. */

#define RST_EXTRF_bm  0x02  /* External Reset Flag bit mask. */
#define RST_EXTRF_bp  1  /* External Reset Flag bit position. */

#define RST_PORF_bm  0x01  /* Power-on Reset Flag bit mask. */
#define RST_PORF_bp  0  /* Power-on Reset Flag bit position. */


/* RST.CTRL  bit masks and bit positions */
#define RST_FWRST_bm  0x01  /* Firmware Reset bit mask. */
#define RST_FWRST_bp  0  /* Firmware Reset bit position. */


/* USART - Universal Asynchronous Receiver-Transmitter */
/* USART.STATUS  bit masks and bit positions */
#define USART_RXCIF_bm  0x80  /* Receive Interrupt Flag bit mask. */
#define USART_RXCIF_bp  7  /* Receive Interrupt Flag bit position. */

#define USART_TXCIF_bm  0x40  /* Transmit Interrupt Flag bit mask. */
#define USART_TXCIF_bp  6  /* Transmit Interrupt Flag bit position. */

#define USART_DREIF_bm  0x20  /* Data Register Empty Flag bit mask. */
#define USART_DREIF_bp  5  /* Data Register Empty Flag bit position. */

#define USART_FERR_bm  0x10  /* Frame Error bit mask. */
#define USART_FERR_bp  4  /* Frame Error bit position. */

#define USART_BUFOVF_bm  0x08  /* Buffer Overflow bit mask. */
#define USART_BUFOVF_bp  3  /* Buffer Overflow bit position. */

#define USART_PERR_bm  0x04  /* Parity Error bit mask. */
#define USART_PERR_bp  2  /* Parity Error bit position. */

#define USART_RXB8_bm  0x01  /* Receive Bit 8 bit mask. */
#define USART_RXB8_bp  0  /* Receive Bit 8 bit position. */


/* USART.CTRLD  bit masks and bit positions */
#define USART_ONEWIRE_bm  0x01  /* One Wire Mode bit mask. */
#define USART_ONEWIRE_bp  0  /* One Wire Mode bit position. */


/* USART.CTRLA  bit masks and bit positions */
#define USART_RXCINTLVL_gm  0x30  /* Receive Interrupt Level group mask. */
#define USART_RXCINTLVL_gp  4  /* Receive Interrupt Level group position. */
#define USART_RXCINTLVL0_bm  (1<<4)  /* Receive Interrupt Level bit 0 mask. */
#define USART_RXCINTLVL0_bp  4  /* Receive Interrupt Level bit 0 position. */
#define USART_RXCINTLVL1_bm  (1<<5)  /* Receive Interrupt Level bit 1 mask. */
#define USART_RXCINTLVL1_bp  5  /* Receive Interrupt Level bit 1 position. */

#define USART_TXCINTLVL_gm  0x0C  /* Transmit Interrupt Level group mask. */
#define USART_TXCINTLVL_gp  2  /* Transmit Interrupt Level group position. */
#define USART_TXCINTLVL0_bm  (1<<2)  /* Transmit Interrupt Level bit 0 mask. */
#define USART_TXCINTLVL0_bp  2  /* Transmit Interrupt Level bit 0 position. */
#define USART_TXCINTLVL1_bm  (1<<3)  /* Transmit Interrupt Level bit 1 mask. */
#define USART_TXCINTLVL1_bp  3  /* Transmit Interrupt Level bit 1 position. */

#define USART_DREINTLVL_gm  0x03  /* Data Register Empty Interrupt Level group mask. */
#define USART_DREINTLVL_gp  0  /* Data Register Empty Interrupt Level group position. */
#define USART_DREINTLVL0_bm  (1<<0)  /* Data Register Empty Interrupt Level bit 0 mask. */
#define USART_DREINTLVL0_bp  0  /* Data Register Empty Interrupt Level bit 0 position. */
#define USART_DREINTLVL1_bm  (1<<1)  /* Data Register Empty Interrupt Level bit 1 mask. */
#define USART_DREINTLVL1_bp  1  /* Data Register Empty Interrupt Level bit 1 position. */


/* USART.CTRLB  bit masks and bit positions */
#define USART_RXEN_bm  0x10  /* Receiver Enable bit mask. */
#define USART_RXEN_bp  4  /* Receiver Enable bit position. */

#define USART_TXEN_bm  0x08  /* Transmitter Enable bit mask. */
#define USART_TXEN_bp  3  /* Transmitter Enable bit position. */

#define USART_CLK2X_bm  0x04  /* Double transmission speed bit mask. */
#define USART_CLK2X_bp  2  /* Double transmission speed bit position. */

#define USART_MPCM_bm  0x02  /* Multi-processor Communication Mode bit mask. */
#define USART_MPCM_bp  1  /* Multi-processor Communication Mode bit position. */

#define USART_TXB8_bm  0x01  /* Transmit bit 8 bit mask. */
#define USART_TXB8_bp  0  /* Transmit bit 8 bit position. */


/* USART.CTRLC  bit masks and bit positions */
#define USART_CMODE_gm  0xC0  /* Communication Mode group mask. */
#define USART_CMODE_gp  6  /* Communication Mode group position. */
#define USART_CMODE0_bm  (1<<6)  /* Communication Mode bit 0 mask. */
#define USART_CMODE0_bp  6  /* Communication Mode bit 0 position. */
#define USART_CMODE1_bm  (1<<7)  /* Communication Mode bit 1 mask. */
#define USART_CMODE1_bp  7  /* Communication Mode bit 1 position. */

#define USART_PMODE_gm  0x30  /* Parity Mode group mask. */
#define USART_PMODE_gp  4  /* Parity Mode group position. */
#define USART_PMODE0_bm  (1<<4)  /* Parity Mode bit 0 mask. */
#define USART_PMODE0_bp  4  /* Parity Mode bit 0 position. */
#define USART_PMODE1_bm  (1<<5)  /* Parity Mode bit 1 mask. */
#define USART_PMODE1_bp  5  /* Parity Mode bit 1 position. */

#define USART_SBMODE_bm  0x08  /* Stop Bit Mode bit mask. */
#define USART_SBMODE_bp  3  /* Stop Bit Mode bit position. */

#define USART_CHSIZE_gm  0x07  /* Character Size group mask. */
#define USART_CHSIZE_gp  0  /* Character Size group position. */
#define USART_CHSIZE0_bm  (1<<0)  /* Character Size bit 0 mask. */
#define USART_CHSIZE0_bp  0  /* Character Size bit 0 position. */
#define USART_CHSIZE1_bm  (1<<1)  /* Character Size bit 1 mask. */
#define USART_CHSIZE1_bp  1  /* Character Size bit 1 position. */
#define USART_CHSIZE2_bm  (1<<2)  /* Character Size bit 2 mask. */
#define USART_CHSIZE2_bp  2  /* Character Size bit 2 position. */


/* USART.BAUDCTRLA  bit masks and bit positions */
#define USART_BSEL_gm  0xFF  /* Baud Rate Selection Bits [7:0] group mask. */
#define USART_BSEL_gp  0  /* Baud Rate Selection Bits [7:0] group position. */
#define USART_BSEL0_bm  (1<<0)  /* Baud Rate Selection Bits [7:0] bit 0 mask. */
#define USART_BSEL0_bp  0  /* Baud Rate Selection Bits [7:0] bit 0 position. */
#define USART_BSEL1_bm  (1<<1)  /* Baud Rate Selection Bits [7:0] bit 1 mask. */
#define USART_BSEL1_bp  1  /* Baud Rate Selection Bits [7:0] bit 1 position. */
#define USART_BSEL2_bm  (1<<2)  /* Baud Rate Selection Bits [7:0] bit 2 mask. */
#define USART_BSEL2_bp  2  /* Baud Rate Selection Bits [7:0] bit 2 position. */
#define USART_BSEL3_bm  (1<<3)  /* Baud Rate Selection Bits [7:0] bit 3 mask. */
#define USART_BSEL3_bp  3  /* Baud Rate Selection Bits [7:0] bit 3 position. */
#define USART_BSEL4_bm  (1<<4)  /* Baud Rate Selection Bits [7:0] bit 4 mask. */
#define USART_BSEL4_bp  4  /* Baud Rate Selection Bits [7:0] bit 4 position. */
#define USART_BSEL5_bm  (1<<5)  /* Baud Rate Selection Bits [7:0] bit 5 mask. */
#define USART_BSEL5_bp  5  /* Baud Rate Selection Bits [7:0] bit 5 position. */
#define USART_BSEL6_bm  (1<<6)  /* Baud Rate Selection Bits [7:0] bit 6 mask. */
#define USART_BSEL6_bp  6  /* Baud Rate Selection Bits [7:0] bit 6 position. */
#define USART_BSEL7_bm  (1<<7)  /* Baud Rate Selection Bits [7:0] bit 7 mask. */
#define USART_BSEL7_bp  7  /* Baud Rate Selection Bits [7:0] bit 7 position. */


/* USART.BAUDCTRLB  bit masks and bit positions */
#define USART_BSCALE_gm  0xF0  /* Baud Rate Scale group mask. */
#define USART_BSCALE_gp  4  /* Baud Rate Scale group position. */
#define USART_BSCALE0_bm  (1<<4)  /* Baud Rate Scale bit 0 mask. */
#define USART_BSCALE0_bp  4  /* Baud Rate Scale bit 0 position. */
#define USART_BSCALE1_bm  (1<<5)  /* Baud Rate Scale bit 1 mask. */
#define USART_BSCALE1_bp  5  /* Baud Rate Scale bit 1 position. */
#define USART_BSCALE2_bm  (1<<6)  /* Baud Rate Scale bit 2 mask. */
#define USART_BSCALE2_bp  6  /* Baud Rate Scale bit 2 position. */
#define USART_BSCALE3_bm  (1<<7)  /* Baud Rate Scale bit 3 mask. */
#define USART_BSCALE3_bp  7  /* Baud Rate Scale bit 3 position. */

/* USART_BSEL_gm  Predefined. */
/* USART_BSEL_gp  Predefined. */
/* USART_BSEL0_bm  Predefined. */
/* USART_BSEL0_bp  Predefined. */
/* USART_BSEL1_bm  Predefined. */
/* USART_BSEL1_bp  Predefined. */
/* USART_BSEL2_bm  Predefined. */
/* USART_BSEL2_bp  Predefined. */
/* USART_BSEL3_bm  Predefined. */
/* USART_BSEL3_bp  Predefined. */


/* TIM8_16 - Timer 8/16 bit */
/* TIM8_16.CTRLA  bit masks and bit positions */
#define TIM8_16_WORDM_bm  0x80  /* Word (16-bit) Mode bit mask. */
#define TIM8_16_WORDM_bp  7  /* Word (16-bit) Mode bit position. */

#define TIM8_16_ICEN_bm  0x20  /* Input Capture Mode Enable bit mask. */
#define TIM8_16_ICEN_bp  5  /* Input Capture Mode Enable bit position. */

#define TIM8_16_ICNC_bm  0x10  /* Input Capture Noise Canceller bit mask. */
#define TIM8_16_ICNC_bp  4  /* Input Capture Noise Canceller bit position. */

#define TIM8_16_ICES_bm  0x08  /* Input Capture Edge Select bit mask. */
#define TIM8_16_ICES_bp  3  /* Input Capture Edge Select bit position. */

#define TIM8_16_CTC_bm  0x01  /* Clear Timer on Compare Match Mode bit mask. */
#define TIM8_16_CTC_bp  0  /* Clear Timer on Compare Match Mode bit position. */


/* TIM8_16.CTRLB  bit masks and bit positions */
#define TIM8_16_ICS_gm  0x70  /* Input Capture Select group mask. */
#define TIM8_16_ICS_gp  4  /* Input Capture Select group position. */
#define TIM8_16_ICS0_bm  (1<<4)  /* Input Capture Select bit 0 mask. */
#define TIM8_16_ICS0_bp  4  /* Input Capture Select bit 0 position. */
#define TIM8_16_ICS1_bm  (1<<5)  /* Input Capture Select bit 1 mask. */
#define TIM8_16_ICS1_bp  5  /* Input Capture Select bit 1 position. */
#define TIM8_16_ICS2_bm  (1<<6)  /* Input Capture Select bit 2 mask. */
#define TIM8_16_ICS2_bp  6  /* Input Capture Select bit 2 position. */

#define TIM8_16_CS_gm  0x0F  /* Clock Select group mask. */
#define TIM8_16_CS_gp  0  /* Clock Select group position. */
#define TIM8_16_CS0_bm  (1<<0)  /* Clock Select bit 0 mask. */
#define TIM8_16_CS0_bp  0  /* Clock Select bit 0 position. */
#define TIM8_16_CS1_bm  (1<<1)  /* Clock Select bit 1 mask. */
#define TIM8_16_CS1_bp  1  /* Clock Select bit 1 position. */
#define TIM8_16_CS2_bm  (1<<2)  /* Clock Select bit 2 mask. */
#define TIM8_16_CS2_bp  2  /* Clock Select bit 2 position. */
#define TIM8_16_CS3_bm  (1<<3)  /* Clock Select bit 3 mask. */
#define TIM8_16_CS3_bp  3  /* Clock Select bit 3 position. */


/* TIM8_16.INTCTRL  bit masks and bit positions */
#define TIM8_16_ICILVL_gm  0xC0  /* Input Capture Interrupt Level group mask. */
#define TIM8_16_ICILVL_gp  6  /* Input Capture Interrupt Level group position. */
#define TIM8_16_ICILVL0_bm  (1<<6)  /* Input Capture Interrupt Level bit 0 mask. */
#define TIM8_16_ICILVL0_bp  6  /* Input Capture Interrupt Level bit 0 position. */
#define TIM8_16_ICILVL1_bm  (1<<7)  /* Input Capture Interrupt Level bit 1 mask. */
#define TIM8_16_ICILVL1_bp  7  /* Input Capture Interrupt Level bit 1 position. */

#define TIM8_16_OVBILVL_gm  0x30  /* Output Compare Match B Interrupt Level group mask. */
#define TIM8_16_OVBILVL_gp  4  /* Output Compare Match B Interrupt Level group position. */
#define TIM8_16_OVBILVL0_bm  (1<<4)  /* Output Compare Match B Interrupt Level bit 0 mask. */
#define TIM8_16_OVBILVL0_bp  4  /* Output Compare Match B Interrupt Level bit 0 position. */
#define TIM8_16_OVBILVL1_bm  (1<<5)  /* Output Compare Match B Interrupt Level bit 1 mask. */
#define TIM8_16_OVBILVL1_bp  5  /* Output Compare Match B Interrupt Level bit 1 position. */

#define TIM8_16_OVAILVL_gm  0x0C  /* Output Compare Match A Interrupt Level group mask. */
#define TIM8_16_OVAILVL_gp  2  /* Output Compare Match A Interrupt Level group position. */
#define TIM8_16_OVAILVL0_bm  (1<<2)  /* Output Compare Match A Interrupt Level bit 0 mask. */
#define TIM8_16_OVAILVL0_bp  2  /* Output Compare Match A Interrupt Level bit 0 position. */
#define TIM8_16_OVAILVL1_bm  (1<<3)  /* Output Compare Match A Interrupt Level bit 1 mask. */
#define TIM8_16_OVAILVL1_bp  3  /* Output Compare Match A Interrupt Level bit 1 position. */

#define TIM8_16_OVFILVL_gm  0x03  /* Overflow Interrupt Level group mask. */
#define TIM8_16_OVFILVL_gp  0  /* Overflow Interrupt Level group position. */
#define TIM8_16_OVFILVL0_bm  (1<<0)  /* Overflow Interrupt Level bit 0 mask. */
#define TIM8_16_OVFILVL0_bp  0  /* Overflow Interrupt Level bit 0 position. */
#define TIM8_16_OVFILVL1_bm  (1<<1)  /* Overflow Interrupt Level bit 1 mask. */
#define TIM8_16_OVFILVL1_bp  1  /* Overflow Interrupt Level bit 1 position. */


/* TIM8_16.STATUS  bit masks and bit positions */
#define TIM8_16_ICIF_bm  0x08  /* Input Capture Interrupt Flag bit mask. */
#define TIM8_16_ICIF_bp  3  /* Input Capture Interrupt Flag bit position. */

#define TIM8_16_OVBIF_bm  0x04  /* Output Compare Match B Interrupt Flag bit mask. */
#define TIM8_16_OVBIF_bp  2  /* Output Compare Match B Interrupt Flag bit position. */

#define TIM8_16_OVAIF_bm  0x02  /* Output Compare Match A Interrupt Flag bit mask. */
#define TIM8_16_OVAIF_bp  1  /* Output Compare Match A Interrupt Flag bit position. */

#define TIM8_16_OVFIF_bm  0x01  /* Overflow Interrupt Flag bit mask. */
#define TIM8_16_OVFIF_bp  0  /* Overflow Interrupt Flag bit position. */


/* TIMPRESC - Timer Prescaler */
/* TIMPRESC.PRESC  bit masks and bit positions */
#define TIMPRESC_PRSYNC_bm  0x01  /* Prescaler Reset bit mask. */
#define TIMPRESC_PRSYNC_bp  0  /* Prescaler Reset bit position. */


/* TWI - Two-Wire Interface */
/* TWI_SLAVE.CTRLA  bit masks and bit positions */
#define TWI_SLAVE_INTLVL_gm  0xC0  /* Interrupt Level group mask. */
#define TWI_SLAVE_INTLVL_gp  6  /* Interrupt Level group position. */
#define TWI_SLAVE_INTLVL0_bm  (1<<6)  /* Interrupt Level bit 0 mask. */
#define TWI_SLAVE_INTLVL0_bp  6  /* Interrupt Level bit 0 position. */
#define TWI_SLAVE_INTLVL1_bm  (1<<7)  /* Interrupt Level bit 1 mask. */
#define TWI_SLAVE_INTLVL1_bp  7  /* Interrupt Level bit 1 position. */

#define TWI_SLAVE_DIEN_bm  0x20  /* Data Interrupt Enable bit mask. */
#define TWI_SLAVE_DIEN_bp  5  /* Data Interrupt Enable bit position. */

#define TWI_SLAVE_APIEN_bm  0x10  /* Address/Stop Interrupt Enable bit mask. */
#define TWI_SLAVE_APIEN_bp  4  /* Address/Stop Interrupt Enable bit position. */

#define TWI_SLAVE_ENABLE_bm  0x08  /* Enable TWI Slave bit mask. */
#define TWI_SLAVE_ENABLE_bp  3  /* Enable TWI Slave bit position. */

#define TWI_SLAVE_PIEN_bm  0x04  /* Stop Interrupt Enable bit mask. */
#define TWI_SLAVE_PIEN_bp  2  /* Stop Interrupt Enable bit position. */

#define TWI_SLAVE_PMEN_bm  0x02  /* Promiscuous Mode Enable bit mask. */
#define TWI_SLAVE_PMEN_bp  1  /* Promiscuous Mode Enable bit position. */

#define TWI_SLAVE_SMEN_bm  0x01  /* Smart Mode Enable bit mask. */
#define TWI_SLAVE_SMEN_bp  0  /* Smart Mode Enable bit position. */


/* TWI_SLAVE.CTRLB  bit masks and bit positions */
#define TWI_SLAVE_ACKACT_bm  0x04  /* Acknowledge Action bit mask. */
#define TWI_SLAVE_ACKACT_bp  2  /* Acknowledge Action bit position. */

#define TWI_SLAVE_CMD_gm  0x03  /* Command group mask. */
#define TWI_SLAVE_CMD_gp  0  /* Command group position. */
#define TWI_SLAVE_CMD0_bm  (1<<0)  /* Command bit 0 mask. */
#define TWI_SLAVE_CMD0_bp  0  /* Command bit 0 position. */
#define TWI_SLAVE_CMD1_bm  (1<<1)  /* Command bit 1 mask. */
#define TWI_SLAVE_CMD1_bp  1  /* Command bit 1 position. */


/* TWI_SLAVE.STATUS  bit masks and bit positions */
#define TWI_SLAVE_DIF_bm  0x80  /* Data Interrupt Flag bit mask. */
#define TWI_SLAVE_DIF_bp  7  /* Data Interrupt Flag bit position. */

#define TWI_SLAVE_APIF_bm  0x40  /* Address/Stop Interrupt Flag bit mask. */
#define TWI_SLAVE_APIF_bp  6  /* Address/Stop Interrupt Flag bit position. */

#define TWI_SLAVE_CLKHOLD_bm  0x20  /* Clock Hold bit mask. */
#define TWI_SLAVE_CLKHOLD_bp  5  /* Clock Hold bit position. */

#define TWI_SLAVE_RXACK_bm  0x10  /* Received Acknowledge bit mask. */
#define TWI_SLAVE_RXACK_bp  4  /* Received Acknowledge bit position. */

#define TWI_SLAVE_COLL_bm  0x08  /* Collision bit mask. */
#define TWI_SLAVE_COLL_bp  3  /* Collision bit position. */

#define TWI_SLAVE_BUSERR_bm  0x04  /* Bus Error bit mask. */
#define TWI_SLAVE_BUSERR_bp  2  /* Bus Error bit position. */

#define TWI_SLAVE_DIR_bm  0x02  /* Read/Write Direction bit mask. */
#define TWI_SLAVE_DIR_bp  1  /* Read/Write Direction bit position. */

#define TWI_SLAVE_AP_bm  0x01  /* Slave Address or Stop bit mask. */
#define TWI_SLAVE_AP_bp  0  /* Slave Address or Stop bit position. */


/* TWI_SLAVE.ADDRMASK  bit masks and bit positions */
#define TWI_SLAVE_ADDRMASK_gm  0xFE  /* Address Mask group mask. */
#define TWI_SLAVE_ADDRMASK_gp  1  /* Address Mask group position. */
#define TWI_SLAVE_ADDRMASK0_bm  (1<<1)  /* Address Mask bit 0 mask. */
#define TWI_SLAVE_ADDRMASK0_bp  1  /* Address Mask bit 0 position. */
#define TWI_SLAVE_ADDRMASK1_bm  (1<<2)  /* Address Mask bit 1 mask. */
#define TWI_SLAVE_ADDRMASK1_bp  2  /* Address Mask bit 1 position. */
#define TWI_SLAVE_ADDRMASK2_bm  (1<<3)  /* Address Mask bit 2 mask. */
#define TWI_SLAVE_ADDRMASK2_bp  3  /* Address Mask bit 2 position. */
#define TWI_SLAVE_ADDRMASK3_bm  (1<<4)  /* Address Mask bit 3 mask. */
#define TWI_SLAVE_ADDRMASK3_bp  4  /* Address Mask bit 3 position. */
#define TWI_SLAVE_ADDRMASK4_bm  (1<<5)  /* Address Mask bit 4 mask. */
#define TWI_SLAVE_ADDRMASK4_bp  5  /* Address Mask bit 4 position. */
#define TWI_SLAVE_ADDRMASK5_bm  (1<<6)  /* Address Mask bit 5 mask. */
#define TWI_SLAVE_ADDRMASK5_bp  6  /* Address Mask bit 5 position. */
#define TWI_SLAVE_ADDRMASK6_bm  (1<<7)  /* Address Mask bit 6 mask. */
#define TWI_SLAVE_ADDRMASK6_bp  7  /* Address Mask bit 6 position. */

#define TWI_SLAVE_ADDREN_bm  0x01  /* Address Enable bit mask. */
#define TWI_SLAVE_ADDREN_bp  0  /* Address Enable bit position. */


/* TWI.CTRL  bit masks and bit positions */
#define TWI_SDAHOLD_gm  0x06  /* SDA Hold Time Enable group mask. */
#define TWI_SDAHOLD_gp  1  /* SDA Hold Time Enable group position. */
#define TWI_SDAHOLD0_bm  (1<<1)  /* SDA Hold Time Enable bit 0 mask. */
#define TWI_SDAHOLD0_bp  1  /* SDA Hold Time Enable bit 0 position. */
#define TWI_SDAHOLD1_bm  (1<<2)  /* SDA Hold Time Enable bit 1 mask. */
#define TWI_SDAHOLD1_bp  2  /* SDA Hold Time Enable bit 1 position. */

#define TWI_EDIEN_bm  0x01  /* External Driver Interface Enable bit mask. */
#define TWI_EDIEN_bp  0  /* External Driver Interface Enable bit position. */


/* VADC - Voltage ADC */
/* VADC.CTRLA  bit masks and bit positions */
#define VADC_STARTCONV_bm  0x01  /* Start Conversion bit mask. */
#define VADC_STARTCONV_bp  0  /* Start Conversion bit position. */


/* VADC.CTRLB  bit masks and bit positions */
#define VADC_PILVL_gm  0x60  /* Protection Scan Complete Interrupt Level group mask. */
#define VADC_PILVL_gp  5  /* Protection Scan Complete Interrupt Level group position. */
#define VADC_PILVL0_bm  (1<<5)  /* Protection Scan Complete Interrupt Level bit 0 mask. */
#define VADC_PILVL0_bp  5  /* Protection Scan Complete Interrupt Level bit 0 position. */
#define VADC_PILVL1_bm  (1<<6)  /* Protection Scan Complete Interrupt Level bit 1 mask. */
#define VADC_PILVL1_bp  6  /* Protection Scan Complete Interrupt Level bit 1 position. */

#define VADC_CILVL_gm  0x18  /* Conversion Complete Interrupt Level group mask. */
#define VADC_CILVL_gp  3  /* Conversion Complete Interrupt Level group position. */
#define VADC_CILVL0_bm  (1<<3)  /* Conversion Complete Interrupt Level bit 0 mask. */
#define VADC_CILVL0_bp  3  /* Conversion Complete Interrupt Level bit 0 position. */
#define VADC_CILVL1_bm  (1<<4)  /* Conversion Complete Interrupt Level bit 1 mask. */
#define VADC_CILVL1_bp  4  /* Conversion Complete Interrupt Level bit 1 position. */

#define VADC_CHOPEN_bm  0x04  /* Chopping Enable bit mask. */
#define VADC_CHOPEN_bp  2  /* Chopping Enable bit position. */

#define VADC_DECIR_gm  0x03  /* Decimation Ratio group mask. */
#define VADC_DECIR_gp  0  /* Decimation Ratio group position. */
#define VADC_DECIR0_bm  (1<<0)  /* Decimation Ratio bit 0 mask. */
#define VADC_DECIR0_bp  0  /* Decimation Ratio bit 0 position. */
#define VADC_DECIR1_bm  (1<<1)  /* Decimation Ratio bit 1 mask. */
#define VADC_DECIR1_bp  1  /* Decimation Ratio bit 1 position. */


/* VADC.DIAGNOSIS  bit masks and bit positions */
#define VADC_PVDIAG_gm  0x0C  /* PV Diagnostic Configuration group mask. */
#define VADC_PVDIAG_gp  2  /* PV Diagnostic Configuration group position. */
#define VADC_PVDIAG0_bm  (1<<2)  /* PV Diagnostic Configuration bit 0 mask. */
#define VADC_PVDIAG0_bp  2  /* PV Diagnostic Configuration bit 0 position. */
#define VADC_PVDIAG1_bm  (1<<3)  /* PV Diagnostic Configuration bit 1 mask. */
#define VADC_PVDIAG1_bp  3  /* PV Diagnostic Configuration bit 1 position. */

#define VADC_NVDIAG_gm  0x03  /* NV Diagnostic Configuration group mask. */
#define VADC_NVDIAG_gp  0  /* NV Diagnostic Configuration group position. */
#define VADC_NVDIAG0_bm  (1<<0)  /* NV Diagnostic Configuration bit 0 mask. */
#define VADC_NVDIAG0_bp  0  /* NV Diagnostic Configuration bit 0 position. */
#define VADC_NVDIAG1_bm  (1<<1)  /* NV Diagnostic Configuration bit 1 mask. */
#define VADC_NVDIAG1_bp  1  /* NV Diagnostic Configuration bit 1 position. */


/* VADC.SCANSETUP  bit masks and bit positions */
#define VADC_CHANNELSEL_gm  0x0F  /* Channel Select group mask. */
#define VADC_CHANNELSEL_gp  0  /* Channel Select group position. */
#define VADC_CHANNELSEL0_bm  (1<<0)  /* Channel Select bit 0 mask. */
#define VADC_CHANNELSEL0_bp  0  /* Channel Select bit 0 position. */
#define VADC_CHANNELSEL1_bm  (1<<1)  /* Channel Select bit 1 mask. */
#define VADC_CHANNELSEL1_bp  1  /* Channel Select bit 1 position. */
#define VADC_CHANNELSEL2_bm  (1<<2)  /* Channel Select bit 2 mask. */
#define VADC_CHANNELSEL2_bp  2  /* Channel Select bit 2 position. */
#define VADC_CHANNELSEL3_bm  (1<<3)  /* Channel Select bit 3 mask. */
#define VADC_CHANNELSEL3_bp  3  /* Channel Select bit 3 position. */


/* VADC.PROTSETUPA  bit masks and bit positions */
/* VADC_CHANNELSEL_gm  Predefined. */
/* VADC_CHANNELSEL_gp  Predefined. */
/* VADC_CHANNELSEL0_bm  Predefined. */
/* VADC_CHANNELSEL0_bp  Predefined. */
/* VADC_CHANNELSEL1_bm  Predefined. */
/* VADC_CHANNELSEL1_bp  Predefined. */
/* VADC_CHANNELSEL2_bm  Predefined. */
/* VADC_CHANNELSEL2_bp  Predefined. */
/* VADC_CHANNELSEL3_bm  Predefined. */
/* VADC_CHANNELSEL3_bp  Predefined. */


/* VADC.PROTSETUPB  bit masks and bit positions */
#define VADC_TIMING_gm  0x07  /* Protection Timing group mask. */
#define VADC_TIMING_gp  0  /* Protection Timing group position. */
#define VADC_TIMING0_bm  (1<<0)  /* Protection Timing bit 0 mask. */
#define VADC_TIMING0_bp  0  /* Protection Timing bit 0 position. */
#define VADC_TIMING1_bm  (1<<1)  /* Protection Timing bit 1 mask. */
#define VADC_TIMING1_bp  1  /* Protection Timing bit 1 position. */
#define VADC_TIMING2_bm  (1<<2)  /* Protection Timing bit 2 mask. */
#define VADC_TIMING2_bp  2  /* Protection Timing bit 2 position. */


/* VADC.STATUS  bit masks and bit positions */
#define VADC_PROTFIF_bm  0x08  /* Protection Scan Finished Interrupt Flag bit mask. */
#define VADC_PROTFIF_bp  3  /* Protection Scan Finished Interrupt Flag bit position. */

#define VADC_PROTSF_bm  0x04  /* Protection Scan Started Flag bit mask. */
#define VADC_PROTSF_bp  2  /* Protection Scan Started Flag bit position. */

#define VADC_ABORTED_bm  0x02  /* Conversion Aborted Flag bit mask. */
#define VADC_ABORTED_bp  1  /* Conversion Aborted Flag bit position. */

#define VADC_CONVIF_bm  0x01  /* Conversion Complete Interrupt Flag bit mask. */
#define VADC_CONVIF_bp  0  /* Conversion Complete Interrupt Flag bit position. */


/* WDT - Watch-Dog Timer */
/* WDT.CTRL  bit masks and bit positions */
#define WDT_PER_gm  0x3C  /* Period group mask. */
#define WDT_PER_gp  2  /* Period group position. */
#define WDT_PER0_bm  (1<<2)  /* Period bit 0 mask. */
#define WDT_PER0_bp  2  /* Period bit 0 position. */
#define WDT_PER1_bm  (1<<3)  /* Period bit 1 mask. */
#define WDT_PER1_bp  3  /* Period bit 1 position. */
#define WDT_PER2_bm  (1<<4)  /* Period bit 2 mask. */
#define WDT_PER2_bp  4  /* Period bit 2 position. */
#define WDT_PER3_bm  (1<<5)  /* Period bit 3 mask. */
#define WDT_PER3_bp  5  /* Period bit 3 position. */

#define WDT_ENABLE_bm  0x02  /* Enable bit mask. */
#define WDT_ENABLE_bp  1  /* Enable bit position. */

#define WDT_CEN_bm  0x01  /* Change Enable bit mask. */
#define WDT_CEN_bp  0  /* Change Enable bit position. */


/* WDT.WINCTRL  bit masks and bit positions */
#define WDT_WPER_gm  0x3C  /* Windowed Mode Period group mask. */
#define WDT_WPER_gp  2  /* Windowed Mode Period group position. */
#define WDT_WPER0_bm  (1<<2)  /* Windowed Mode Period bit 0 mask. */
#define WDT_WPER0_bp  2  /* Windowed Mode Period bit 0 position. */
#define WDT_WPER1_bm  (1<<3)  /* Windowed Mode Period bit 1 mask. */
#define WDT_WPER1_bp  3  /* Windowed Mode Period bit 1 position. */
#define WDT_WPER2_bm  (1<<4)  /* Windowed Mode Period bit 2 mask. */
#define WDT_WPER2_bp  4  /* Windowed Mode Period bit 2 position. */
#define WDT_WPER3_bm  (1<<5)  /* Windowed Mode Period bit 3 mask. */
#define WDT_WPER3_bp  5  /* Windowed Mode Period bit 3 position. */

#define WDT_WEN_bm  0x02  /* Windowed Mode Enable bit mask. */
#define WDT_WEN_bp  1  /* Windowed Mode Enable bit position. */

#define WDT_WCEN_bm  0x01  /* Windowed Mode Change Enable bit mask. */
#define WDT_WCEN_bp  0  /* Windowed Mode Change Enable bit position. */


/* WDT.STATUS  bit masks and bit positions */
#define WDT_SYNCBUSY_bm  0x01  /* Syncronization busy bit mask. */
#define WDT_SYNCBUSY_bp  0  /* Syncronization busy bit position. */


/* XMEGAX1_FUSES - Fuses and Lockbits */
/* NVM_LOCKBITS.LOCKBITS  bit masks and bit positions */
#define NVM_LOCKBITS_BLBB_gm  0xC0  /* Boot Lock Bits - Boot Section group mask. */
#define NVM_LOCKBITS_BLBB_gp  6  /* Boot Lock Bits - Boot Section group position. */
#define NVM_LOCKBITS_BLBB0_bm  (1<<6)  /* Boot Lock Bits - Boot Section bit 0 mask. */
#define NVM_LOCKBITS_BLBB0_bp  6  /* Boot Lock Bits - Boot Section bit 0 position. */
#define NVM_LOCKBITS_BLBB1_bm  (1<<7)  /* Boot Lock Bits - Boot Section bit 1 mask. */
#define NVM_LOCKBITS_BLBB1_bp  7  /* Boot Lock Bits - Boot Section bit 1 position. */

#define NVM_LOCKBITS_BLBA_gm  0x30  /* Boot Lock Bits - Application Section group mask. */
#define NVM_LOCKBITS_BLBA_gp  4  /* Boot Lock Bits - Application Section group position. */
#define NVM_LOCKBITS_BLBA0_bm  (1<<4)  /* Boot Lock Bits - Application Section bit 0 mask. */
#define NVM_LOCKBITS_BLBA0_bp  4  /* Boot Lock Bits - Application Section bit 0 position. */
#define NVM_LOCKBITS_BLBA1_bm  (1<<5)  /* Boot Lock Bits - Application Section bit 1 mask. */
#define NVM_LOCKBITS_BLBA1_bp  5  /* Boot Lock Bits - Application Section bit 1 position. */

#define NVM_LOCKBITS_BLBAT_gm  0x0C  /* Boot Lock Bits - Application Table group mask. */
#define NVM_LOCKBITS_BLBAT_gp  2  /* Boot Lock Bits - Application Table group position. */
#define NVM_LOCKBITS_BLBAT0_bm  (1<<2)  /* Boot Lock Bits - Application Table bit 0 mask. */
#define NVM_LOCKBITS_BLBAT0_bp  2  /* Boot Lock Bits - Application Table bit 0 position. */
#define NVM_LOCKBITS_BLBAT1_bm  (1<<3)  /* Boot Lock Bits - Application Table bit 1 mask. */
#define NVM_LOCKBITS_BLBAT1_bp  3  /* Boot Lock Bits - Application Table bit 1 position. */

#define NVM_LOCKBITS_LB_gm  0x03  /* Lock Bits group mask. */
#define NVM_LOCKBITS_LB_gp  0  /* Lock Bits group position. */
#define NVM_LOCKBITS_LB0_bm  (1<<0)  /* Lock Bits bit 0 mask. */
#define NVM_LOCKBITS_LB0_bp  0  /* Lock Bits bit 0 position. */
#define NVM_LOCKBITS_LB1_bm  (1<<1)  /* Lock Bits bit 1 mask. */
#define NVM_LOCKBITS_LB1_bp  1  /* Lock Bits bit 1 position. */


/* NVM_FUSES.FUSEBYTE1  bit masks and bit positions */
#define NVM_FUSES_WDWP_gm  0xF0  /* Watchdog Window Timeout Period group mask. */
#define NVM_FUSES_WDWP_gp  4  /* Watchdog Window Timeout Period group position. */
#define NVM_FUSES_WDWP0_bm  (1<<4)  /* Watchdog Window Timeout Period bit 0 mask. */
#define NVM_FUSES_WDWP0_bp  4  /* Watchdog Window Timeout Period bit 0 position. */
#define NVM_FUSES_WDWP1_bm  (1<<5)  /* Watchdog Window Timeout Period bit 1 mask. */
#define NVM_FUSES_WDWP1_bp  5  /* Watchdog Window Timeout Period bit 1 position. */
#define NVM_FUSES_WDWP2_bm  (1<<6)  /* Watchdog Window Timeout Period bit 2 mask. */
#define NVM_FUSES_WDWP2_bp  6  /* Watchdog Window Timeout Period bit 2 position. */
#define NVM_FUSES_WDWP3_bm  (1<<7)  /* Watchdog Window Timeout Period bit 3 mask. */
#define NVM_FUSES_WDWP3_bp  7  /* Watchdog Window Timeout Period bit 3 position. */

#define NVM_FUSES_WDP_gm  0x0F  /* Watchdog Timeout Period group mask. */
#define NVM_FUSES_WDP_gp  0  /* Watchdog Timeout Period group position. */
#define NVM_FUSES_WDP0_bm  (1<<0)  /* Watchdog Timeout Period bit 0 mask. */
#define NVM_FUSES_WDP0_bp  0  /* Watchdog Timeout Period bit 0 position. */
#define NVM_FUSES_WDP1_bm  (1<<1)  /* Watchdog Timeout Period bit 1 mask. */
#define NVM_FUSES_WDP1_bp  1  /* Watchdog Timeout Period bit 1 position. */
#define NVM_FUSES_WDP2_bm  (1<<2)  /* Watchdog Timeout Period bit 2 mask. */
#define NVM_FUSES_WDP2_bp  2  /* Watchdog Timeout Period bit 2 position. */
#define NVM_FUSES_WDP3_bm  (1<<3)  /* Watchdog Timeout Period bit 3 mask. */
#define NVM_FUSES_WDP3_bp  3  /* Watchdog Timeout Period bit 3 position. */


/* NVM_FUSES.FUSEBYTE2  bit masks and bit positions */
#define NVM_FUSES_BOOTRST_bm  0x40  /* Boot Loader Section Reset Vector bit mask. */
#define NVM_FUSES_BOOTRST_bp  6  /* Boot Loader Section Reset Vector bit position. */


/* NVM_FUSES.FUSEBYTE4  bit masks and bit positions */
#define NVM_FUSES_RSTDISBL_bm  0x10  /* External Reset Disable bit mask. */
#define NVM_FUSES_RSTDISBL_bp  4  /* External Reset Disable bit position. */

#define NVM_FUSES_SUT_gm  0x0C  /* Start-up Time group mask. */
#define NVM_FUSES_SUT_gp  2  /* Start-up Time group position. */
#define NVM_FUSES_SUT0_bm  (1<<2)  /* Start-up Time bit 0 mask. */
#define NVM_FUSES_SUT0_bp  2  /* Start-up Time bit 0 position. */
#define NVM_FUSES_SUT1_bm  (1<<3)  /* Start-up Time bit 1 mask. */
#define NVM_FUSES_SUT1_bp  3  /* Start-up Time bit 1 position. */

#define NVM_FUSES_WDLOCK_bm  0x02  /* Watchdog Timer Lock bit mask. */
#define NVM_FUSES_WDLOCK_bp  1  /* Watchdog Timer Lock bit position. */


/* NVM_FUSES.FUSEBYTE5  bit masks and bit positions */
#define NVM_FUSES_AUTOLD_bm  0x08  /* Autoload select value bit mask. */
#define NVM_FUSES_AUTOLD_bp  3  /* Autoload select value bit position. */

#define NVM_FUSES_EESAVE_bm  0x08  /* Preserve EEPROM Through Chip Erase bit mask. */
#define NVM_FUSES_EESAVE_bp  3  /* Preserve EEPROM Through Chip Erase bit position. */


/* XOCD - On-Chip Debug System */
/* OCD.OCDR1  bit masks and bit positions */
#define OCD_OCDRD_bm  0x01  /* OCDR Dirty bit mask. */
#define OCD_OCDRD_bp  0  /* OCDR Dirty bit position. */


/* PORT - I/O Port Configuration */
/* PORT.INTCTRL  bit masks and bit positions */
#define PORT_INT1LVL_gm  0x0C  /* Port Interrupt 1 Level group mask. */
#define PORT_INT1LVL_gp  2  /* Port Interrupt 1 Level group position. */
#define PORT_INT1LVL0_bm  (1<<2)  /* Port Interrupt 1 Level bit 0 mask. */
#define PORT_INT1LVL0_bp  2  /* Port Interrupt 1 Level bit 0 position. */
#define PORT_INT1LVL1_bm  (1<<3)  /* Port Interrupt 1 Level bit 1 mask. */
#define PORT_INT1LVL1_bp  3  /* Port Interrupt 1 Level bit 1 position. */

#define PORT_INT0LVL_gm  0x03  /* Port Interrupt 0 Level group mask. */
#define PORT_INT0LVL_gp  0  /* Port Interrupt 0 Level group position. */
#define PORT_INT0LVL0_bm  (1<<0)  /* Port Interrupt 0 Level bit 0 mask. */
#define PORT_INT0LVL0_bp  0  /* Port Interrupt 0 Level bit 0 position. */
#define PORT_INT0LVL1_bm  (1<<1)  /* Port Interrupt 0 Level bit 1 mask. */
#define PORT_INT0LVL1_bp  1  /* Port Interrupt 0 Level bit 1 position. */


/* PORT.INTFLAGS  bit masks and bit positions */
#define PORT_INT1IF_bm  0x02  /* Port Interrupt 1 Flag bit mask. */
#define PORT_INT1IF_bp  1  /* Port Interrupt 1 Flag bit position. */

#define PORT_INT0IF_bm  0x01  /* Port Interrupt 0 Flag bit mask. */
#define PORT_INT0IF_bp  0  /* Port Interrupt 0 Flag bit position. */


/* PORT.PIN0CTRL  bit masks and bit positions */
#define PORT_SRLEN_bm  0x80  /* Slew Rate Enable bit mask. */
#define PORT_SRLEN_bp  7  /* Slew Rate Enable bit position. */

#define PORT_INVEN_bm  0x40  /* Inverted I/O Enable bit mask. */
#define PORT_INVEN_bp  6  /* Inverted I/O Enable bit position. */

#define PORT_OPC_gm  0x38  /* Output/Pull Configuration group mask. */
#define PORT_OPC_gp  3  /* Output/Pull Configuration group position. */
#define PORT_OPC0_bm  (1<<3)  /* Output/Pull Configuration bit 0 mask. */
#define PORT_OPC0_bp  3  /* Output/Pull Configuration bit 0 position. */
#define PORT_OPC1_bm  (1<<4)  /* Output/Pull Configuration bit 1 mask. */
#define PORT_OPC1_bp  4  /* Output/Pull Configuration bit 1 position. */
#define PORT_OPC2_bm  (1<<5)  /* Output/Pull Configuration bit 2 mask. */
#define PORT_OPC2_bp  5  /* Output/Pull Configuration bit 2 position. */

#define PORT_ISC_gm  0x07  /* Input/Sense Configuration group mask. */
#define PORT_ISC_gp  0  /* Input/Sense Configuration group position. */
#define PORT_ISC0_bm  (1<<0)  /* Input/Sense Configuration bit 0 mask. */
#define PORT_ISC0_bp  0  /* Input/Sense Configuration bit 0 position. */
#define PORT_ISC1_bm  (1<<1)  /* Input/Sense Configuration bit 1 mask. */
#define PORT_ISC1_bp  1  /* Input/Sense Configuration bit 1 position. */
#define PORT_ISC2_bm  (1<<2)  /* Input/Sense Configuration bit 2 mask. */
#define PORT_ISC2_bp  2  /* Input/Sense Configuration bit 2 position. */


/* PORT.PIN1CTRL  bit masks and bit positions */
/* PORT_SRLEN_bm  Predefined. */
/* PORT_SRLEN_bp  Predefined. */

/* PORT_INVEN_bm  Predefined. */
/* PORT_INVEN_bp  Predefined. */

/* PORT_OPC_gm  Predefined. */
/* PORT_OPC_gp  Predefined. */
/* PORT_OPC0_bm  Predefined. */
/* PORT_OPC0_bp  Predefined. */
/* PORT_OPC1_bm  Predefined. */
/* PORT_OPC1_bp  Predefined. */
/* PORT_OPC2_bm  Predefined. */
/* PORT_OPC2_bp  Predefined. */

/* PORT_ISC_gm  Predefined. */
/* PORT_ISC_gp  Predefined. */
/* PORT_ISC0_bm  Predefined. */
/* PORT_ISC0_bp  Predefined. */
/* PORT_ISC1_bm  Predefined. */
/* PORT_ISC1_bp  Predefined. */
/* PORT_ISC2_bm  Predefined. */
/* PORT_ISC2_bp  Predefined. */


/* PORT.PIN2CTRL  bit masks and bit positions */
/* PORT_SRLEN_bm  Predefined. */
/* PORT_SRLEN_bp  Predefined. */

/* PORT_INVEN_bm  Predefined. */
/* PORT_INVEN_bp  Predefined. */

/* PORT_OPC_gm  Predefined. */
/* PORT_OPC_gp  Predefined. */
/* PORT_OPC0_bm  Predefined. */
/* PORT_OPC0_bp  Predefined. */
/* PORT_OPC1_bm  Predefined. */
/* PORT_OPC1_bp  Predefined. */
/* PORT_OPC2_bm  Predefined. */
/* PORT_OPC2_bp  Predefined. */

/* PORT_ISC_gm  Predefined. */
/* PORT_ISC_gp  Predefined. */
/* PORT_ISC0_bm  Predefined. */
/* PORT_ISC0_bp  Predefined. */
/* PORT_ISC1_bm  Predefined. */
/* PORT_ISC1_bp  Predefined. */
/* PORT_ISC2_bm  Predefined. */
/* PORT_ISC2_bp  Predefined. */


/* VPORT - Virtual Ports */
/* VPORT.INTFLAGS  bit masks and bit positions */
#define VPORT_INT1IF_bm  0x02  /* Port Interrupt 1 Flag bit mask. */
#define VPORT_INT1IF_bp  1  /* Port Interrupt 1 Flag bit position. */

#define VPORT_INT0IF_bm  0x01  /* Port Interrupt 0 Flag bit mask. */
#define VPORT_INT0IF_bp  0  /* Port Interrupt 0 Flag bit position. */



// Generic Port Pins

#define PIN0_bm 0x01
#define PIN0_bp 0
#define PIN1_bm 0x02
#define PIN1_bp 1
#define PIN2_bm 0x04
#define PIN2_bp 2
#define PIN3_bm 0x08
#define PIN3_bp 3
#define PIN4_bm 0x10
#define PIN4_bp 4
#define PIN5_bm 0x20
#define PIN5_bp 5
#define PIN6_bm 0x40
#define PIN6_bp 6
#define PIN7_bm 0x80
#define PIN7_bp 7


/* ========== Interrupt Vector Definitions ========== */
/* Vector 0 is the reset vector */

/* FET interrupt vectors */
#define FET_PROT_vect_num  1
#define FET_PROT_vect      _VECTOR(1)  /* Protection Interrupt */

/* VREF interrupt vectors */
#define VREF_VREF_vect_num  2
#define VREF_VREF_vect      _VECTOR(2)  /* VREF Interrupt */

/* LDO interrupt vectors */
#define LDO_LDO_WARN_vect_num  3
#define LDO_LDO_WARN_vect      _VECTOR(3)  /* LDO Warning Interrupt */

/* PORTA interrupt vectors */
#define PORTA_INT0_vect_num  4
#define PORTA_INT0_vect      _VECTOR(4)  /* External Interrupt 0 */
#define PORTA_INT1_vect_num  5
#define PORTA_INT1_vect      _VECTOR(5)  /* External Interrupt 1 */

/* PORTC interrupt vectors */
#define PORTC_INT0_vect_num  6
#define PORTC_INT0_vect      _VECTOR(6)  /* External Interrupt 0 */
#define PORTC_INT1_vect_num  7
#define PORTC_INT1_vect      _VECTOR(7)  /* External Interrupt 1 */

/* TIMER interrupt vectors */
#define TIMER_OVF_vect_num  8
#define TIMER_OVF_vect      _VECTOR(8)  /* Overflow Interrupt */
#define TIMER_OCFA_vect_num  9
#define TIMER_OCFA_vect      _VECTOR(9)  /* Output Compare A Interrupt */
#define TIMER_OCFB_vect_num  10
#define TIMER_OCFB_vect      _VECTOR(10)  /* Output Compare B Interupt  */
#define TIMER_ICF_vect_num  11
#define TIMER_ICF_vect      _VECTOR(11)  /* Input Capture Interrupt */

/* BCD interrupt vectors */
#define BCD_BCD_vect_num  12
#define BCD_BCD_vect      _VECTOR(12)  /* Bus Connect/Disconnect Interrupt */

/* USARTC interrupt vectors */
#define USARTC_RXC_vect_num  13
#define USARTC_RXC_vect      _VECTOR(13)  /* Reception Complete Interrupt */
#define USARTC_DRE_vect_num  14
#define USARTC_DRE_vect      _VECTOR(14)  /* Data Register Empty Interrupt */
#define USARTC_TXC_vect_num  15
#define USARTC_TXC_vect      _VECTOR(15)  /* Transmission Complete Interrupt */

/* TWIC interrupt vectors */
#define TWIC_TWIS_vect_num  16
#define TWIC_TWIS_vect      _VECTOR(16)  /* TWI Slave Interrupt */

/* WUT interrupt vectors */

/* VADC interrupt vectors */
#define VADC_VADCCF_vect_num  19
#define VADC_VADCCF_vect      _VECTOR(19)  /* VADC conversion finished */
#define VADC_VADCPROTF_vect_num  20
#define VADC_VADCPROTF_vect      _VECTOR(20)  /* VADC protection scan finished */

/* CADC interrupt vectors */
#define CADC_CADCCF_vect_num  21
#define CADC_CADCCF_vect      _VECTOR(21)  /* CADC conversion finish interrupt */
#define CADC_CADCSM_vect_num  22
#define CADC_CADCSM_vect      _VECTOR(22)  /* CADC sample mode interrupt */

/* NVM interrupt vectors */
#define NVM_EE_vect_num  22
#define NVM_EE_vect      _VECTOR(22)  /* EE Interrupt */
#define NVM_SPM_vect_num  23
#define NVM_SPM_vect      _VECTOR(23)  /* SPM Interrupt */


#define _VECTOR_SIZE 4 /* Size of individual vector. */
#define _VECTORS_SIZE (24 * _VECTOR_SIZE)


/* ========== Constants ========== */

#define PROGMEM_START     (0x0000)
#define PROGMEM_SIZE      (36864)
#define PROGMEM_PAGE_SIZE (256)
#define PROGMEM_END       (PROGMEM_START + PROGMEM_SIZE - 1)

#define APP_SECTION_START     (0x0000)
#define APP_SECTION_SIZE      (32768)
#define APP_SECTION_PAGE_SIZE (256)
#define APP_SECTION_END       (APP_SECTION_START + APP_SECTION_SIZE - 1)

#define APPTABLE_SECTION_START     (0x7000)
#define APPTABLE_SECTION_SIZE      (4096)
#define APPTABLE_SECTION_PAGE_SIZE (256)
#define APPTABLE_SECTION_END       (APPTABLE_SECTION_START + APPTABLE_SECTION_SIZE - 1)

#define BOOT_SECTION_START     (0x8000)
#define BOOT_SECTION_SIZE      (4096)
#define BOOT_SECTION_PAGE_SIZE (256)
#define BOOT_SECTION_END       (BOOT_SECTION_START + BOOT_SECTION_SIZE - 1)

#define DATAMEM_START     (0x0000)
#define DATAMEM_SIZE      (12288)
#define DATAMEM_PAGE_SIZE (0)
#define DATAMEM_END       (DATAMEM_START + DATAMEM_SIZE - 1)

#define IO_START     (0x0000)
#define IO_SIZE      (4096)
#define IO_PAGE_SIZE (0)
#define IO_END       (IO_START + IO_SIZE - 1)

#define INTERNAL_SRAM_START     (0x2000)
#define INTERNAL_SRAM_SIZE      (2048)
#define INTERNAL_SRAM_PAGE_SIZE (0)
#define INTERNAL_SRAM_END       (INTERNAL_SRAM_START + INTERNAL_SRAM_SIZE - 1)

#define MAPPED_EEPROM_START     (0x1000)
#define MAPPED_EEPROM_SIZE      (1024)
#define MAPPED_EEPROM_PAGE_SIZE (32)
#define MAPPED_EEPROM_END       (MAPPED_EEPROM_START + MAPPED_EEPROM_SIZE - 1)

#define EEPROM_START     (0x0000)
#define EEPROM_SIZE      (1024)
#define EEPROM_PAGE_SIZE (32)
#define EEPROM_END       (EEPROM_START + EEPROM_SIZE - 1)

#define FUSE_START     (0x0000)
#define FUSE_SIZE      (6)
#define FUSE_PAGE_SIZE (0)
#define FUSE_END       (FUSE_START + FUSE_SIZE - 1)

#define LOCKBIT_START     (0x0000)
#define LOCKBIT_SIZE      (1)
#define LOCKBIT_PAGE_SIZE (0)
#define LOCKBIT_END       (LOCKBIT_START + LOCKBIT_SIZE - 1)

#define SIGNATURES_START     (0x0000)
#define SIGNATURES_SIZE      (3)
#define SIGNATURES_PAGE_SIZE (0)
#define SIGNATURES_END       (SIGNATURES_START + SIGNATURES_SIZE - 1)

#define USER_SIGNATURES_START     (0x0000)
#define USER_SIGNATURES_SIZE      (256)
#define USER_SIGNATURES_PAGE_SIZE (0)
#define USER_SIGNATURES_END       (USER_SIGNATURES_START + USER_SIGNATURES_SIZE - 1)

#define PROD_SIGNATURES_START     (0x0000)
#define PROD_SIGNATURES_SIZE      (52)
#define PROD_SIGNATURES_PAGE_SIZE (0)
#define PROD_SIGNATURES_END       (PROD_SIGNATURES_START + PROD_SIGNATURES_SIZE - 1)

#define FLASHEND     PROGMEM_END
#define SPM_PAGESIZE PROGMEM_PAGE_SIZE
#define RAMSTART     INTERNAL_SRAM_START
#define RAMSIZE      INTERNAL_SRAM_SIZE
#define RAMEND       INTERNAL_SRAM_END
#define XRAMSTART    EXTERNAL_SRAM_START
#define XRAMSIZE     EXTERNAL_SRAM_SIZE
#define XRAMEND      INTERNAL_SRAM_END
#define E2END        EEPROM_END
#define E2PAGESIZE   EEPROM_PAGE_SIZE


/* ========== Fuses ========== */
#define FUSE_MEMORY_SIZE 6

/* Fuse Byte 0 */
#define FUSE0_DEFAULT  (0xFF)

/* Fuse Byte 1 */
#define FUSE_WDP0  (unsigned char)~_BV(0)  /* Watchdog Timeout Period Bit 0 */
#define FUSE_WDP1  (unsigned char)~_BV(1)  /* Watchdog Timeout Period Bit 1 */
#define FUSE_WDP2  (unsigned char)~_BV(2)  /* Watchdog Timeout Period Bit 2 */
#define FUSE_WDP3  (unsigned char)~_BV(3)  /* Watchdog Timeout Period Bit 3 */
#define FUSE_WDWP0  (unsigned char)~_BV(4)  /* Watchdog Window Timeout Period Bit 0 */
#define FUSE_WDWP1  (unsigned char)~_BV(5)  /* Watchdog Window Timeout Period Bit 1 */
#define FUSE_WDWP2  (unsigned char)~_BV(6)  /* Watchdog Window Timeout Period Bit 2 */
#define FUSE_WDWP3  (unsigned char)~_BV(7)  /* Watchdog Window Timeout Period Bit 3 */
#define FUSE1_DEFAULT  (0xFF)

/* Fuse Byte 2 */
#define FUSE_BOOTRST  (unsigned char)~_BV(6)  /* Boot Loader Section Reset Vector */
#define FUSE2_DEFAULT  (0xFF)

/* Fuse Byte 3 Reserved */

/* Fuse Byte 4 */
#define FUSE_WDLOCK  (unsigned char)~_BV(1)  /* Watchdog Timer Lock */
#define FUSE_SUT0  (unsigned char)~_BV(2)  /* Start-up Time Bit 0 */
#define FUSE_SUT1  (unsigned char)~_BV(3)  /* Start-up Time Bit 1 */
#define FUSE_RSTDISBL  (unsigned char)~_BV(4)  /* External Reset Disable */
#define FUSE4_DEFAULT  (0xFF)

/* Fuse Byte 5 */
#define FUSE_AUTOLD  (unsigned char)~_BV(3)  /* Autoload select value */
#define FUSE_EESAVE  (unsigned char)~_BV(3)  /* Preserve EEPROM Through Chip Erase */
#define FUSE5_DEFAULT  (0xFF)


/* ========== Lock Bits ========== */
#define __LOCK_BITS_EXIST
#define __BOOT_LOCK_APPLICATION_TABLE_BITS_EXIST
#define __BOOT_LOCK_APPLICATION_BITS_EXIST
#define __BOOT_LOCK_BOOT_BITS_EXIST


/* ========== Signature ========== */
#define SIGNATURE_0 0x00
#define SIGNATURE_1 0x00
#define SIGNATURE_2 0x00


#endif /* _AVR_ATxmega32X1_H_ */

