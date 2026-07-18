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

uint slice_num;
uint dma_channel;
dma_channel_config dma_config;

/**
 * @brief Delay for a specified number of milliseconds
 * @param xms  Delay time in milliseconds
 */
void DEV_Delay_Ms(uint32_t xms)
{
    sleep_ms(xms);
}

/**
 * @brief Delay for a specified number of microseconds
 * @param xus  Delay time in microseconds
 */
void DEV_Delay_Us(uint32_t xus)
{
    sleep_us(xus);
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
    uint8_t data[2] = {reg, Value};
    i2c_write_blocking(I2C_PORT, addr, data, 2, false);
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
    uint8_t buf;
    i2c_write_blocking(I2C_PORT,addr,&reg,1,true);
    i2c_read_blocking(I2C_PORT,addr,&buf,1,false);
    return buf;
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
    stdio_init_all();   
    sleep_ms(1000);
    DEV_GPIO_Init();
    
    // I2C Initialisation. Using it at 100Khz.
    i2c_init(SENSOR_I2C_PORT, 400 * 1000);
    gpio_set_function(ES8311_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(ES8311_SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_pulls(ES8311_SDA_PIN, true, false);
    gpio_set_pulls(ES8311_SCL_PIN, true, false);

    // DMA
    dma_channel = dma_claim_unused_channel(true);   
    dma_config = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_16); 
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
    
    printf("DEV_Module_Init OK \r\n");
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
