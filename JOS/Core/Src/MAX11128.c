/*
 * MAX11128.c
 *
 *  Created on: Aug 30, 2022
 *      Author: NAWAT.KIPHUWADON
 */

/*	Instruction
 * 	1.Initialize the MAX11128
 * 	2.Configure the ADC
 * 	3.Program the ADC mode control
 */
#include "MAX11128.h"
#include "spi1_bus.h"
#include <stdint.h>
#include <stdbool.h>

/* Finite SPI timeout: a 2-byte frame at >=1 MHz is microseconds on the wire;
 * 10 ms is generous headroom and — unlike HAL_MAX_DELAY — can never park a
 * task forever. Every transaction below runs under the shared SPI1 bus mutex
 * (radio SX1268 + this ADC, see App/comms/spi1_bus.h) from CS assert to CS
 * de-assert, so a radio burst can never interleave inside an ADC frame. */
#define MAX11128_SPI_TIMEOUT_MS   (10U)

/* One complete 16-bit frame with CS held low throughout: the 2-byte command
 * clocks out while the 2-byte response clocks in (single TransmitReceive —
 * the old Transmit + HAL_Delay + Receive split the frame and glitched CS
 * mid-transaction). On any failure (bus busy, HAL error/timeout) CS is still
 * de-asserted, the bus released, and rx left untouched; callers degrade to a
 * best-effort miss (0). */
// cppcheck-suppress constParameterPointer  // tx cannot be const: HAL takes uint8_t*
static bool max11128_frame(MAX11128_t *adc, uint8_t tx[2], uint8_t rx[2])
{
	if ((adc == NULL) || (adc->spiHandle == NULL) || (tx == NULL) || (rx == NULL)) {
		return false;
	}
	if (!spi1_bus_lock(SPI1_BUS_LOCK_TIMEOUT_MS)) {
		return false;
	}
	MAX11128_CS(adc,0);
	HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(adc->spiHandle,
			tx, rx, 2U, MAX11128_SPI_TIMEOUT_MS);
	MAX11128_CS(adc,1);
	spi1_bus_unlock();
	return (st == HAL_OK);
}

void 	MAX11128_Initialize(MAX11128_t *adc, SPI_HandleTypeDef *spiHandle)  //Initialize SPI on MAX11128
{	/* SET STRUCT PARAMETERS */
	adc -> spiHandle = spiHandle;
	spi1_bus_init(); /* idempotent: first client init wins */
	MAX11128_CS(adc,1);
}

void MAX11128_ADC_Config(MAX11128_t *adc)	//Set configuration on MAX11128
{
	uint8_t ADC_CONFIG[2] = {0x82,0x02};
	uint8_t dummy[2];
	(void)max11128_frame(adc, ADC_CONFIG, dummy);
}

void 	MAX11128_ADC_Uni_Setup(MAX11128_t *adc) //Set Unipolar on MAX11128
{
	uint8_t UNI_SETUP[2] = {0x88,0x02};
	uint8_t dummy[2];
	(void)max11128_frame(adc, UNI_SETUP, dummy);
}

void 	MAX11128_ADC_Bpi_Setup(MAX11128_t *adc) //Set Bipolar on MAX11128
{
	uint8_t BPI_SETUP[2] = {0x90,0x02};
	uint8_t dummy[2];
	(void)max11128_frame(adc, BPI_SETUP, dummy);
}

float	MAX11128_ADC_ReadVoltCH(MAX11128_t *adc,uint8_t CH) //Read voltage CHx on MAX11128
{
	if(CH<16)
	{  // Check if CH is <= 15 as we have 16 channel
		uint16_t ADC_MODE = 0x0804|(CH<<7);
		uint8_t ADC_MODE_CTRL[2],ADC_Receive[2];
		ADC_MODE_CTRL[0] = (ADC_MODE&(0XFF00))>>8;
		ADC_MODE_CTRL[1] = (ADC_MODE&(0X00FF));
		// Single full-duplex frame: mode-control out, conversion result in.
		if (!max11128_frame(adc, ADC_MODE_CTRL, ADC_Receive))
		{
			return 0;
		}
		// check if CHAN_ID is correct
		if (CH == ((ADC_Receive[0]&(0xF0)) >> 4 ))
		{
			uint16_t ADC_Value;
			float    ADC_CH;
			ADC_Value = (((uint16_t)(ADC_Receive[0])) << 8 ) | ADC_Receive[1];
			ADC_Value =  ADC_Value & (0x0FFF);	//remove CHAN_ID to get ADC 12 bit value
			ADC_CH    = (float)(MAX1112XVREF * ADC_Value)/((1u << MAX1112XBIT) - 1);
			return ADC_CH;
		}
		// if CHAD_ID is wrong return 0;
		else
			return 0;
	} // if CH is wrong return 0;
	else
		return 0;
}

uint16_t MAX11128_ADC_ReadRawCH (MAX11128_t *adc,uint8_t CH)
{
	if(CH<16)  // Check if CH is <= 15 as we have 16 channel
	{
		uint16_t ADC_MODE = 0x0804|(CH<<7);
		uint16_t ADC_Value;
		uint8_t ADC_MODE_CTRL[2],ADC_Receive[2];
		ADC_MODE_CTRL[0] = (ADC_MODE&(0XFF00))>>8;
		ADC_MODE_CTRL[1] = (ADC_MODE&(0X00FF));
		// Single full-duplex frame: mode-control out, conversion result in.
		if (!max11128_frame(adc, ADC_MODE_CTRL, ADC_Receive))
		{
			return 0;
		}

		ADC_Value = (((uint16_t)(ADC_Receive[0])) << 8 ) | ADC_Receive[1];
		//ADC_Value =  ADC_Value & (0x0FFF);	//remove CHAN_ID ADC to get 12 bit value
		return ADC_Value;
	}
	else
		return 0;
}


uint8_t MAX11128_CS(MAX11128_t*adc, uint8_t on) //chip select pin
{
  if(on == 0)
  	  HAL_GPIO_WritePin( adc->csPort, adc->csPin, GPIO_PIN_RESET);
  else
	  HAL_GPIO_WritePin( adc->csPort, adc->csPin, GPIO_PIN_SET);
  return 0;
}




