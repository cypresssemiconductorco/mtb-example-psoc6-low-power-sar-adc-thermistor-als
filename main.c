/******************************************************************************
* File Name: main.c
*
* Description: This is the source code for the PSoC 6 MCU SAR ADC Low-Power
*              Sensing - Thermistor and ALS
*
* Related Document: See README.md
*
*
*******************************************************************************
* Copyright 2020-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

#include "cy_pdl.h"
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "math.h"

/*******************************************************************************
* Macros
********************************************************************************/
/* Defines for the ADC channels */
#define THERMISTOR_SENSOR_CHANNEL           (1)
#define REF_RESISTOR_CHANNEL                (0)
#define ALS_SENSOR_CHANNEL                  (2)

/* Number of channels used */
#define CHANNEL_COUNT                       (3)

/* Reference resistor in series with the thermistor is of 10kohm */
#define R_REFERENCE                         (float)(10000)

/* Beta constant of NCP18XH103F03RB thermistor is 3380 Kelvin. See the thermistor
   data sheet for more details. */
#define B_CONSTANT                          (float)(3380)

/* Resistance of the thermistor is 10K at 25 degrees C (from the data sheet)
   Therefore R0 = 10000 Ohm, and T0 = 298.15 Kelvin, which gives
   R_INFINITY = R0 e^(-B_CONSTANT / T0) = 0.1192855 */
#define R_INFINITY                          (float)(0.1192855)

/* Zero Kelvin in degree C */
#define ABSOLUTE_ZERO                       (float)(-273.15)

/* ALS offset in Percent */
/* To configure this value, begin with offset of 0 and note down the lowest ALS
percent value. Configure the ALS_OFFSET with the lowest observed ALS percent. */
#define ALS_OFFSET                          (20)

/* ALS low threshold value - if ALS percentage is lower than this value, user
 * LED is turned ON */
#define ALS_LOW_THRESHOLD                   (45)

/* ALS high threshold value - if ALS percentage is higher than this value, user
 * LED is turned OFF */
#define ALS_HIGH_THRESHOLD                  (55)

/* Raw data value from the ALS for the fully dark ambient */
#define ALS_DARK_AMBIENT_RAW_DATA           (0xFFF0)

/* The value that is passed to the IIR filter for the fully dark ambient */
#define ALS_DARK_AMBIENT_DATA              (0)

/*******************************************************************************
* Function Prototypes
********************************************************************************/
/* Function to convert the measured voltage in the thermistor circuit into
 * temperature */
double get_temperature(int32 therm_count, int32 ref_count);

/* Function to convert the measured voltage in the ALS circuit into percentage */
uint8 get_light_intensity(int32 adc_count);

/* IIR Filter implementation */
int32 low_pass_filter(int32 input, uint8 data_source);

/* FIFO Interrupt Handler */
void sar_fifo_interrupt_handler(void);

/* Function to initialize analog resources */
/* Resources include SAR ADC and its associated FIFO, analog references and
 deep sleep resources */
void init_analog_resources(void);

/*******************************************************************************
* Global Variables
********************************************************************************/
/* IIR Filter variables */
int32 filt_var[CHANNEL_COUNT];

/* FIFO interrupt configuration structure */
/* Source is set to FIFO 0 and Priority as 7 */
const cy_stc_sysint_t fifo_irq_cfg = {
    .intrSrc = (IRQn_Type) pass_interrupt_fifo_0_IRQn,
    .intrPriority = 7
};

/* This flag is set in the FIFO interrupt handler */
volatile uint8 fifo_intr_flag = false;


/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* This is the main function for CM4 CPU. It initializes the ADC, does the
* measurement of thermistor and ALS and sends the data over UART.
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{

    /* Configure P6[5] - JTAG Data to Analog High Z to avoid leakage current */
    /* This pin is logic high by default which causes leakage current on CY8CKIT-062S4 Pioneer Kit. */
    cyhal_gpio_configure(P6_5, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_ANALOG);

    /* Variable to capture return value of functions */
    cy_rslt_t result;

    /* FIFO read structure */
    cy_stc_sar_fifo_read_t fifo_data = {0};

    /* Variable for filtered reference voltage (thermistor circuit) and als data */
    int32 filtered_data[CHANNEL_COUNT];

    /* Temperature value in deg C */
    double temperature;

    /* Light intensity in percentage */
    uint8 light_intensity;

    /* Variable to initialize IIR filter for the first iteration */
    uint8 first_run[CHANNEL_COUNT]= {true, true, true};

    /* Variable for number of samples accumulated in FIFO */
    uint8 data_count;

    uint16 display_delay = 0;

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Initialize the debug uart */
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX,
                                     CY_RETARGET_IO_BAUDRATE);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Print message */

    /* \x1b[2J\x1b[;H - ANSI ESC sequence for clear screen */
    printf("\x1b[2J\x1b[;H");

    printf("---------------------------------------------------------------------------\r\n");
    printf("PSoC 6 MCU: SAR ADC Low-Power Sensing - Thermistor and Ambient Light Sensor\r\n");
    printf("---------------------------------------------------------------------------\r\n\n");
    printf("Touch the thermistor and block/increase the light over the ambient light \r\n");
    printf("sensor to observe change in the readings. \r\n\n");

    /* Initialize and enable analog resources */
    init_analog_resources();

    /* Configure the LED pin */
    result = cyhal_gpio_init(CYBSP_USER_LED2, CYHAL_GPIO_DIR_OUTPUT , CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable the global interrupt */
    __enable_irq();

    /* Enable the timer to start the sampling process  */
    /* Using the device configurator, trigger interval from the timer is
    * set to 2.5ms which results in effective scan rate of 400sps for the SAR ADC.
    */
    Cy_SysAnalog_TimerEnable(PASS);

    for (;;)
    {
        /* Wait till printf completes the UART transfer */
        while(cyhal_uart_is_tx_active(&cy_retarget_io_uart_obj) == true);

        /* Put the device to deep-sleep mode. Device wakes up with the level interrupt from FIFO.
           With the effective scan rate of 400sps, level count of 120 and 3 channels, device
           wakes up every 120/(400*3) seconds, that is, 100ms. */
        Cy_SysPm_CpuEnterDeepSleep(CY_SYSPM_WAIT_FOR_INTERRUPT);

        /* Check if the interrupt is from the FIFO */
        if(fifo_intr_flag)
        {
            /* Clear the flag */
            fifo_intr_flag = false;

            /* Check how many entries to be read. Should be equal to (LEVEL+1) when level
             * interrupt is enabled */
            data_count = Cy_SAR_FifoGetDataCount(SAR0);

            /* Take all the readings from the FIFO and feed through IIR Filter */
            while(data_count > 0)
            {
                data_count--;

                /* Read the FIFO */
                Cy_SAR_FifoRead(SAR0, &fifo_data);

                /* If it is the first time reading the data, initialize the IIR filter
                 * variable and result variable */
                if(first_run[fifo_data.channel] == true)
                {
                    /* If the raw data value from the ALS is greater than or equal to ALS_DARK_AMBIENT_RAW_DATA value,
                     * then will skip the value and pass the ALS_DARK_AMBIENT_DATA value to the IIR filter. */
                    if ((fifo_data.channel == ALS_SENSOR_CHANNEL) && (fifo_data.value >= ALS_DARK_AMBIENT_RAW_DATA))
                    {
                        filtered_data[fifo_data.channel] = ALS_DARK_AMBIENT_DATA;
                        filt_var[fifo_data.channel] = ALS_DARK_AMBIENT_DATA;

                    }
                    else
                    {
                        filtered_data[fifo_data.channel] = fifo_data.value;
                        filt_var[fifo_data.channel] = fifo_data.value << 8;
                    }

                    /* Clear the flag */
                    first_run[fifo_data.channel] = false;
                }
                else /* Push the data to the IIR filter */
                    filtered_data[fifo_data.channel] = low_pass_filter((int16)fifo_data.value, fifo_data.channel);
            }

            /* Calculate the temperature value */
            temperature = get_temperature(filtered_data[THERMISTOR_SENSOR_CHANNEL], filtered_data[REF_RESISTOR_CHANNEL]);

            /* Calculate the ambient light intensity in percentage */
            light_intensity = get_light_intensity(filtered_data[ALS_SENSOR_CHANNEL]);

            /* Control the LED */
            if(light_intensity < ALS_LOW_THRESHOLD)
                cyhal_gpio_write(CYBSP_USER_LED2, CYBSP_LED_STATE_ON);
            else
            if(light_intensity > ALS_HIGH_THRESHOLD)
                cyhal_gpio_write(CYBSP_USER_LED2, CYBSP_LED_STATE_OFF);

            /* Send over UART every 500ms */
            if(display_delay == 4)
            {
                /* Print the temperature and the ambient light value*/
                printf("Temperature: %2.1lfC    Ambient Light: %d%%\r\n", temperature, light_intensity);

                /* Clear the counter */
                display_delay = false;
            }
            else /* Increment the counter */
                display_delay++;
        }
    }
}

/*******************************************************************************
* Function Name: init_analog_resources
********************************************************************************
* Summary:
* This function initializes the analog resources such as SAR ADC, reference block,
* and deep sleep resources.
*
* Parameters:
*  None
*
* Return:
*  None
*
*******************************************************************************/
void init_analog_resources()
{
    /* Variable to capture return value of functions */
    cy_rslt_t result;

    /* Initialize AREF */
    result = Cy_SysAnalog_Init(&pass_0_aref_0_config);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Initialize deep sleep resources - Timer, LPOSC */
    result = Cy_SysAnalog_DeepSleepInit(PASS, &cy_cfg_pass0_deep_sleep_config);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable AREF */
    Cy_SysAnalog_Enable();

    /* Enable Low-Power Oscillator */
    Cy_SysAnalog_LpOscEnable(PASS);

    /* Initialize the SAR ADC; it includes initialization of FIFO */
    result = Cy_SAR_Init(SAR0, &pass_0_saradc_0_sar_0_config);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Initialize common resources for SAR ADCs in the pass block.
       Common resources include simultaneous trigger parameters, scan count
       and power up delay */
    result = Cy_SAR_CommonInit(PASS, &pass_0_saradc_0_config);

    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable SAR block */
    Cy_SAR_Enable(SAR0);

    /* Enable the FIFO Level Interrupt mask */
    Cy_SAR_SetFifoInterruptMask(SAR0, CY_SAR_INTR_FIFO_LEVEL);

    /* Configure the interrupt and provide the ISR address. */
    (void)Cy_SysInt_Init(&fifo_irq_cfg, sar_fifo_interrupt_handler);

    /* Enable the interrupt. */
    NVIC_EnableIRQ(fifo_irq_cfg.intrSrc);
}


/*******************************************************************************
* Function Name: get_temperature
********************************************************************************
* Summary:
* This function calculates the temperature in degree celsius.
*
* Parameters:
*  ADC results for thermistor and reference resistor voltages
*
* Return:
*  temperature in degree celsius (float)
*
*******************************************************************************/
double get_temperature(int32 therm_count, int32 ref_count)
{
    double temperature;
    double rThermistor;

    /* Calculate the thermistor resistance */
    rThermistor = (double)therm_count * R_REFERENCE / ref_count;

    /* Calculate the temperature in deg C */
    temperature = (double)(B_CONSTANT/(logf(rThermistor/R_INFINITY))) + ABSOLUTE_ZERO;

    return(temperature);
}

/*******************************************************************************
* Function Name: get_light_intensity
********************************************************************************
* Summary:
* This function calculates the ambient light intensity in terms of percentage.
*
* Parameters:
*  ADC measurement result of the photo-transistor
*
* Return:
*  ambient light intensity in percentage (uint8: 0 - 100)
*
*******************************************************************************/
uint8 get_light_intensity(int32 adc_count)
{
    int16 als_level;
    
    if(adc_count < 0)
        adc_count = 0;

    /* Calculate the ambient light intensity in terms of percentage */
    /* Adjust the shift parameter for the required sensitivity */
    als_level = ((adc_count * 100)>>10) - ALS_OFFSET;

    /* Limit the values between 0 and 100 */
    if(als_level > 100)
        als_level = 100;

    if(als_level < 0)
        als_level = 0;

    return((uint8)als_level);
}

/*******************************************************************************
* Function Name: low_pass_filter
********************************************************************************
* Summary:
* This function implements IIR filter for each SAR channel data. Cut-off frequency
* is given by  F0 = Fs / (2 * pi * a) where, a is the attenuation constant and Fs
* is the sample rate, that is, 400 sps.
*
* In this function, for thermistor and reference resistor channel, a = 256/160 and
* cut-off frequency is approximately 40Hz; for ALS, a=256/4, cut-off frequency is
* approximately 1Hz.
*
* Parameters:
*  Data to be filtered and the data source.
*
* Return:
*  Filtered data
*
*******************************************************************************/
int32 low_pass_filter(int32 input, uint8 data_source)
{
    int32 k;
    input <<= 8;

    switch(data_source)
    {
        case THERMISTOR_SENSOR_CHANNEL:
            filt_var[THERMISTOR_SENSOR_CHANNEL] = filt_var[THERMISTOR_SENSOR_CHANNEL] + (((input-filt_var[THERMISTOR_SENSOR_CHANNEL]) >> 8) * 160);
            k = (filt_var[THERMISTOR_SENSOR_CHANNEL]>>8) + ((filt_var[THERMISTOR_SENSOR_CHANNEL] & 0x00000080) >> 7);
        break;

        case REF_RESISTOR_CHANNEL:
            filt_var[REF_RESISTOR_CHANNEL] = filt_var[REF_RESISTOR_CHANNEL] + (((input-filt_var[REF_RESISTOR_CHANNEL]) >> 8) * 160);
            k = (filt_var[REF_RESISTOR_CHANNEL]>>8) + ((filt_var[REF_RESISTOR_CHANNEL] & 0x00000080) >> 7);
        break;

        case ALS_SENSOR_CHANNEL:
            filt_var[ALS_SENSOR_CHANNEL] = filt_var[ALS_SENSOR_CHANNEL] + (((input-filt_var[ALS_SENSOR_CHANNEL]) >> 8) * 4);
            k = (filt_var[ALS_SENSOR_CHANNEL]>>8) + ((filt_var[ALS_SENSOR_CHANNEL] & 0x00000080) >> 7);
        break;

        default:
            k = 0;
        break;
    }

    return k;
}

/*******************************************************************************
* Function Name: sar_fifo_interrupt_handler
********************************************************************************
* Summary:
* This function is the handler for FIFO level interrupt
*
* Parameters:
*  None
*
* Return:
*  None
*
*******************************************************************************/
void sar_fifo_interrupt_handler()
{
    /* Clear the FIFO interrupt */
    Cy_SAR_ClearFifoInterrupt(SAR0, CY_SAR_INTR_FIFO_LEVEL);

    /* Set the flag */
    fifo_intr_flag = true;
}

/* [] END OF FILE */
