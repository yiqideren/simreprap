/*
	simavr.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include "simavr.h"
#include "sim_elf.h"

#include "sim_core.h"
#include "avr_eeprom.h"
#include "avr_uart.h"

void hdump(const char *w, uint8_t *b, size_t l)
{
	uint32_t i;
	if (l < 16) {
		printf("%s: ",w);
		for (i = 0; i < l; i++) printf("%02x",b[i]);
	} else {
		printf("%s:\n",w);
		for (i = 0; i < l; i++) {
			if (!(i & 0x1f)) printf("    ");
			printf("%02x",b[i]);
			if ((i & 0x1f) == 0x1f) {
				printf(" ");
				printf("\n");
			}
		}
	}
	printf("\n");
}



int avr_init(avr_t * avr)
{
	avr->flash = malloc(avr->flashend + 1);
	memset(avr->flash, 0xff, avr->flashend + 1);
	avr->data = malloc(avr->ramend + 1);
	memset(avr->data, 0, avr->ramend + 1);

	// cpu is in limbo before init is finished.
	avr->state = cpu_Limbo;
	avr->frequency = 1000000;	// can be overriden via avr_mcu_section
	if (avr->init)
		avr->init(avr);
	avr->state = cpu_Running;
	avr_reset(avr);	
	return 0;
}

void avr_reset(avr_t * avr)
{
	memset(avr->data, 0x0, avr->ramend + 1);
	_avr_sp_set(avr, avr->ramend);
	avr->pc = 0;
	for (int i = 0; i < 8; i++)
		avr->sreg[i] = 0;
	if (avr->reset)
		avr->reset(avr);

	avr_io_t * port = avr->io_port;
	while (port) {
		if (port->reset)
			port->reset(avr, port);
		port = port->next;
	}

}


void avr_loadcode(avr_t * avr, uint8_t * code, uint32_t size, uint32_t address)
{
	memcpy(avr->flash + address, code, size);
}

void avr_core_watch_write(avr_t *avr, uint16_t addr, uint8_t v)
{
	if (addr > avr->ramend) {
		printf("*** Invalid write address PC=%04x SP=%04x O=%04x Address %04x=%02x out of ram\n",
				avr->pc, _avr_sp_get(avr), avr->flash[avr->pc] | (avr->flash[avr->pc]<<8), addr, v);
		CRASH();
	}
	if (addr < 32) {
		printf("*** Invalid write address PC=%04x SP=%04x O=%04x Address %04x=%02x low registers\n",
				avr->pc, _avr_sp_get(avr), avr->flash[avr->pc] | (avr->flash[avr->pc]<<8), addr, v);
		CRASH();
	}
#if AVR_STACK_WATCH
	/*
	 * this checks that the current "function" is not doctoring the stack frame that is located
	 * higher on the stack than it should be. It's a sign of code that has overrun it's stack
	 * frame and is munching on it's own return address.
	 */
	if (avr->stack_frame_index > 1 && addr > avr->stack_frame[avr->stack_frame_index-2].sp) {
		printf("\e[31m%04x : munching stack SP %04x, A=%04x <= %02x\e[0m\n", avr->pc, _avr_sp_get(avr), addr, v);
	}
#endif
	avr->data[addr] = v;
}

uint8_t avr_core_watch_read(avr_t *avr, uint16_t addr)
{
	if (addr > avr->ramend) {
		printf("*** Invalid read address PC=%04x SP=%04x O=%04x Address %04x out of ram (%04x)\n",
				avr->pc, _avr_sp_get(avr), avr->flash[avr->pc] | (avr->flash[avr->pc]<<8), addr, avr->ramend);
		CRASH();
	}
	return avr->data[addr];
}


int avr_run(avr_t * avr)
{
	if (avr->state == cpu_Stopped)
		return avr->state;

	uint16_t new_pc = avr->pc;

	if (avr->state == cpu_Running) {
		new_pc = avr_run_one(avr);
		avr_dump_state(avr);
	} else
		avr->cycle ++;

	// re-synth the SREG
	//SREG();
	// if we just re-enabled the interrupts...
	if (avr->sreg[S_I] && !(avr->data[R_SREG] & (1 << S_I))) {
	//	printf("*** %s: Renabling interrupts\n", __FUNCTION__);
		avr->pending_wait++;
	}
	avr_io_t * port = avr->io_port;
	while (port) {
		if (port->run)
			port->run(avr, port);
		port = port->next;
	}

	avr->pc = new_pc;

	if (avr->state == cpu_Sleeping) {
		if (!avr->sreg[S_I]) {
			printf("simavr: sleeping with interrupts off, quitting gracefuly\n");
			exit(0);
		}
		usleep(500);
		long sleep = (float)avr->frequency * (1.0f / 500.0f);
		avr->cycle += sleep;
	//	avr->state = cpu_Running;
	}
	// Interrupt servicing might change the PC too
	if (avr->state == cpu_Running || avr->state == cpu_Sleeping) {
		avr_service_interrupts(avr);

		avr->data[R_SREG] = 0;
		for (int i = 0; i < 8; i++)
			if (avr->sreg[i] > 1) {
				printf("** Invalid SREG!!\n");
				CRASH();
			} else if (avr->sreg[i])
				avr->data[R_SREG] |= (1 << i);
	}
	return avr->state;
}

extern avr_kind_t tiny85;
extern avr_kind_t mega48,mega88,mega168;
extern avr_kind_t mega644;

avr_kind_t * avr_kind[] = {
	&tiny85,
	&mega48,
	&mega88,
	&mega168,
	&mega644,
	NULL
};

void display_usage()
{
	printf("usage: simavr [-t] [-m <device>] [-f <frequency>] firmware\n");
	printf("       -t: run full scale decoder trace\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	elf_firmware_t f;
	long f_cpu = 0;
	int trace = 0;
	char name[16] = "";
	int option_count;
	int option_index = 0;

	struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"mcu", required_argument, 0, 'm'},
		{"freq", required_argument, 0, 'f'},
		{"trace", no_argument, 0, 't'},
		{0, 0, 0, 0}
	};

	if (argc == 1)
		display_usage();

	while ((option_count = getopt_long(argc, argv, "thm:f:", long_options, &option_index)) != -1) {
		switch (option_count) {
			case 'h':
				display_usage();
				break;
			case 'm':
				strcpy(name, optarg);
				break;
			case 'f':
				f_cpu = atoi(optarg);
				break;
			case 't':
				trace++;
				break;
		}
	}

	elf_read_firmware(argv[argc-1], &f);

	if (strlen(name))
		strcpy(f.mmcu.name, name);
	if (f_cpu)
		f.mmcu.f_cpu = f_cpu;

	printf("firmware %s f=%ld mmcu=%s\n", argv[argc-1], f.mmcu.f_cpu, f.mmcu.name);

	avr_kind_t * maker = NULL;
	for (int i = 0; avr_kind[i] && !maker; i++) {
		for (int j = 0; avr_kind[i]->names[j]; j++)
			if (!strcmp(avr_kind[i]->names[j], f.mmcu.name)) {
				maker = avr_kind[i];
				break;
			}
	}
	if (!maker) {
		fprintf(stderr, "%s: AVR '%s' now known\n", argv[0], f.mmcu.name);
		exit(1);
	}

	avr_t * avr = maker->make();
	printf("Starting %s - flashend %04x ramend %04x e2end %04x\n", avr->mmcu, avr->flashend, avr->ramend, avr->e2end);
	avr_init(avr);
	avr->frequency = f.mmcu.f_cpu;
	avr->codeline = f.codeline;
	avr_loadcode(avr, f.flash, f.flashsize, 0);
	avr->codeend = f.flashsize - f.datasize;
	if (f.eeprom && f.eesize) {
		avr_eeprom_desc_t d = { .ee = f.eeprom, .offset = 0, .size = f.eesize };
		avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &d);
	}
	avr->trace = trace;

	// try to enable "local echo" on the first uart, for testing purposes
	{
		avr_irq_t * src = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT);
		avr_irq_t * dst = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);
		printf("%s:%s activating uart local echo IRQ src %p dst %p\n", __FILE__, __FUNCTION__, src, dst);
		if (src && dst)
			avr_connect_irq(avr, src, dst);
	}

	for (long long i = 0; i < 8000000*10; i++)
//	for (long long i = 0; i < 80000; i++)
		avr_run(avr);
	
}
