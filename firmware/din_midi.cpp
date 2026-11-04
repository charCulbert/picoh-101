// DIN MIDI in on UART1. The interrupt only stores bytes; the control loop
// parses them, so a slow loop delays notes but never loses them unless it
// falls 256 bytes (about 80 ms) behind.
#include "board.h"
#include "midi_parser.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

namespace
{
uint8_t ring[256];
volatile uint8_t head; // written by the interrupt
uint8_t tail;
MidiParser parser;

void received()
{
    while (uart_is_readable (uart1))
    {
        uint8_t byte = uart_getc (uart1);
        uint8_t at = head;
        if (uint8_t (at + 1) != tail)
        {
            ring[at] = byte;
            head = uint8_t (at + 1);
        } // full: drop the newest
    }
}
} // namespace

namespace din
{
void start()
{
    uart_init (uart1, 31250);
    gpio_set_function (rxPin, GPIO_FUNC_UART);
    gpio_pull_up (rxPin); // idle high with nothing plugged in
    uart_set_fifo_enabled (uart1, true);
    irq_set_exclusive_handler (UART1_IRQ, received);
    irq_set_enabled (UART1_IRQ, true);
    uart_set_irq_enables (uart1, true, false);
}

void poll (Control& control)
{
    uint8_t message[3];
    while (tail != head)
        if (parser.feed (ring[tail++], message))
            control.midi (message, EVENT_PORT_DIN);
}
} // namespace din
