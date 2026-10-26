/* Copyright (c) 2011 Atmel Corporation
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

/* $Id$ */

/* avr/iotn1634.h - definitions for ATtiny1634 */

/* This file should only be included from <avr/io.h>, never directly. */

#ifndef _AVR_IO_H_
#  error "Include <avr/io.h> instead of this file."
#endif

#ifndef _AVR_IOXXX_H_
#  define _AVR_IOXXX_H_ "iotn1634.h"
#else
#  error "Attempt to include more than one <avr/ioXXX.h> file."
#endif 


#ifndef _AVR_ATtiny1634_H_
#define _AVR_ATtiny1634_H_ 1


/* Registers and associated bit numbers. */

#ifndef __ASSEMBLER__
#define ADC _SFR_IO16(0x00)
#endif
#define ADCW _SFR_IO16(0x00)

#define ADCL _SFR_IO8(0x00)
#define ADCL0 0
#define ADCL1 1
#define ADCL2 2
#define ADCL3 3
#define ADCL4 4
#define ADCL5 5
#define ADCL6 6
#define ADCL7 7

#define ADCH _SFR_IO8(0x01)
#define ADCH0 0
#define ADCH1 1
#define ADCH2 2
#define ADCH3 3
#define ADCH4 4
#define ADCH5 5
#define ADCH6 6
#define ADCH7 7

#define ADCSRB _SFR_IO8(0x02)
#define ADTS0 0
#define ADTS1 1
#define ADTS2 2
#define ADLAR 3

#define ADCSRA _SFR_IO8(0x03)
#define ADPS0 0
#define ADPS1 1
#define ADPS2 2
#define ADIE 3
#define ADIF 4
#define ADATE 5
#define ADSC 6
#define ADEN 7

#define ADMUX _SFR_IO8(0x04)
#define MUX0 0
#define MUX1 1
#define MUX2 2
#define MUX3 3
#define REFS0 6
#define REFS1 7

#define ACSRB _SFR_IO8(0x05)
#define ACME 2
#define HLEV 6
#define HSEL 7

#define ACSRA _SFR_IO8(0x06)
#define ACIS0 0
#define ACIS1 1
#define ACIC 2
#define ACIE 3
#define ACI 4
#define ACO 5
#define ACBG 6
#define ACD 7

#define PINC _SFR_IO8(0x07)
#define PINC0 0
#define PINC1 1
#define PINC2 2
#define PINC3 3
#define PINC4 4
#define PINC5 5

#define DDRC _SFR_IO8(0x08)
#define DDC0 0
#define DDC1 1
#define DDC2 2
#define DDC3 3
#define DDC4 4
#define DDC5 5

#define PORTC _SFR_IO8(0x09)
#define PORTC0 0
#define PORTC1 1
#define PORTC2 2
#define PORTC3 3
#define PORTC4 4
#define PORTC5 5

#define PUEC _SFR_IO8(0x0A)
#define PUEC0 0
#define PUEC1 1
#define PUEC2 2
#define PUEC3 3
#define PUEC4 4
#define PUEC5 5

#define PINB _SFR_IO8(0x0B)
#define PINB0 0
#define PINB1 1
#define PINB2 2
#define PINB3 3

#define DDRB _SFR_IO8(0x0C)
#define DDB0 0
#define DDB1 1
#define DDB2 2
#define DDB3 3

#define PORTB _SFR_IO8(0x0D)
#define PORTB0 0
#define PORTB1 1
#define PORTB2 2
#define PORTB3 3

#define PUEB _SFR_IO8(0x0E)
#define PUEB0 0
#define PUEB1 1
#define PUEB2 2
#define PUEB3 3

#define PINA _SFR_IO8(0x0F)
#define PINA0 0
#define PINA1 1
#define PINA2 2
#define PINA3 3
#define PINA4 4
#define PINA5 5
#define PINA6 6
#define PINA7 7

#define DDRA _SFR_IO8(0x10)
#define DDA0 0
#define DDA1 1
#define DDA2 2
#define DDA3 3
#define DDA4 4
#define DDA5 5
#define DDA6 6
#define DDA7 7

#define PORTA _SFR_IO8(0x11)
#define PORTA0 0
#define PORTA1 1
#define PORTA2 2
#define PORTA3 3
#define PORTA4 4
#define PORTA5 5
#define PORTA6 6
#define PORTA7 7

#define PUEA _SFR_IO8(0x12)
#define PUEA0 0
#define PUEA1 1
#define PUEA2 2
#define PUEA3 3
#define PUEA4 4
#define PUEA5 5
#define PUEA6 6
#define PUEA7 7

#define PORTCR _SFR_IO8(0x13)
#define BBMA 0
#define BBMB 1
#define BBMC 2

#define GPIOR0 _SFR_IO8(0x14)
#define GPIOR00 0
#define GPIOR01 1
#define GPIOR02 2
#define GPIOR03 3
#define GPIOR04 4
#define GPIOR05 5
#define GPIOR06 6
#define GPIOR07 7

#define GPIOR1 _SFR_IO8(0x15)
#define GPIOR10 0
#define GPIOR11 1
#define GPIOR12 2
#define GPIOR13 3
#define GPIOR14 4
#define GPIOR15 5
#define GPIOR16 6
#define GPIOR17 7

#define GPIOR2 _SFR_IO8(0x16)
#define GPIOR20 0
#define GPIOR21 1
#define GPIOR22 2
#define GPIOR23 3
#define GPIOR24 4
#define GPIOR25 5
#define GPIOR26 6
#define GPIOR27 7

#define OCR0B _SFR_IO8(0x17)
#define OCR0_0 0
#define OCR0_1 1
#define OCR0_2 2
#define OCR0_3 3
#define OCR0_4 4
#define OCR0_5 5
#define OCR0_6 6
#define OCR0_7 7

#define OCR0A _SFR_IO8(0x18)
#define OCR0A_0 0
#define OCR0A_1 1
#define OCR0A_2 2
#define OCR0A_3 3
#define OCR0A_4 4
#define OCR0A_5 5
#define OCR0A_6 6
#define OCR0A_7 7

#define TCNT0 _SFR_IO8(0x19)
#define TCNT0_0 0
#define TCNT0_1 1
#define TCNT0_2 2
#define TCNT0_3 3
#define TCNT0_4 4
#define TCNT0_5 5
#define TCNT0_6 6
#define TCNT0_7 7

#define TCCR0B _SFR_IO8(0x1A)
#define CS00 0
#define CS01 1
#define CS02 2
#define WGM02 3
#define FOC0B 6
#define FOC0A 7

#define TCCR0A _SFR_IO8(0x1B)
#define WGM00 0
#define WGM01 1
#define COM0B0 4
#define COM0B1 5
#define COM0A0 6
#define COM0A1 7

#define EECR _SFR_IO8(0x1C)
#define EERE 0
#define EEPE 1
#define EEMPE 2
#define EERIE 3
#define EEPM0 4
#define EEPM1 5

#define EEDR _SFR_IO8(0x1D)
#define EEDR0 0
#define EEDR1 1
#define EEDR2 2
#define EEDR3 3
#define EEDR4 4
#define EEDR5 5
#define EEDR6 6
#define EEDR7 7

#define EEAR _SFR_IO8(0x1E)
#define EEAR0 0
#define EEAR1 1
#define EEAR2 2
#define EEAR3 3
#define EEAR4 4
#define EEAR5 5
#define EEAR6 6
#define EEAR7 7

#define UDR0 _SFR_IO8(0x20)
#define UDR0_0 0
#define UDR0_1 1
#define UDR0_2 2
#define UDR0_3 3
#define UDR0_4 4
#define UDR0_5 5
#define UDR0_6 6
#define UDR0_7 7

#define UBRR0 _SFR_IO16(0x21)

#define UBRR0L _SFR_IO8(0x21)
#define _UBRR0 0
#define _UBRR1 1
#define UBRR2 2
#define UBRR3 3
#define UBRR4 4
#define UBRR5 5
#define UBRR6 6
#define UBRR7 7

#define UBRR0H _SFR_IO8(0x22)
#define UBRR8 0
#define UBRR9 1
#define UBRR10 2
#define UBRR11 3

#define UCSR0D _SFR_IO8(0x23)
#define SFDE0 5
#define RXS0 6
#define RXSIE0 7

#define UCSR0C _SFR_IO8(0x24)
#define UCPOL0 0
#define UCSZ00 1
#define UCSZ01 2
#define USBS0 3
#define UPM00 4
#define UPM01 5
#define UMSEL00 6
#define UMSEL01 7

#define UCSR0B _SFR_IO8(0x25)
#define TXB80 0
#define RXB80 1
#define UCSZ02 2
#define TXEN0 3
#define RXEN0 4
#define UDRIE0 5
#define TXCIE0 6
#define RXCIE0 7

#define UCSR0A _SFR_IO8(0x26)
#define MPCM0 0
#define U2X0 1
#define UPE0 2
#define DOR0 3
#define FE0 4
#define UDRE0 5
#define TXC0 6
#define RXC0 7

#define PCMSK0 _SFR_IO8(0x27)
#define PCINT0 0
#define PCINT1 1
#define PCINT2 2
#define PCINT3 3
#define PCINT4 4
#define PCINT5 5
#define PCINT6 6
#define PCINT7 7

#define PCMSK1 _SFR_IO8(0x28)
#define PCINT8 0
#define PCINT9 1
#define PCINT10 2
#define PCINT11 3

#define PCMSK2 _SFR_IO8(0x29)
#define PCINT12 0
#define PCINT13 1
#define PCINT14 2
#define PCINT15 3
#define PCINT16 4
#define PCINT17 5

#define USIBR _SFR_IO8(0x2A)
#define USIBR0 0
#define USIBR1 1
#define USIBR2 2
#define USIBR3 3
#define USIBR4 4
#define USIBR5 5
#define USIBR6 6
#define USIBR7 7

#define USIDR _SFR_IO8(0x2B)
#define USIDR0 0
#define USIDR1 1
#define USIDR2 2
#define USIDR3 3
#define USIDR4 4
#define USIDR5 5
#define USIDR6 6
#define USIDR7 7

#define USICR _SFR_IO8(0x2C)
#define USITC 0
#define USICLK 1
#define USICS0 2
#define USICS1 3
#define USIWM0 4
#define USIWM1 5
#define USIOIE 6
#define USISIE 7

#define USISR _SFR_IO8(0x2D)
#define USICNT0 0
#define USICNT1 1
#define USICNT2 2
#define USICNT3 3
#define USIDC 4
#define USIPF 5
#define USIOIF 6
#define USISIF 7

#define ICR1 _SFR_IO16(0x2E)

#define ICR1L _SFR_IO8(0x2E)
#define ICR1L0 0
#define ICR1L1 1
#define ICR1L2 2
#define ICR1L3 3
#define ICR1L4 4
#define ICR1L5 5
#define ICR1L6 6
#define ICR1L7 7

#define ICR1H _SFR_IO8(0x2E)
#define ICR1H0 0
#define ICR1H1 1
#define ICR1H2 2
#define ICR1H3 3
#define ICR1H4 4
#define ICR1H5 5
#define ICR1H6 6
#define ICR1H7 7

#define OCR1B _SFR_IO16(0x2E)

#define OCR1BL _SFR_IO8(0x2E)
#define OCR1BL0 0
#define OCR1BL1 1
#define OCR1BL2 2
#define OCR1BL3 3
#define OCR1BL4 4
#define OCR1BL5 5
#define OCR1BL6 6
#define OCR1BL7 7

#define OCR1BH _SFR_IO8(0x2E)
#define OCR1BH0 0
#define OCR1BH1 1
#define OCR1BH2 2
#define OCR1BH3 3
#define OCR1BH4 4
#define OCR1BH5 5
#define OCR1BH6 6
#define OCR1BH7 7

#define CCP _SFR_IO8(0x2F)
#define CCP0 0
#define CCP1 1
#define CCP2 2
#define CCP3 3
#define CCP4 4
#define CCP5 5
#define CCP6 6
#define CCP7 7

#define WDTCSR _SFR_IO8(0x30)
#define WDP0 0
#define WDP1 1
#define WDP2 2
#define WDE 3
#define WDP3 5
#define WDIE 6
#define WDIF 7

#define CLKSR _SFR_IO8(0x32)
#define CKSEL0 0
#define CKSEL1 1
#define CKSEL2 2
#define CKSEL3 3
#define SUT 4
#define CKOUT_IO 5
#define CSTR 6
#define OSCRDY 7

#define CLKPR _SFR_IO8(0x33)
#define CLKPS0 0
#define CLKPS1 1
#define CLKPS2 2
#define CLKPS3 3

#define PRR _SFR_IO8(0x34)
#define PRADC 0
#define PRUSART0 1
#define PRUSART1 2
#define PRUSI 3
#define PRTIM0 4
#define PRTIM1 5
#define PRTWI 6

#define MCUSR _SFR_IO8(0x35)
#define PORF 0
#define EXTRF 1
#define BORF 2
#define WDRF 3

#define MCUCR _SFR_IO8(0x36)
#define ISC00 0
#define ISC01 1
#define SE 4
#define SM0 5
#define SM1 6

#define SPMCSR _SFR_IO8(0x37)

#define TIFR _SFR_IO8(0x39)
#define OCF0A 0
#define TOV0 1
#define OCF0B 2
#define ICF1 3
#define OCF1B 5
#define OCF1A 6
#define TOV1 7

#define TIMSK _SFR_IO8(0x3A)
#define OCIE0A 0
#define TOIE0 1
#define OCIE0B 2
#define ICIE1 3
#define OCIE1B 5
#define OCIE1A 6
#define TOIE1 7

#define GIFR _SFR_IO8(0x3B)
#define PCIF0 3
#define PCIF1 4
#define PCIF2 5
#define INTF0 6

#define GIMSK _SFR_IO8(0x3C)
#define PCIE0 3
#define PCIE1 4
#define PCIE2 5
#define INT0 6

#define DIDR0 _SFR_MEM8(0x60)
#define AREFD 0
#define AIN0D 1
#define AIN1D 2
#define ADC0D 3
#define ADC1D 4
#define ADC2D 5
#define ADC3D 6
#define ADC4D 7

#define DIDR1 _SFR_MEM8(0x61)
#define ADC5D 0
#define ADC6D 1
#define ADC7D 2
#define ADC8D 3

#define DIDR2 _SFR_MEM8(0x62)
#define ADC9D 0
#define ADC10D 1
#define ADC11D 2

#define OSCCAL0 _SFR_MEM8(0x63)
#define CAL00 0
#define CAL01 1
#define CAL02 2
#define CAL03 3
#define CAL04 4
#define CAL05 5
#define CAL06 6
#define CAL07 7

#define OSCTCAL0A _SFR_MEM8(0x64)
#define TCAL0A0 0
#define TCAL0A1 1
#define TCAL0A2 2
#define TCAL0A3 3
#define TCAL0A4 4
#define TCAL0A5 5
#define TCAL0A6 6
#define TCAL0A7 7

#define OSCTCAL0B _SFR_MEM8(0x65)
#define TCAL0B0 0
#define TCAL0B1 1
#define TCAL0B2 2
#define TCAL0B3 3
#define TCAL0B4 4
#define TCAL0B5 5
#define TCAL0B6 6
#define TCAL0B7 7

#define OSCCAL1 _SFR_MEM8(0x66)
#define CAL10 0
#define CAL11 1

#define GTCCR _SFR_MEM8(0x67)

#define OCR1A _SFR_MEM16(0x6C)

#define OCR1AL _SFR_MEM8(0x6C)
#define OCR1AL0 0
#define OCR1AL1 1
#define OCR1AL2 2
#define OCR1AL3 3
#define OCR1AL4 4
#define OCR1AL5 5
#define OCR1AL6 6
#define OCR1AL7 7

#define OCR1AH _SFR_MEM8(0x6D)
#define OCR1AH0 0
#define OCR1AH1 1
#define OCR1AH2 2
#define OCR1AH3 3
#define OCR1AH4 4
#define OCR1AH5 5
#define OCR1AH6 6
#define OCR1AH7 7

#define TCNT1 _SFR_MEM16(0x6E)

#define TCNT1L _SFR_MEM8(0x6E)
#define TCNT1L0 0
#define TCNT1L1 1
#define TCNT1L2 2
#define TCNT1L3 3
#define TCNT1L4 4
#define TCNT1L5 5
#define TCNT1L6 6
#define TCNT1L7 7

#define TCNT1H _SFR_MEM8(0x6F)
#define TCNT1H0 0
#define TCNT1H1 1
#define TCNT1H2 2
#define TCNT1H3 3
#define TCNT1H4 4
#define TCNT1H5 5
#define TCNT1H6 6
#define TCNT1H7 7

#define TCCR1C _SFR_MEM8(0x70)
#define FOC1B 6
#define FOC1A 7

#define TCCR1B _SFR_MEM8(0x71)
#define CS10 0
#define CS11 1
#define CS12 2
#define WGM12 3
#define WGM13 4
#define ICES1 6
#define ICNC1 7

#define TCCR1A _SFR_MEM8(0x72)
#define WGM10 0
#define WGM11 1
#define COM1B0 4
#define COM1B1 5
#define COM1A0 6
#define COM1A1 7

#define UDR1 _SFR_MEM8(0x73)
#define UDR1_0 0
#define UDR1_1 1
#define UDR1_2 2
#define UDR1_3 3
#define UDR1_4 4
#define UDR1_5 5
#define UDR1_6 6
#define UDR1_7 7

#define UBRR1 _SFR_MEM16(0x74)

#define UBRR1L _SFR_MEM8(0x74)
#define UBRR_0 0
#define UBRR_1 1
#define UBRR_2 2
#define UBRR_3 3
#define UBRR_4 4
#define UBRR_5 5
#define UBRR_6 6
#define UBRR_7 7

#define UBRR1H _SFR_MEM8(0x75)
#define UBRR_8 0
#define UBRR_9 1
#define UBRR_10 2
#define UBRR_11 3

#define UCSR1D _SFR_MEM8(0x76)
#define SFDE1 5
#define RXS1 6
#define RXSIE1 7

#define UCSR1C _SFR_MEM8(0x77)
#define UCPOL1 0
#define UCSZ10 1
#define UCSZ11 2
#define USBS1 3
#define UPM10 4
#define UPM11 5
#define UMSEL10 6
#define UMSEL11 7

#define UCSR1B _SFR_MEM8(0x78)
#define TXB81 0
#define RXB81 1
#define UCSZ12 2
#define TXEN1 3
#define RXEN1 4
#define UDRIE1 5
#define TXCIE1 6
#define RXCIE1 7

#define UCSR1A _SFR_MEM8(0x79)
#define MPCM1 0
#define U2X1 1
#define UPE1 2
#define DOR1 3
#define FE1 4
#define UDRE1 5
#define TXC1 6
#define RXC1 7

#define TWSD _SFR_MEM8(0x7A)
#define TWSD0 0
#define TWSD1 1
#define TWSD2 2
#define TWSD3 3
#define TWSD4 4
#define TWSD5 5
#define TWSD6 6
#define TWSD7 7

#define TWSAM _SFR_MEM8(0x7B)
#define TWAE 0
#define TWSAM1 1
#define TWSAM2 2
#define TWSAM3 3
#define TWSAM4 4
#define TWSAM5 5
#define TWSAM6 6
#define TWSAM7 7

#define TWSA _SFR_MEM8(0x7C)
#define TWSA0 0
#define TWSA1 1
#define TWSA2 2
#define TWSA3 3
#define TWSA4 4
#define TWSA5 5
#define TWSA6 6
#define TWSA7 7

#define TWSSRA _SFR_MEM8(0x7D)
#define TWAS 0
#define TWDIR 1
#define TWBE 2
#define TWC 3
#define TWRA 4
#define TWCH 5
#define TWASIF 6
#define TWDIF 7

#define TWSCRB _SFR_MEM8(0x7E)
#define TWCMD0 0
#define TWCMD1 1
#define TWAA 2

#define TWSCRA _SFR_MEM8(0x7F)
#define TWSME 0
#define TWPME 1
#define TWSIE 2
#define TWEN 3
#define TWASIE 4
#define TWDIE 5
#define TWSHE 7


/* Interrupt vectors */
/* Vector 0 is the reset vector */
#define INT0_vect_num  1
#define INT0_vect      _VECTOR(1)  /* External Interrupt Request 0 */
#define PCINT0_vect_num  2
#define PCINT0_vect      _VECTOR(2)  /* Pin Change Interrupt Request 0 */
#define PCINT1_vect_num  3
#define PCINT1_vect      _VECTOR(3)  /* Pin Change Interrupt Request 1 */
#define PCINT2_vect_num  4
#define PCINT2_vect      _VECTOR(4)  /* Pin Change Interrupt Request 2 */
#define WDT_vect_num  5
#define WDT_vect      _VECTOR(5)  /* Watchdog Time-out Interrupt */
#define TIMER1_CAPT_vect_num  6
#define TIMER1_CAPT_vect      _VECTOR(6)  /* Timer/Counter1 Capture Event */
#define TIMER1_COMPA_vect_num  7
#define TIMER1_COMPA_vect      _VECTOR(7)  /* Timer/Counter1 Compare Match A */
#define TIMER1_COMPB_vect_num  8
#define TIMER1_COMPB_vect      _VECTOR(8)  /* Timer/Counter1 Compare Match B */
#define TIMER1_OVF_vect_num  9
#define TIMER1_OVF_vect      _VECTOR(9)  /* Timer/Counter1 Overflow */
#define TIMER0_COMPA_vect_num  10
#define TIMER0_COMPA_vect      _VECTOR(10)  /* TimerCounter0 Compare Match A */
#define TIMER0_COMPB_vect_num  11
#define TIMER0_COMPB_vect      _VECTOR(11)  /* TimerCounter0 Compare Match B */
#define TIMER0_OVF_vect_num  12
#define TIMER0_OVF_vect      _VECTOR(12)  /* Timer/Couner0 Overflow */
#define ANA_COMP_vect_num  13
#define ANA_COMP_vect      _VECTOR(13)  /* Analog Comparator */
#define ADC_vect_num  14
#define ADC_vect      _VECTOR(14)  /* ADC Conversion Complete */
#define USART0_START_vect_num  15
#define USART0_START_vect      _VECTOR(15)  /* USART0, Start */
#define USART0_RX_vect_num  16
#define USART0_RX_vect      _VECTOR(16)  /* USART0, Rx Complete */
#define USART0_UDRE_vect_num  17
#define USART0_UDRE_vect      _VECTOR(17)  /* USART0 Data Register Empty */
#define USART0_TX_vect_num  18
#define USART0_TX_vect      _VECTOR(18)  /* USART0, Tx Complete */
#define USART1_START_vect_num  19
#define USART1_START_vect      _VECTOR(19)  /* USART1, Start */
#define USART1_RX_vect_num  20
#define USART1_RX_vect      _VECTOR(20)  /* USART1, Rx Complete */
#define USART1_UDRE_vect_num  21
#define USART1_UDRE_vect      _VECTOR(21)  /* USART1 Data Register Empty */
#define USART1_TX_vect_num  22
#define USART1_TX_vect      _VECTOR(22)  /* USART1, Tx Complete */
#define USI_START_vect_num  23
#define USI_START_vect      _VECTOR(23)  /* USI Start Condition */
#define USI_OVERFLOW_vect_num  24
#define USI_OVERFLOW_vect      _VECTOR(24)  /* USI Overflow */
#define TWI_SLAVE_vect_num  25
#define TWI_SLAVE_vect      _VECTOR(25)  /* Two-wire Serial Interface */
#define EE_RDY_vect_num  26
#define EE_RDY_vect      _VECTOR(26)  /* EEPROM Ready */
#define QTRIP_vect_num  27
#define QTRIP_vect      _VECTOR(27)  /* Touch Sensing */

#define _VECTOR_SIZE 4 /* Size of individual vector. */
#define _VECTORS_SIZE (28 * _VECTOR_SIZE)


/* Constants */
#define SPM_PAGESIZE (32)
#define RAMSTART     (0x100)
#define RAMSIZE      (1024)
#define RAMEND       (RAMSTART + RAMSIZE - 1)
#define XRAMSTART    (NA)
#define XRAMSIZE     (0)
#define XRAMEND      (RAMEND)
#define E2END        (0xFF)
#define E2PAGESIZE   (4)
#define FLASHEND     (0x3FFF)


/* Fuses */
#define FUSE_MEMORY_SIZE 3

/* Low Fuse Byte */
#define FUSE_CKSEL0  (unsigned char)~_BV(0)  /* Select Clock Source */
#define FUSE_CKSEL1  (unsigned char)~_BV(1)  /* Select Clock Source */
#define FUSE_CKSEL2  (unsigned char)~_BV(2)  /* Select Clock Source */
#define FUSE_CKSEL3  (unsigned char)~_BV(3)  /* Select Clock Source */
#define FUSE_SUT  (unsigned char)~_BV(4)  /* Select start-up time */
#define FUSE_CKOUT  (unsigned char)~_BV(6)  /* Clock output */
#define FUSE_CKDIV8  (unsigned char)~_BV(7)  /* Divide clock by 8 */
#define LFUSE_DEFAULT (FUSE_CKDIV8 & FUSE_SUT & FUSE_CKSEL0)

/* High Fuse Byte */
#define FUSE_BODLEVEL0  (unsigned char)~_BV(0)  /* Brown-out Detector trigger level */
#define FUSE_BODLEVEL1  (unsigned char)~_BV(1)  /* Brown-out Detector trigger level */
#define FUSE_BODLEVEL2  (unsigned char)~_BV(2)  /* Brown-out Detector trigger level */
#define FUSE_EESAVE  (unsigned char)~_BV(3)  /* EEPROM memory is preserved through chip erase */
#define FUSE_WDTON  (unsigned char)~_BV(4)  /* Watchdog Timer Always On */
#define FUSE_SPIEN  (unsigned char)~_BV(5)  /* Enable Serial programming and Data Downloading */
#define FUSE_DWEN  (unsigned char)~_BV(6)  /* debugWIRE Enable */
#define FUSE_RSTDISBL  (unsigned char)~_BV(7)  /* External reset disable */
#define HFUSE_DEFAULT (FUSE_SPIEN)

/* Extended Fuse Byte */
#define FUSE_SELFPRGEN  (unsigned char)~_BV(0)  /* Self Programming Enable */
#define FUSE_BODACT0  (unsigned char)~_BV(1)  /* Brown-out detector mode */
#define FUSE_BODACT1  (unsigned char)~_BV(2)  /* Brown-out detector mode */
#define FUSE_BODPD0  (unsigned char)~_BV(3)  /* Brown-out detector mode */
#define FUSE_BODPD1  (unsigned char)~_BV(4)  /* Brown-out detector mode */
#define EFUSE_DEFAULT (0xFF)


/* Lock Bits */
#define __LOCK_BITS_EXIST


/* Signature */
#define SIGNATURE_0 0x1E
#define SIGNATURE_1 0x94
#define SIGNATURE_2 0x12


/* Device Pin Definitions */
#define ADC5_DDR   DDRB
#define ADC5_PORT  PORTB
#define ADC5_PIN   PINB
#define ADC5_BIT   0

#define TXD0_DDR   DDRB
#define TXD0_PORT  PORTB
#define TXD0_PIN   PINB
#define TXD0_BIT   0

#define PCINT8_DDR   DDRB
#define PCINT8_PORT  PORTB
#define PCINT8_PIN   PINB
#define PCINT8_BIT   0

#define ADC4_DDR   DDRA
#define ADC4_PORT  PORTA
#define ADC4_PIN   PINA
#define ADC4_BIT   7

#define RXD0_DDR   DDRA
#define RXD0_PORT  PORTA
#define RXD0_PIN   PINA
#define RXD0_BIT   7

#define PCINT7_DDR   DDRA
#define PCINT7_PORT  PORTA
#define PCINT7_PIN   PINA
#define PCINT7_BIT   7

#define ADC3_DDR   DDRA
#define ADC3_PORT  PORTA
#define ADC3_PIN   PINA
#define ADC3_BIT   6

#define OC1B_DDR   DDRA
#define OC1B_PORT  PORTA
#define OC1B_PIN   PINA
#define OC1B_BIT   6

#define PCINT6_DDR   DDRA
#define PCINT6_PORT  PORTA
#define PCINT6_PIN   PINA
#define PCINT6_BIT   6

#define ADC2_DDR   DDRA
#define ADC2_PORT  PORTA
#define ADC2_PIN   PINA
#define ADC2_BIT   5

#define OC0B_DDR   DDRA
#define OC0B_PORT  PORTA
#define OC0B_PIN   PINA
#define OC0B_BIT   5

#define PCINT5_DDR   DDRA
#define PCINT5_PORT  PORTA
#define PCINT5_PIN   PINA
#define PCINT5_BIT   5

#define ADC1_DDR   DDRA
#define ADC1_PORT  PORTA
#define ADC1_PIN   PINA
#define ADC1_BIT   4

#define T0_DDR   DDRA
#define T0_PORT  PORTA
#define T0_PIN   PINA
#define T0_BIT   4

#define PCINT4_DDR   DDRA
#define PCINT4_PORT  PORTA
#define PCINT4_PIN   PINA
#define PCINT4_BIT   4

#define ADC0_DDR   DDRA
#define ADC0_PORT  PORTA
#define ADC0_PIN   PINA
#define ADC0_BIT   3

#define T1_DDR   DDRA
#define T1_PORT  PORTA
#define T1_PIN   PINA
#define T1_BIT   3

#define PCINT3_DDR   DDRA
#define PCINT3_PORT  PORTA
#define PCINT3_PIN   PINA
#define PCINT3_BIT   3

#define AIN1_DDR   DDRA
#define AIN1_PORT  PORTA
#define AIN1_PIN   PINA
#define AIN1_BIT   2

#define PCINT2_DDR   DDRA
#define PCINT2_PORT  PORTA
#define PCINT2_PIN   PINA
#define PCINT2_BIT   2

#define AIN0_DDR   DDRA
#define AIN0_PORT  PORTA
#define AIN0_PIN   PINA
#define AIN0_BIT   1

#define PCINT1_DDR   DDRA
#define PCINT1_PORT  PORTA
#define PCINT1_PIN   PINA
#define PCINT1_BIT   1

#define PCINT0_DDR   DDRA
#define PCINT0_PORT  PORTA
#define PCINT0_PIN   PINA
#define PCINT0_BIT   0

#define CLKI_DDR   DDRC
#define CLKI_PORT  PORTC
#define CLKI_PIN   PINC
#define CLKI_BIT   5

#define PCINT17_DDR   DDRC
#define PCINT17_PORT  PORTC
#define PCINT17_PIN   PINC
#define PCINT17_BIT   5

#define PCINT16_DDR   DDRC
#define PCINT16_PORT  PORTC
#define PCINT16_PIN   PINC
#define PCINT16_BIT   4

#define PCINT15_DDR   DDRC
#define PCINT15_PORT  PORTC
#define PCINT15_PIN   PINC
#define PCINT15_BIT   3

#define ADC11_DDR   DDRC
#define ADC11_PORT  PORTC
#define ADC11_PIN   PINC
#define ADC11_BIT   2

#define CLKO_DDR   DDRC
#define CLKO_PORT  PORTC
#define CLKO_PIN   PINC
#define CLKO_BIT   2

#define INT0_DDR   DDRC
#define INT0_PORT  PORTC
#define INT0_PIN   PINC
#define INT0_BIT   2

#define PCINT14_DDR   DDRC
#define PCINT14_PORT  PORTC
#define PCINT14_PIN   PINC
#define PCINT14_BIT   2

#define ADC10_DDR   DDRC
#define ADC10_PORT  PORTC
#define ADC10_PIN   PINC
#define ADC10_BIT   1

#define ICP1_DDR   DDRC
#define ICP1_PORT  PORTC
#define ICP1_PIN   PINC
#define ICP1_BIT   1

#define XCK1_DDR   DDRC
#define XCK1_PORT  PORTC
#define XCK1_PIN   PINC
#define XCK1_BIT   1

#define SCL_DDR   DDRC
#define SCL_PORT  PORTC
#define SCL_PIN   PINC
#define SCL_BIT   1

#define USCK_DDR   DDRC
#define USCK_PORT  PORTC
#define USCK_PIN   PINC
#define USCK_BIT   1

#define PCINT13_DDR   DDRC
#define PCINT13_PORT  PORTC
#define PCINT13_PIN   PINC
#define PCINT13_BIT   1

#define ADC9_DDR   DDRC
#define ADC9_PORT  PORTC
#define ADC9_PIN   PINC
#define ADC9_BIT   0

#define OC0A_DDR   DDRC
#define OC0A_PORT  PORTC
#define OC0A_PIN   PINC
#define OC0A_BIT   0

#define XCK0_DDR   DDRC
#define XCK0_PORT  PORTC
#define XCK0_PIN   PINC
#define XCK0_BIT   0

#define PCINT12_DDR   DDRC
#define PCINT12_PORT  PORTC
#define PCINT12_PIN   PINC
#define PCINT12_BIT   0

#define ADC8_DDR   DDRB
#define ADC8_PORT  PORTB
#define ADC8_PIN   PINB
#define ADC8_BIT   3

#define OC1A_DDR   DDRB
#define OC1A_PORT  PORTB
#define OC1A_PIN   PINB
#define OC1A_BIT   3

#define PCINT11_DDR   DDRB
#define PCINT11_PORT  PORTB
#define PCINT11_PIN   PINB
#define PCINT11_BIT   3

#define ADC7_DDR   DDRB
#define ADC7_PORT  PORTB
#define ADC7_PIN   PINB
#define ADC7_BIT   2

#define TXD1_DDR   DDRB
#define TXD1_PORT  PORTB
#define TXD1_PIN   PINB
#define TXD1_BIT   2

#define DO_DDR   DDRB
#define DO_PORT  PORTB
#define DO_PIN   PINB
#define DO_BIT   2

#define PCINT10_DDR   DDRB
#define PCINT10_PORT  PORTB
#define PCINT10_PIN   PINB
#define PCINT10_BIT   2

#define ADC6_DDR   DDRB
#define ADC6_PORT  PORTB
#define ADC6_PIN   PINB
#define ADC6_BIT   1

#define RXD1_DDR   DDRB
#define RXD1_PORT  PORTB
#define RXD1_PIN   PINB
#define RXD1_BIT   1

#define SDA_DDR   DDRB
#define SDA_PORT  PORTB
#define SDA_PIN   PINB
#define SDA_BIT   1

#define DI_DDR   DDRB
#define DI_PORT  PORTB
#define DI_PIN   PINB
#define DI_BIT   1

#define PCINT9_DDR   DDRB
#define PCINT9_PORT  PORTB
#define PCINT9_PIN   PINB
#define PCINT9_BIT   1

#endif /* _AVR_ATtiny1634_H_ */

