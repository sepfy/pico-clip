/*****************************************************************************
* | File      	:   DEV_Config.c
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
#include "DEV_Config.h"

#include "hardware/structs/iobank0.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/structs/sio.h"

#define CORE1_I2C_DELAY_LOOPS 80U

static void core1_i2c_delay(void)
{
    for (volatile uint32_t i = 0; i < CORE1_I2C_DELAY_LOOPS; i++) {
    }
}

static void core1_i2c_set_dir(uint gpio, bool output)
{
    if (output) sio_hw->gpio_oe_set = 1u << gpio;
    else sio_hw->gpio_oe_clr = 1u << gpio;
}

static void core1_i2c_sda_high(void)
{
    core1_i2c_set_dir(ES8311_SDA_PIN, false);
    core1_i2c_delay();
}

static void core1_i2c_sda_low(void)
{
    sio_hw->gpio_clr = 1u << ES8311_SDA_PIN;
    core1_i2c_set_dir(ES8311_SDA_PIN, true);
    core1_i2c_delay();
}

static void core1_i2c_scl_high(void)
{
    core1_i2c_set_dir(ES8311_SCL_PIN, false);
    for (uint32_t i = 0; i < 1000U &&
         (sio_hw->gpio_in & (1u << ES8311_SCL_PIN)) == 0U; i++) {
        core1_i2c_delay();
    }
    core1_i2c_delay();
}

static void core1_i2c_scl_low(void)
{
    sio_hw->gpio_clr = 1u << ES8311_SCL_PIN;
    core1_i2c_set_dir(ES8311_SCL_PIN, true);
    core1_i2c_delay();
}

static void core1_i2c_start(void)
{
    core1_i2c_sda_high();
    core1_i2c_scl_high();
    core1_i2c_sda_low();
    core1_i2c_scl_low();
}

static void core1_i2c_stop(void)
{
    core1_i2c_sda_low();
    core1_i2c_scl_high();
    core1_i2c_sda_high();
}

static bool core1_i2c_write_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        if ((value & (1u << bit)) != 0U) core1_i2c_sda_high();
        else core1_i2c_sda_low();
        core1_i2c_scl_high();
        core1_i2c_scl_low();
    }
    core1_i2c_sda_high();
    core1_i2c_scl_high();
    bool ack = (sio_hw->gpio_in & (1u << ES8311_SDA_PIN)) == 0U;
    core1_i2c_scl_low();
    return ack;
}

static uint8_t core1_i2c_read_byte(bool ack)
{
    uint8_t value = 0;
    core1_i2c_sda_high();
    for (int bit = 7; bit >= 0; bit--) {
        core1_i2c_scl_high();
        if ((sio_hw->gpio_in & (1u << ES8311_SDA_PIN)) != 0U)
            value |= 1u << bit;
        core1_i2c_scl_low();
    }
    if (ack) core1_i2c_sda_low();
    else core1_i2c_sda_high();
    core1_i2c_scl_high();
    core1_i2c_scl_low();
    core1_i2c_sda_high();
    return value;
}

static void core1_i2c_init_pins(void)
{
    iobank0_hw->io[ES8311_SDA_PIN].ctrl = GPIO_FUNC_SIO;
    iobank0_hw->io[ES8311_SCL_PIN].ctrl = GPIO_FUNC_SIO;
    padsbank0_hw->io[ES8311_SDA_PIN] = PADS_BANK0_GPIO0_IE_BITS |
        PADS_BANK0_GPIO0_SCHMITT_BITS | PADS_BANK0_GPIO0_PUE_BITS;
    padsbank0_hw->io[ES8311_SCL_PIN] = PADS_BANK0_GPIO0_IE_BITS |
        PADS_BANK0_GPIO0_SCHMITT_BITS | PADS_BANK0_GPIO0_PUE_BITS;
    sio_hw->gpio_clr = (1u << ES8311_SDA_PIN) | (1u << ES8311_SCL_PIN);
    core1_i2c_set_dir(ES8311_SDA_PIN, false);
    core1_i2c_set_dir(ES8311_SCL_PIN, false);
    core1_i2c_delay();
}

static void core1_output_init(uint gpio, bool value)
{
    iobank0_hw->io[gpio].ctrl = GPIO_FUNC_SIO;
    padsbank0_hw->io[gpio] = PADS_BANK0_GPIO0_IE_BITS |
        PADS_BANK0_GPIO0_SCHMITT_BITS;
    if (value) sio_hw->gpio_set = 1u << gpio;
    else sio_hw->gpio_clr = 1u << gpio;
    sio_hw->gpio_oe_set = 1u << gpio;
}

uint slice_num;
uint dma_channel;
dma_channel_config dma_config;

/**
 * @brief Delay for a specified number of milliseconds
 * @param xms  Delay time in milliseconds
 */
void DEV_Delay_Ms(uint32_t xms)
{
    for (volatile uint32_t i = 0; i < xms * 37500U; i++) {
        __asm volatile ("nop");
    }
}

/**
 * @brief Delay for a specified number of microseconds
 * @param xus  Delay time in microseconds
 */
void DEV_Delay_Us(uint32_t xus)
{
    for (volatile uint32_t i = 0; i < xus * 37U; i++) {
        __asm volatile ("nop");
    }
}

/**
 * @brief Initialize GPIO pins for the device
 */
void DEV_GPIO_Init(void)
{
    gpio_init(PA_CTRL);
    gpio_set_dir(PA_CTRL, GPIO_OUT);
    gpio_put(PA_CTRL, 1);
    DEV_KEY_Config(KEY_PLUS);
    gpio_init(BAT_EN);
    gpio_set_dir(BAT_EN, GPIO_OUT);
    gpio_put(BAT_EN, 1);
    DEV_KEY_Config(KEY_PWR);
}

/**
 * @brief GPIO read and write
 */
void DEV_Digital_Write(uint16_t Pin, uint8_t Value)
{
    gpio_put(Pin, Value);
}

/**
 * @brief Read a digital value from a GPIO pin
 * @param Pin  GPIO pin number
 * @return uint8_t  Digital value read (0 or 1)
 */
uint8_t DEV_Digital_Read(uint16_t Pin)
{
    return gpio_get(Pin);
}

/**
 * @brief SPI
 */
void DEV_SPI_Write_Byte(spi_inst_t *SPI_PORT,uint8_t Value)
{
    spi_write_blocking(SPI_PORT, &Value, 1);
}

/**
 * @brief Write multiple bytes to SPI
 * @param SPI_PORT  SPI port instance
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to write
 */
void DEV_SPI_Write_nByte(spi_inst_t *SPI_PORT,uint8_t pData[], uint32_t Len)
{
    spi_write_blocking(SPI_PORT, pData, Len);
}

/**
 * @brief I2C
 */
void DEV_I2C_Write_Byte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t reg, uint8_t Value)
{
    (void)I2C_PORT;
    core1_i2c_start();
    (void)core1_i2c_write_byte((uint8_t)(addr << 1));
    (void)core1_i2c_write_byte(reg);
    (void)core1_i2c_write_byte(Value);
    core1_i2c_stop();
}

/**
 * @brief Write multiple bytes to I2C
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to write
 */
void DEV_I2C_Write_nByte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t *pData, uint32_t Len)
{
    i2c_write_blocking(I2C_PORT, addr, pData, Len, false);
}

/**
 * @brief Read a byte from I2C register
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param reg  Register address
 * @return uint8_t  Byte value read
 */
uint8_t DEV_I2C_Read_Byte(i2c_inst_t *I2C_PORT,uint8_t addr, uint8_t reg)
{
    (void)I2C_PORT;
    core1_i2c_start();
    (void)core1_i2c_write_byte((uint8_t)(addr << 1));
    (void)core1_i2c_write_byte(reg);
    core1_i2c_start();
    (void)core1_i2c_write_byte((uint8_t)((addr << 1) | 1U));
    uint8_t value = core1_i2c_read_byte(false);
    core1_i2c_stop();
    return value;
}

/**
 * @brief Read multiple bytes from I2C
 * @param I2C_PORT  I2C port instance
 * @param addr  I2C device address
 * @param reg  Register address
 * @param pData  Pointer to data buffer
 * @param Len  Length of data to read
 */
void DEV_I2C_Read_nByte(i2c_inst_t *I2C_PORT,uint8_t addr,uint8_t reg, uint8_t *pData, uint32_t Len)
{
    i2c_write_blocking(I2C_PORT,addr,&reg,1,true);
    i2c_read_blocking(I2C_PORT,addr,pData,Len,false);
}

/**
 * @brief GPIO Mode
 */
void DEV_GPIO_Mode(uint16_t Pin, uint16_t Mode)
{
    gpio_init(Pin);
    if (Mode == 0 || Mode == GPIO_IN)
    {
        gpio_set_dir(Pin, GPIO_IN);
    }
    else
    {
        gpio_set_dir(Pin, GPIO_OUT);
    }
}

/**
 * @brief KEY Config
 */
void DEV_KEY_Config(uint16_t Pin)
{
    gpio_init(Pin);
    gpio_pull_up(Pin);
    gpio_set_dir(Pin, GPIO_IN);
}

/**
 * @brief PWM
 */
void DEV_SET_PWM(uint8_t Value)
{
    if (Value < 0 || Value > 100)
    {
        printf("DEV_SET_PWM Error \r\n");
    }
    else
    {
        pwm_set_chan_level(slice_num, PWM_CHAN_B, Value);
    }
}

/**
 * @brief IRQ
 */
void DEV_SET_IRQ(uint gpio, uint32_t events, gpio_irq_callback_t callback)
{
    gpio_set_irq_enabled_with_callback(gpio,events,true,callback);
}

/**
 * @brief Module Initialize, the library and initialize the pins, SPI protocol
 * @return uint8_t  0 on success, non-zero on failure
 */
uint8_t DEV_Module_Init(void)
{
    core1_output_init(PA_CTRL, true);
    core1_output_init(BAT_EN, true);
    core1_i2c_init_pins();
    return 0;
}

/******************************************************************************
function:	Module exits, closes SPI and BCM2835 library
parameter:
Info:
******************************************************************************/
void DEV_Module_Exit(void)
{

}
