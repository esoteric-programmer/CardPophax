/* Record PB0 (IR LED) edges of the ATtiny firmware; PB1 (IR RX) held high = no light. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_hex.h>
#include <simavr/avr_ioport.h>
static avr_t *avr;
static void pb0(struct avr_irq_t *irq, uint32_t v, void *p)
{ (void)irq; (void)p; printf("%llu %u\n", (unsigned long long)avr->cycle, v); }
int main(int argc, char **argv)
{
    uint32_t base, size;
    uint8_t *img = read_ihex_file(argv[1], &size, &base);
    double secs = atof(argv[2]);
    avr = avr_make_mcu_by_name("attiny85");
    avr_init(avr);
    avr->frequency = 8000000;
    memcpy(avr->flash + base, img, size);
    avr->pc = base;
    avr->codeend = avr->flashend;
    avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0), pb0, NULL);
    avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 1), 1);
    while (avr->cycle < (avr_cycle_count_t)(secs * 8e6)) {
        int st = avr_run(avr);
        if (st == cpu_Done || st == cpu_Crashed) { fprintf(stderr, "cpu state %d\n", st); break; }
    }
    return 0;
}
