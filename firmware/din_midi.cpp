// DIN MIDI in on UART1. The interrupt stores each byte with the time it arrived; the control loop parses
// them, and a message is stamped with its last byte's time, so a slow loop doesn't move its notes. Bytes
// are lost only if the loop falls 256 bytes (about 80 ms) behind. The FIFO is off: with it on, a lone
// message would interrupt only 32 bit times (1 ms) after its last byte. The interrupt runs from RAM, so it
// keeps running while core 0 writes flash (flash_saves.h).
#include <atomic>
#include "board.h"
#include "midi_parser.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

namespace
{
uint8_t ring[256];
uint32_t arrivedUs[256];
std::atomic<uint8_t> head; // written by the interrupt, after the byte and its time
std::atomic<uint8_t> tail; // written by the loop, after it has read them
MidiParser parser;

void __not_in_flash_func (dinReceived)()
{
    while (uart_is_readable (uart1))
    {
        const uint32_t now = time_us_32();
        uint8_t byte = uart_getc (uart1);
        const uint8_t at = head.load (std::memory_order_relaxed);
        if (uint8_t (at + 1) != tail.load (std::memory_order_acquire))
        {
            ring[at] = byte;
            arrivedUs[at] = now;
            head.store (uint8_t (at + 1), std::memory_order_release);
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
    uart_set_fifo_enabled (uart1, false);
    irq_set_exclusive_handler (UART1_IRQ, dinReceived);
    irq_set_enabled (UART1_IRQ, true);
    uart_set_irq_enables (uart1, true, false);
}

void poll (Control& control)
{
    uint8_t message[3];
    for (uint8_t at = tail.load (std::memory_order_relaxed); at != head.load (std::memory_order_acquire);)
    {
        const uint32_t timeUs = arrivedUs[at];
        const uint8_t byte = ring[at++];
        tail.store (at, std::memory_order_release);
        if (parser.feed (byte, message))
            control.at (timeUs, [&] { control.midi (message, EVENT_PORT_DIN); });
    }
}
} // namespace din
