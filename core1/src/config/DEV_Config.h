/*****************************************************************************
* | File      	:   DEV_Config.h
* | Author      :
* | Function    :   Hardware underlying interface
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2026-04-01
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"

extern uint dma_channel;
extern dma_channel_config dma_config;

#define PLL_SYS_KHZ 150 * 1000

#define LCD_SPI_PORT    (spi1)
#define SENSOR_I2C_PORT (i2c1)

/**
 * GPIOI config
 **/
#define ES8311_SDA_PIN  6
#define ES8311_SCL_PIN  7
#define PA_CTRL         0

#define KEY_PLUS        25
#define BAT_EN          28
#define KEY_PWR         24

/*------------------------------------------------------------------------------------------------------*/

/**
 * @brief Delay for a specified number of milliseconds
 * @param xms  Delay time in milliseconds
 */
void DEV_Delay_Ms(uint32_t xms);

/**
 * @brief Delay for a specified number of microseconds
 * @param xus  Delay time in microseconds
 */
void DEV_Delay_Us(uint32_t xus);

/**
 * @brief Write a digital value to a GPIO pin
 * @param Pin  GPIO pin number
 * @param Value  Value to write (0 or 1)
 */
void DEV_Digital_Write(uint16_t Pin, uint8_t Value);

/**
 * @brief Read a digital value from a GPIO pin
 * @param Pin  GPIO pin number
 * @return uint8_t  Digital value read (0 or 1)
 */
uint8_t DEV_Digital_Read(uint16_t Pin);

/**
 * @brief Set the mode of a GPIO pin
 * @param Pin  GPIO pin number
 * @param Mode  Mode to set (0 for input, 1 for output)
 */
void DEV_GPIO_Mode(uint16_t Pin, uint16_t Mode);

/**
 * @brief Configure a key pin
 * @param Pin  GPIO pin number for the key
 */
void DEV_KEY_Config(uint16_t Pin);

/**
 * @brief Write a digital value to a GPIO pin (duplicate)
 * @param Pin  GPIO pin number
 * @param Value  Value to write (0 or 1)
 */
void DEV_Digital_Write(uint16_t Pin, uint8_t Value);

/**
 * @brief Read a digital value from a GPIO pin (duplicate)
 * @param Pin  GPIO pin number
 * @return uint8_t  Digital value read (0 or 1)
 */
uint8_t DEV_Digital_Read(uint16_t Pin);

/**
 * @brief Read ADC value
 * @return uint16_t  ADC reading
 */
uint16_t DEC_ADC_Read(void);

/**
 * @brief Write a byte to SPI
 * @param SPI_PORT  SPI port instance
 * @param Value  Byte value to write
 */
void DEV_SPI_Write_Byte(spi_inst_t *SPI_PORT,uint8_t Value);

/**
 * @brief Write multiple bytes to SPI
 * @param SPI_PORT  SPI port instance
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to write
 */
void DEV_SPI_Write_nByte(spi_inst_t *SPI_PORT,uint8_t *pData, uint32_t Len);

/**
 * @brief Write a byte to I2C register
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param reg  Register address
 * @param Value  Byte value to write
 */
void DEV_I2C_Write_Byte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t reg, uint8_t Value);

/**
 * @brief Write multiple bytes to I2C
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to write
 */
void DEV_I2C_Write_nByte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t *pData, uint32_t Len);

/**
 * @brief Read a byte from I2C register
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param reg  Register address
 * @return uint8_t  Byte value read
 */
uint8_t DEV_I2C_Read_Byte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t reg);

/**
 * @brief Read multiple bytes from I2C
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param reg  Register address
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to read
 */
void DEV_I2C_Read_nByte(i2c_inst_t *I2C_PORT,uint8_t addr,uint8_t reg, uint8_t *pData, uint32_t Len);

/**
 * @brief Set GPIO interrupt
 * @param gpio  GPIO pin number
 * @param events  Interrupt events
 * @param callback  Interrupt callback function
 */
void DEV_SET_IRQ(uint gpio, uint32_t events, gpio_irq_callback_t callback);

/**
 * @brief Set PWM value
 * @param Value  PWM value
 */
void DEV_SET_PWM(uint8_t Value);

/**
 * @brief Initialize the module
 * @return uint8_t  0 on success, non-zero on failure
 */
uint8_t DEV_Module_Init(void);

/**
 * @brief Exit the module
 */
void DEV_Module_Exit(void);

void GPIO_Test();
#endif
