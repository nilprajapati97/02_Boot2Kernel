#include <stdint.h>

// Assume memory-mapped GPIO registers (example for ARM Cortex-M like STM32)
#define GPIO_PORT_DIR   (*(volatile uint32_t*)0x40020000)  // Direction register
#define GPIO_PORT_IN    (*(volatile uint32_t*)0x40020010)  // Input data register
#define GPIO_PORT_OUT   (*(volatile uint32_t*)0x40020014)  // Output data register

#define SWITCH_PIN   (1 << 0)   // Switch connected at pin 0
#define LED_PIN      (1 << 1)   // LED connected at pin 1

int main(void)
{
    // 1. Configure GPIO directions
    GPIO_PORT_DIR &= ~SWITCH_PIN;   // Switch = Input
    GPIO_PORT_DIR |= LED_PIN;       // LED = Output

    while (1)
    {
        // 2. Read switch state
        if (GPIO_PORT_IN & SWITCH_PIN)  
        {
            // Switch pressed → Turn LED ON
            GPIO_PORT_OUT |= LED_PIN;
        }
        else
        {
            // Switch released → Turn LED OFF
            GPIO_PORT_OUT &= ~LED_PIN;
        }
    }
}
