/*
 * FreeRTOS V202212.01
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */


/* Environment includes. */
#include "DriverLib.h"

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* UART configuration */
#define mainBAUD_RATE				( 19200 )

#define mainCHECK_TASK_PRIORITY		( tskIDLE_PRIORITY + 3 )

/* Misc. */
#define mainQUEUE_SIZE				( 3 )
#define configSENSOR_FREQUENCY_MS   ( ( TickType_t ) 100 / portTICK_PERIOD_MS ) // 100 ms
#define configTOP_DELAY             ( ( TickType_t ) 3000 / portTICK_PERIOD_MS ) // 3 seg

#define MAX_BUFFER_SIZE 99 // We only have 2 columns to display the current n in the graph.
#define GRAPH_COLUMNS 85
#define GRAPH_X_INIT 18

/*I setted these two temperature limits because of the graph limits*/
#define MAX_TEMP 30
#define MIN_TEMP 0

#define UART_BUFFER_SIZE 5

#define MONITORING_STACK_WATER_MARK 0 //Enable this for stack monitoring

void vTemperatureSensorTask( void *pvParameters );
void vFilterTask( void *pvParameters );
void vGraphTask( void *pvParameters );
void vReceiveCharTask( void *pvParameters );
void vMonitorTask( void *pvParameters );
void vTopTask( void *pvParameters );

void prvSetupHardware( void );
void prvConfigTimer(void);
char* itoa(int num, char* str, int base);
int atoi(char *str);
void reverse(char str[], int length);
void pushTempIntoBuffer(int temp, int* temperatureBuffer, int buffer_size);
void drawAxis(void);
char* getColumnOctal(int value);
int uiGetRandomNumber( void );
void UARTSendString(const char *str);
void vTimer0IntHandler(void);
unsigned long ulGetRunTimeCounterValue(void);
void printTop(void);


static uint32_t _dwRandNext = 0xEEEEAAAA;
volatile int current_n = 15;
int UARTBufferIndex = 0;
char UARTInputBuffer[UART_BUFFER_SIZE];
unsigned long runTimeCounter;
TaskStatus_t *pxTaskStatusArray;


QueueHandle_t xTemperatureQueue;
QueueHandle_t xGraphQueue;
QueueHandle_t xUARTQueue;


/* TaskHandles for stack measurement */
TaskHandle_t xTemperatureSensorTaskHandle;
TaskHandle_t xFilterTaskHandle;
TaskHandle_t xGraphTaskHandle;
TaskHandle_t xReceiveCharTaskHandle;

int main( void )
{
	/* Configure the clocks, UART and GPIO. */
	prvSetupHardware();

	/* Create the queue used to pass message to vPrintTask. */
	xGraphQueue = xQueueCreate( mainQUEUE_SIZE, sizeof( int ) );
    xTemperatureQueue = xQueueCreate( mainQUEUE_SIZE, sizeof( int ) );
    xUARTQueue = xQueueCreate( mainQUEUE_SIZE, sizeof( char ) );

    /* Error handling. */
	if ((xTemperatureQueue == NULL) || (xGraphQueue == NULL) || (xUARTQueue == NULL))
	{
		OSRAMClear();
		OSRAMStringDraw("Queue Error", 0, 0);
    	while (true);
	}

	/* Start the tasks defined within the file. */
	xTaskCreate( vTemperatureSensorTask, "Sensor", configSENSOR_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY + 1, &xTemperatureSensorTaskHandle );
	xTaskCreate( vFilterTask, "Filter", configFILTER_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY, &xFilterTaskHandle );
    xTaskCreate( vGraphTask, "Graph", configGRAPH_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY - 1, &xGraphTaskHandle );
    xTaskCreate( vReceiveCharTask, "UART", configRECEIVE_CHAR_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY, &xReceiveCharTaskHandle);
    xTaskCreate( vTopTask, "Top", configTOP_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY - 2, NULL);
    if(MONITORING_STACK_WATER_MARK){
        xTaskCreate( vMonitorTask, "Monitor", configMINIMAL_STACK_SIZE, NULL, mainCHECK_TASK_PRIORITY - 3, NULL);
    }

	/* Start the scheduler. */
	vTaskStartScheduler();

	/* Will only get here if there was insufficient heap to start the
	scheduler. */

	return 0;
}

//---------------------- TASKS ----------------------------------

void vTemperatureSensorTask( void *pvParameters ) {

    TickType_t xLastExecutionTime = xTaskGetTickCount();

	// Inicializar la temperatura con un valor base de 18
    int temperature = 18;
    int change = 0;
    while (1) {
        /* Generar el valor de temperatura cada configSENSOR_FREQUENCY_MS milisegundos */
		vTaskDelayUntil( &xLastExecutionTime, configSENSOR_FREQUENCY_MS );

        // Generar un cambio aleatorio de -1 o +1
        change = ( uiGetRandomNumber() % 3 ) - 1; //-1, 0, 1
        temperature += change;

        if (temperature < MIN_TEMP){
            temperature = MIN_TEMP;
        }
        if (temperature > MAX_TEMP){
            temperature = MAX_TEMP;
        }

        /* Send the new temperature value to the filter task */
        xQueueSend(xTemperatureQueue, &temperature, portMAX_DELAY);

    }
}

void vFilterTask( void *pvParameters ) {

    int temperatureBuffer[MAX_BUFFER_SIZE];

	while(1) {
		int sum = 0;
        int tempValue;
		int avg = 0;
        /* Receive the new temperature value */
        xQueueReceive(xTemperatureQueue, &tempValue, portMAX_DELAY);

        /* Add the new value into the buffer */
        pushTempIntoBuffer(tempValue, temperatureBuffer, MAX_BUFFER_SIZE);

        /* Calculate the average of the last n values */
        for(int i=0; i < current_n; i++) {
            sum += temperatureBuffer[i];
        }
        avg = sum / current_n;

        /* Send the average value to the graph task */
        xQueueSend(xGraphQueue, &avg, portMAX_DELAY);
    }
}

void vGraphTask( void *pvParameters ){
    int avgTemp;
    int avgTempArray[GRAPH_COLUMNS] = {};
    
    for (;;)
    {
        /* Receive the new avg temp value from filter */
        xQueueReceive(xGraphQueue, &avgTemp, portMAX_DELAY);

        /* Add the new value into the buffer */
        pushTempIntoBuffer(avgTemp, avgTempArray, GRAPH_COLUMNS);

        /* Graph */
        OSRAMClear();
        drawAxis();

        for (int i = 0; i < GRAPH_COLUMNS ; i++){
            char *character = getColumnOctal(avgTempArray[i]);
            int row = (avgTempArray[i] < 16) ? 1 : 0;
            OSRAMImageDraw(character, i+GRAPH_X_INIT+1, row, 1, 1);
        }
    }
}

void vReceiveCharTask( void *pvParameters ){

    char receivedChar;
    int aux_n;
    while(1){
        xQueueReceive(xUARTQueue, &receivedChar, portMAX_DELAY);

        /* Check if the received character is a digit or Enter */
        if (receivedChar >= '0' && receivedChar <= '9')
        {
            if (UARTBufferIndex < UART_BUFFER_SIZE - 1) {
                UARTInputBuffer[UARTBufferIndex++] = receivedChar;
                UARTInputBuffer[UARTBufferIndex] = '\0'; // Null-terminate the string
            }
        }
        else if (receivedChar == '\r' || receivedChar == '\n') // Enter key
        {
            if (UARTBufferIndex > 0) {
                aux_n = atoi(UARTInputBuffer);
                UARTBufferIndex = 0; // Reset the buffer index
                if (aux_n > MAX_BUFFER_SIZE) {
                    UARTSendString("The number entered exceeds the maximum stored temperatures. This maximum will be taken for the filter.\n");
                    aux_n = MAX_BUFFER_SIZE;
                }
                char auxBuffer[16];
                itoa(aux_n, auxBuffer, 10);
                current_n = aux_n;
                UARTSendString("New value of n: ");
                UARTSendString(auxBuffer);
                UARTSendString("\n");
            }
        }
        else
        {
            UARTSendString("Invalid character.\n");
        }
    }
}

void vTopTask( void *pvParameters ){
    UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
	pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

    while(1){
        vTaskDelay(configTOP_DELAY);

        if (MONITORING_STACK_WATER_MARK){
            continue;
        }

        printTop();
    }
}

void vMonitorTask(void *pvParameters) {
    for (;;) {
        // Obtener el uso del stack de cada tarea
        UBaseType_t uxHighWaterMark;

        uxHighWaterMark = uxTaskGetStackHighWaterMark(xTemperatureSensorTaskHandle);
        UARTSendString("Temperature Sensor Task Stack High Water Mark: ");
        char buffer[16];
        itoa(uxHighWaterMark, buffer, 10);
        UARTSendString(buffer);
        UARTSendString("\n");

        uxHighWaterMark = uxTaskGetStackHighWaterMark(xFilterTaskHandle);
        UARTSendString("Filter Task Stack High Water Mark: ");
        itoa(uxHighWaterMark, buffer, 10);
        UARTSendString(buffer);
        UARTSendString("\n");

        uxHighWaterMark = uxTaskGetStackHighWaterMark(xGraphTaskHandle);
        UARTSendString("Graph Task Stack High Water Mark: ");
        itoa(uxHighWaterMark, buffer, 10);
        UARTSendString(buffer);
        UARTSendString("\n");

        uxHighWaterMark = uxTaskGetStackHighWaterMark(xReceiveCharTaskHandle);
        UARTSendString("Receive Char Task Stack High Water Mark: ");
        itoa(uxHighWaterMark, buffer, 10);
        UARTSendString(buffer);
        UARTSendString("\n");

        // Esperar un tiempo antes de volver a verificar
        vTaskDelay(pdMS_TO_TICKS(5000)); // 5 segundos
    }
}

//--------------------CONFIG FUNCTIONS--------------------

void prvSetupHardware(void) {
	/* Enable the UART.  */
	SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
	UARTConfigSet(UART0_BASE, mainBAUD_RATE, (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
	UARTIntEnable(UART0_BASE, UART_INT_RX);
	IntPrioritySet(INT_UART0, configKERNEL_INTERRUPT_PRIORITY);
	IntEnable(INT_UART0);

    /* Setup display */
    OSRAMInit(false);
}

void prvConfigTimer(void){
    /* Setup timer */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    IntMasterEnable();
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    TimerConfigure(TIMER0_BASE, TIMER_CFG_32_BIT_TIMER);
    TimerLoadSet(TIMER0_BASE, TIMER_A, 1500);
    TimerIntRegister(TIMER0_BASE, TIMER_A, vTimer0IntHandler);
    TimerEnable(TIMER0_BASE, TIMER_A);
}
/*-----------------------------------------------------------*/

// ------------------------- UTIL FUNCTIONS -------------------------

/**
 * @brief Random number generator.
 *
 * Took from https://github.com/istarc/freertos/blob/master/FreeRTOS/Demo/CORTEX_A5_SAMA5D3x_Xplained_IAR/AtmelFiles/libboard_sama5d3x-ek/source/rand.c
 *
 * @return A random number.
 */
int uiGetRandomNumber( void ) {
	_dwRandNext = _dwRandNext * 1103515245 + 12345 ;

    return (uint32_t)(_dwRandNext/131072) % 65536 ;
}

/**
 * @brief Gets the octal representation of a column value.
 * 
 * The graph is divided in two rows of 8 pixels each.
 * So for the first row (bottom one) we need to graph the value of the x Axis and the dot of the graph.
 * For the second row (top one) we need to graph just the dot of the graph.
 * 
 * So for example if we have a temperature of 10, the binary representation of the column would be:
 * 1001000 = \220 -> Beign the first 1 the x Axis and the second 1 the dot of the graph.
 * 
 * @param value The column value.
 * @return A pointer to the octal representation string.
 */
char* getColumnOctal(int value)
{   
    if (value < 2) 			return "\200";
	else if (value < 4) 	return "\300";
	else if (value < 8) 	return "\240";
	else if (value < 10) 	return "\220";
	else if (value < 12) 	return "\210";
	else if (value < 14) 	return "\204";
	else if (value < 15) 	return "\202";
	else if (value < 16) 	return "\201"; 		// Last value of first row
	else if (value < 20) 	return "\200";
	else if (value < 22) 	return "\100";
	else if (value < 24) 	return "\040";
	else if (value < 25) 	return "\020";
	else if (value < 26) 	return "\010";
	else if (value < 28) 	return "\004";
	else if (value < 29) 	return "\002";
	else if (value <= 30)	return "\001";		// Last value of second row
}

/**
 * @brief Draws the axis on the display.
 * 
 * OSRAMImageDraw( const char *pcImage, int x, int y, int width, int height )
 * 
 * Each character is a representation of an octal or ascii value, that is then converted to binary.
 * The image data is organized such that each byte represents a column of 8 pixels. 
 * The LSB is the topmost pixel, and the MSB is the bottommost pixel.
 * 
 * So for example \177 = 01111111
 * 
 * Or if i want to draw the number 0:
 * \070 = 00111000 -> First columnn
 * \104 = 01000100 -> Second column
 * \104 = 01000100 -> Third column
 * \070 = 00111000 -> Fourth column
 */
void drawAxis( void )
{
	/* Y Axis */
	OSRAMImageDraw("\377", GRAPH_X_INIT, 0, 1, 1);
	OSRAMImageDraw("\377", GRAPH_X_INIT, 1, 1, 1);

	/* X Axis */
	for (int i=0 ; i<GRAPH_COLUMNS ; i++)
		OSRAMImageDraw("\200", i+GRAPH_X_INIT+1, 1, 1, 1); 			// +GRAPH_X_INIT+1 to skip the Y Axis

	/* Draw the current value of the array size */
    char bufer[16];
    itoa(current_n, bufer, 10);
    OSRAMStringDraw(bufer, 4, 1);
}

/**
 * @brief Push a temperature value into the first place of the buffer
 * 
 * @param temp The temperature value to push into the buffer.
 * @param temperatureBuffer The buffer where the temperature value will be pushed.
 * @param buffer_size The size of the buffer.
 */
void pushTempIntoBuffer(int temp, int *temperatureBuffer, int buffer_size) {
    for(int i = buffer_size - 1; i > 0; i--) {
        temperatureBuffer[i] = temperatureBuffer[i - 1];
    }
    temperatureBuffer[0] = temp;
}

/**
 * @brief Reverse a string.
 * 
 * @param str The string to reverse.
 * @param length The length of the string.
 */
void reverse(char str[], int length){
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        end--;
        start++;
    }
}

/** 
 * @brief Convert integer to string
 * 
 * Implementation of stdio itoa()
 * 
 * @param num The number to convert.
 * @param str The string where the number will be stored.
 * @param base The base of the number.
 * */
char* itoa(int num, char* str, int base){
    int i = 0;
    int isNegative = 0;
 
    /* Handle 0 explicitly, otherwise empty string is
     * printed for 0 */
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }
 
    // In standard itoa(), negative numbers are handled
    // only with base 10. Otherwise numbers are
    // considered unsigned.
    if (num < 0 && base == 10) {
        isNegative = 1;
        num = -num;
    }
 
    // Process individual digits
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }
 
    // If number is negative, append '-'
    if (isNegative)
        str[i++] = '-';
 
    str[i] = '\0'; // Append string terminator
 
    // Reverse the string
    reverse(str, i);
 
    return str;
}

/**
 * @brief Enviar una cadena de caracteres por UART.
 *
 * @param str La cadena de caracteres a enviar.
 */
void UARTSendString(const char *str) {
    while (*str) {
        UARTCharPut(UART0_BASE, *str++);
        UARTCharPut(UART0_BASE, '\0');
    }
}

/**
 * @brief Convert a string to an integer.
 * 
 * @param str The string to convert.
 * 
 * @return The integer value of the string.
 */
int atoi(char *str){
    int res = 0;

    for (int i = 0; str[i] != '\0'; i++){
        res = res * 10 + (str[i] - '0');
    }
    return res; 
}

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName) {
    // Detener el sistema o reiniciar
    OSRAMClear();
    OSRAMStringDraw("Stack Overflow", 0, 0);
    OSRAMStringDraw(pcTaskName, 0, 1);
    while (1);
}

unsigned long ulGetRunTimeCounterValue(void) {
    return runTimeCounter;
}

void printTop(void){
    volatile UBaseType_t uxArraySize;
    volatile UBaseType_t x;
    unsigned long ulTotalRunTime;
    unsigned long ulStatsAsPercentage;

    char counter[12];
    char percentage[12];
    char stack[12];

    if (pxTaskStatusArray != NULL) 
    {
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);
    
        ulTotalRunTime /= 100UL;

        UARTSendString("\r");

        if (ulTotalRunTime > 0) 
        {
            UARTSendString("TASK\tCPU%\tSTACK FREE\tTICKS\r\n");
            UARTSendString("---------------------------------------\r\n");

            for (x = 0; x < uxArraySize; x++) 
            {
                ulStatsAsPercentage = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalRunTime;

                itoa(pxTaskStatusArray[x].ulRunTimeCounter, counter, 10);
                itoa(ulStatsAsPercentage, percentage, 10);
                itoa(pxTaskStatusArray[x].usStackHighWaterMark, stack, 10);

                UARTSendString(pxTaskStatusArray[x].pcTaskName);
                UARTSendString("\t");
                UARTSendString(ulStatsAsPercentage > 0 ? percentage : "<1");
                UARTSendString("%\t");
                UARTSendString(stack);
                UARTSendString("\t\t");
                UARTSendString(counter);
                UARTSendString("\r\n");
            }

            UARTSendString("\r\n\r\n\r\n");
        }
    }
}

// ------------------------- ISR --------------------------------

void vUART_ISR(void)
{
    unsigned long ulStatus;

	/* What caused the interrupt. */
	ulStatus = UARTIntStatus( UART0_BASE, pdTRUE );

    char receivedChar;

	/* Clear the interrupt. */
	UARTIntClear( UART0_BASE, ulStatus );

	/* Was a Tx interrupt pending? */
	if( ulStatus & UART_INT_RX )
	{
		/* Read the received character */
        receivedChar = UARTCharGet(UART0_BASE);

        xQueueSendFromISR(xUARTQueue, &receivedChar, NULL);
	}
}

void vTimer0IntHandler(void){
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    runTimeCounter++;
}
