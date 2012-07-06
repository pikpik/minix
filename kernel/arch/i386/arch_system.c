/* system dependent functions for use inside the whole kernel. */

#include "kernel/kernel.h"

#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <machine/cmos.h>
#include <machine/bios.h>
#include <minix/portio.h>
#include <minix/cpufeature.h>
#include <assert.h>
#include <signal.h>
#include <machine/vm.h>


#include "arch_proto.h"
#include "serial.h"
#include "oxpcie.h"
#include "kernel/proc.h"
#include "kernel/debug.h"
#include "mb_utils.h"
#include <machine/multiboot.h>

#include "glo.h"

#ifdef USE_APIC
#include "apic.h"
#endif

#ifdef USE_ACPI
#include "acpi.h"
#endif

static int osfxsr_feature; /* FXSAVE/FXRSTOR instructions support (SSEx) */

extern __dead void poweroff_jmp();
extern void poweroff16();
extern void poweroff16_end();

/* set MP and NE flags to handle FPU exceptions in native mode. */
#define CR0_MP_NE	0x0022
/* set CR4.OSFXSR[bit 9] if FXSR is supported. */
#define CR4_OSFXSR	(1L<<9)
/* set OSXMMEXCPT[bit 10] if we provide #XM handler. */
#define CR4_OSXMMEXCPT	(1L<<10)

void * k_stacks;

static void ser_debug(int c);
#ifdef CONFIG_SMP
static void ser_dump_proc_cpu(void);
#endif
#if !CONFIG_OXPCIE
static void ser_init(void);
#endif

#define     KBCMDP          4       /* kbd controller port (O) */
#define      KBC_PULSE0     0xfe    /* pulse output bit 0 */
#define      IO_KBD          0x060           /* 8042 Keyboard */

void
reset(void)
{
        uint8_t b;
        /*
         * The keyboard controller has 4 random output pins, one of which is
         * connected to the RESET pin on the CPU in many PCs.  We tell the
         * keyboard controller to pulse this line a couple of times.
         */
        outb(IO_KBD + KBCMDP, KBC_PULSE0);
        busy_delay_ms(100);
        outb(IO_KBD + KBCMDP, KBC_PULSE0);
        busy_delay_ms(100);

        /*
         * Attempt to force a reset via the Reset Control register at
         * I/O port 0xcf9.  Bit 2 forces a system reset when it
         * transitions from 0 to 1.  Bit 1 selects the type of reset
         * to attempt: 0 selects a "soft" reset, and 1 selects a
         * "hard" reset.  We try a "hard" reset.  The first write sets
         * bit 1 to select a "hard" reset and clears bit 2.  The
         * second write forces a 0 -> 1 transition in bit 2 to trigger
         * a reset.
         */
        outb(0xcf9, 0x2);
        outb(0xcf9, 0x6);
        busy_delay_ms(500);  /* wait 0.5 sec to see if that did it */

        /*
         * Attempt to force a reset via the Fast A20 and Init register
         * at I/O port 0x92.  Bit 1 serves as an alternate A20 gate.
         * Bit 0 asserts INIT# when set to 1.  We are careful to only
         * preserve bit 1 while setting bit 0.  We also must clear bit
         * 0 before setting it if it isn't already clear.
         */
        b = inb(0x92);
        if (b != 0xff) {
                if ((b & 0x1) != 0)
                        outb(0x92, b & 0xfe);
                outb(0x92, b | 0x1);
                busy_delay_ms(500);  /* wait 0.5 sec to see if that did it */
        }

	/* Triple fault */
	x86_triplefault();

	/* Give up on resetting */
	while(1) {
		;
	}
}

static __dead void arch_bios_poweroff(void)
{
	u32_t cr0;
	
	/* Disable paging */
	cr0 = read_cr0();
	cr0 &= ~I386_CR0_PG;
	write_cr0(cr0);
	/* Copy 16-bit poweroff code to below 1M */
	phys_copy(
		(u32_t)&poweroff16,
		BIOS_POWEROFF_ENTRY,
		(u32_t)&poweroff16_end-(u32_t)&poweroff16);
	poweroff_jmp();
}

int cpu_has_tsc;

__dead void arch_shutdown(int how)
{
	vm_stop();

	/* Mask all interrupts, including the clock. */
	outb( INT_CTLMASK, ~0);

	if(minix_panicing) {
		unsigned char unused_ch;
		/* We're panicing? Then retrieve and decode currently
		 * loaded segment selectors.
		 */
		printseg("cs: ", 1, get_cpulocal_var(proc_ptr), read_cs());
		printseg("ds: ", 0, get_cpulocal_var(proc_ptr), read_ds());
		if(read_ds() != read_ss()) {
			printseg("ss: ", 0, NULL, read_ss());
		}

		/* Printing is done synchronously over serial. */
		if (do_serial_debug)
			reset();

		/* Print accumulated diagnostics buffer and reset. */
		mb_cls();
		mb_print("Minix panic. System diagnostics buffer:\n\n");
		mb_print(kmess_buf);
		mb_print("\nSystem has panicked, press any key to reboot");
		while (!mb_read_char(&unused_ch))
			;
		reset();
	}

	if (how == RBT_DEFAULT) {
		how = RBT_RESET;
	}

	switch (how) {

		case RBT_HALT:
			/* Poweroff without boot monitor */
			arch_bios_poweroff();
			NOT_REACHABLE;

		case RBT_PANIC:
			/* Allow user to read panic message */
			for (; ; ) halt_cpu();
			NOT_REACHABLE;

		default:	
		case RBT_REBOOT:
		case RBT_RESET:
			/* Reset the system by forcing a processor shutdown. 
			 * First stop the BIOS memory test by setting a soft
			 * reset flag.
			 */
			reset();
			NOT_REACHABLE;
	}

	NOT_REACHABLE;
}

void fpu_init(void)
{
	unsigned short cw, sw;

	fninit();
	sw = fnstsw();
	fnstcw(&cw);

	if((sw & 0xff) == 0 &&
	   (cw & 0x103f) == 0x3f) {
		/* We have some sort of FPU, but don't check exact model.
		 * Set CR0_NE and CR0_MP to handle fpu exceptions
		 * in native mode. */
		write_cr0(read_cr0() | CR0_MP_NE);
		get_cpulocal_var(fpu_presence) = 1;
		if(_cpufeature(_CPUF_I386_FXSR)) {
			u32_t cr4 = read_cr4() | CR4_OSFXSR; /* Enable FXSR. */

			/* OSXMMEXCPT if supported
			 * FXSR feature can be available without SSE
			 */
			if(_cpufeature(_CPUF_I386_SSE))
				cr4 |= CR4_OSXMMEXCPT; 

			write_cr4(cr4);
			osfxsr_feature = 1;
		} else {
			osfxsr_feature = 0;
		}
	} else {
		/* No FPU presents. */
		get_cpulocal_var(fpu_presence) = 0;
                osfxsr_feature = 0;
                return;
        }
}

void save_local_fpu(struct proc *pr, int retain)
{
	char *state = pr->p_seg.fpu_state;

	/* Save process FPU context. If the 'retain' flag is set, keep the FPU
	 * state as is. If the flag is not set, the state is undefined upon
	 * return, and the caller is responsible for reloading a proper state.
	 */

	if(!is_fpu())
		return;

	assert(state);

	if(osfxsr_feature) {
		fxsave(state);
	} else {
		fnsave(state);
		if (retain)
			(void) frstor(state);
	}
}

void save_fpu(struct proc *pr)
{
#ifdef CONFIG_SMP
	if (cpuid != pr->p_cpu) {
		int stopped;

		/* remember if the process was already stopped */
		stopped = RTS_ISSET(pr, RTS_PROC_STOP);

		/* stop the remote process and force its context to be saved */
		smp_schedule_stop_proc_save_ctx(pr);

		/*
		 * If the process wasn't stopped let the process run again. The
		 * process is kept block by the fact that the kernel cannot run
		 * on its cpu
		 */
		if (!stopped)
			RTS_UNSET(pr, RTS_PROC_STOP);

		return;
	}
#endif

	if (get_cpulocal_var(fpu_owner) == pr) {
		disable_fpu_exception();
		save_local_fpu(pr, TRUE /*retain*/);
	}
}

/* reserve a chunk of memory for fpu state; every one has to
 * be FPUALIGN-aligned.
 */
static char fpu_state[NR_PROCS][FPU_XFP_SIZE] __aligned(FPUALIGN);

void arch_proc_init(int nr, struct proc *pr)
{
	if(nr < 0) return;
	char *v;

	assert(nr < NR_PROCS);

	v = fpu_state[nr];

	/* verify alignment */
	assert(!((vir_bytes)v % FPUALIGN));
	
	pr->p_seg.fpu_state = v;
}

int restore_fpu(struct proc *pr)
{
	int failed;
	char *state = pr->p_seg.fpu_state;

	assert(state);

	if(!proc_used_fpu(pr)) {
		fninit();
		pr->p_misc_flags |= MF_FPU_INITIALIZED;
	} else {
		if(osfxsr_feature) {
			failed = fxrstor(state);
		} else {
			failed = frstor(state);
		}

		if (failed) return EINVAL;
	}

	return OK;
}

void cpu_identify(void)
{
	u32_t eax, ebx, ecx, edx;
	unsigned cpu = cpuid;
	
	eax = 0;
	_cpuid(&eax, &ebx, &ecx, &edx);

	if (ebx == INTEL_CPUID_GEN_EBX && ecx == INTEL_CPUID_GEN_ECX &&
			edx == INTEL_CPUID_GEN_EDX) {
		cpu_info[cpu].vendor = CPU_VENDOR_INTEL;
	} else if (ebx == AMD_CPUID_GEN_EBX && ecx == AMD_CPUID_GEN_ECX &&
			edx == AMD_CPUID_GEN_EDX) {
		cpu_info[cpu].vendor = CPU_VENDOR_AMD;
	} else
		cpu_info[cpu].vendor = CPU_VENDOR_UNKNOWN;

	if (eax == 0) 
		return;

	eax = 1;
	_cpuid(&eax, &ebx, &ecx, &edx);

	cpu_info[cpu].family = (eax >> 8) & 0xf;
	if (cpu_info[cpu].family == 0xf)
		cpu_info[cpu].family += (eax >> 20) & 0xff;
	cpu_info[cpu].model = (eax >> 4) & 0xf;
	if (cpu_info[cpu].model == 0xf || cpu_info[cpu].model == 0x6)
		cpu_info[cpu].model += ((eax >> 16) & 0xf) << 4 ;
	cpu_info[cpu].stepping = eax & 0xf;
	cpu_info[cpu].flags[0] = ecx;
	cpu_info[cpu].flags[1] = edx;
}

void arch_init(void)
{
#ifdef USE_APIC
	/*
	 * this is setting kernel segments to cover most of the phys memory. The
	 * value is high enough to reach local APIC nad IOAPICs before paging is
	 * turned on.
	 */
	prot_set_kern_seg_limit(0xfff00000);
	reload_ds();
#endif

	idt_init();

	/* FIXME stupid a.out
	 * align the stacks in the stack are to the K_STACK_SIZE which is a
	 * power of 2
	 */
	k_stacks = (void*) (((vir_bytes)&k_stacks_start + K_STACK_SIZE - 1) &
							~(K_STACK_SIZE - 1));

#ifndef CONFIG_SMP
	/*
	 * use stack 0 and cpu id 0 on a single processor machine, SMP
	 * configuration does this in smp_init() for all cpus at once
	 */
	tss_init(0, get_k_stack_top(0));
#endif

#if !CONFIG_OXPCIE
	ser_init();
#endif

#ifdef USE_ACPI
	acpi_init();
#endif

#if defined(USE_APIC) && !defined(CONFIG_SMP)
	if (config_no_apic) {
		BOOT_VERBOSE(printf("APIC disabled, using legacy PIC\n"));
	}
	else if (!apic_single_cpu_init()) {
		BOOT_VERBOSE(printf("APIC not present, using legacy PIC\n"));
	}
#endif
}

#ifdef DEBUG_SERIAL
void ser_putc(char c)
{
        int i;
        int lsr, thr;

#if CONFIG_OXPCIE
	oxpcie_putc(c);
#else
        lsr= COM1_LSR;
        thr= COM1_THR;
        for (i= 0; i<100000; i++)
        {
                if (inb( lsr) & LSR_THRE)
                        break;
        }
        outb( thr, c);
#endif
}


/*===========================================================================*
 *				do_ser_debug				     * 
 *===========================================================================*/
void do_ser_debug()
{
	u8_t c, lsr;

#if CONFIG_OXPCIE
	{
		int oxin;
		if((oxin = oxpcie_in()) >= 0)
		ser_debug(oxin);
	}
#endif

	lsr= inb(COM1_LSR);
	if (!(lsr & LSR_DR))
		return;
	c = inb(COM1_RBR);
	ser_debug(c);
}

static void ser_dump_queue_cpu(unsigned cpu)
{
	int q;
	struct proc ** rdy_head;
	
	rdy_head = get_cpu_var(cpu, run_q_head);

	for(q = 0; q < NR_SCHED_QUEUES; q++) {
		struct proc *p;
		if(rdy_head[q])	 {
			printf("%2d: ", q);
			for(p = rdy_head[q]; p; p = p->p_nextready) {
				printf("%s / %d  ", p->p_name, p->p_endpoint);
			}
			printf("\n");
		}
	}
}

static void ser_dump_queues(void)
{
#ifdef CONFIG_SMP
	unsigned cpu;

	printf("--- run queues ---\n");
	for (cpu = 0; cpu < ncpus; cpu++) {
		printf("CPU %d :\n", cpu);
		ser_dump_queue_cpu(cpu);
	}
#else
	ser_dump_queue_cpu(0);
#endif
}

static void ser_dump_segs(void)
{
	struct proc *pp;
	for (pp= BEG_PROC_ADDR; pp < END_PROC_ADDR; pp++)
	{
		if (isemptyp(pp))
			continue;
		printf("%d: %s ep %d\n", proc_nr(pp), pp->p_name, pp->p_endpoint);
		printseg("cs: ", 1, pp, pp->p_reg.cs);
		printseg("ds: ", 0, pp, pp->p_reg.ds);
		if(pp->p_reg.ss != pp->p_reg.ds) {
			printseg("ss: ", 0, pp, pp->p_reg.ss);
		}
	}
}

#ifdef CONFIG_SMP
static void dump_bkl_usage(void)
{
	unsigned cpu;

	printf("--- BKL usage ---\n");
	for (cpu = 0; cpu < ncpus; cpu++) {
		printf("cpu %3d kernel ticks 0x%x%08x bkl ticks 0x%x%08x succ %d tries %d\n", cpu,
				(unsigned long)(kernel_ticks[cpu]>>32),
				(unsigned long)kernel_ticks[cpu],
				(unsigned long)(bkl_ticks[cpu]>>32),
				(unsigned long)bkl_ticks[cpu],
				bkl_succ[cpu], bkl_tries[cpu]);
	}
}

static void reset_bkl_usage(void)
{
	memset(kernel_ticks, 0, sizeof(kernel_ticks));
	memset(bkl_ticks, 0, sizeof(bkl_ticks));
	memset(bkl_tries, 0, sizeof(bkl_tries));
	memset(bkl_succ, 0, sizeof(bkl_succ));
}
#endif

static void ser_debug(const int c)
{
	serial_debug_active = 1;

	switch(c)
	{
	case 'Q':
		minix_shutdown(NULL);
		NOT_REACHABLE;
#ifdef CONFIG_SMP
	case 'B':
		dump_bkl_usage();
		break;
	case 'b':
		reset_bkl_usage();
		break;
#endif
	case '1':
		ser_dump_proc();
		break;
	case '2':
		ser_dump_queues();
		break;
	case '3':
		ser_dump_segs();
		break;
#ifdef CONFIG_SMP
	case '4':
		ser_dump_proc_cpu();
		break;
#endif
#if DEBUG_TRACE
#define TOGGLECASE(ch, flag)				\
	case ch: {					\
		if(verboseflags & flag)	{		\
			verboseflags &= ~flag;		\
			printf("%s disabled\n", #flag);	\
		} else {				\
			verboseflags |= flag;		\
			printf("%s enabled\n", #flag);	\
		}					\
		break;					\
		}
	TOGGLECASE('8', VF_SCHEDULING)
	TOGGLECASE('9', VF_PICKPROC)
#endif
#ifdef USE_APIC
	case 'I':
		dump_apic_irq_state();
		break;
#endif
	}
	serial_debug_active = 0;
}

void ser_dump_proc()
{
	struct proc *pp;

	for (pp= BEG_PROC_ADDR; pp < END_PROC_ADDR; pp++)
	{
		if (isemptyp(pp))
			continue;
		print_proc_recursive(pp);
	}
}

#ifdef CONFIG_SMP
static void ser_dump_proc_cpu(void)
{
	struct proc *pp;
	unsigned cpu;

	for (cpu = 0; cpu < ncpus; cpu++) {
		printf("CPU %d processes : \n", cpu);
		for (pp= BEG_USER_ADDR; pp < END_PROC_ADDR; pp++) {
			if (isemptyp(pp) || pp->p_cpu != cpu)
				continue;
			print_proc(pp);
		}
	}
}
#endif

#endif /* DEBUG_SERIAL */

#if SPROFILE

int arch_init_profile_clock(const u32_t freq)
{
  int r;
  /* Set CMOS timer frequency. */
  outb(RTC_INDEX, RTC_REG_A);
  outb(RTC_IO, RTC_A_DV_OK | freq);
  /* Enable CMOS timer interrupts. */
  outb(RTC_INDEX, RTC_REG_B);
  r = inb(RTC_IO);
  outb(RTC_INDEX, RTC_REG_B); 
  outb(RTC_IO, r | RTC_B_PIE);
  /* Mandatory read of CMOS register to enable timer interrupts. */
  outb(RTC_INDEX, RTC_REG_C);
  inb(RTC_IO);

  return CMOS_CLOCK_IRQ;
}

void arch_stop_profile_clock(void)
{
  int r;
  /* Disable CMOS timer interrupts. */
  outb(RTC_INDEX, RTC_REG_B);
  r = inb(RTC_IO);
  outb(RTC_INDEX, RTC_REG_B);  
  outb(RTC_IO, r & ~RTC_B_PIE);
}

void arch_ack_profile_clock(void)
{
  /* Mandatory read of CMOS register to re-enable timer interrupts. */
  outb(RTC_INDEX, RTC_REG_C);
  inb(RTC_IO);
}

#endif

/* Saved by mpx386.s into these variables. */
u32_t params_size, params_offset, mon_ds;

int arch_get_params(char *params, int maxsize)
{
	phys_copy(seg2phys(mon_ds) + params_offset, vir2phys(params),
		MIN(maxsize, params_size));
	params[maxsize-1] = '\0';
	return OK;
}

int arch_set_params(char *params, int size)
{
	if(size > params_size)
		return E2BIG;
	phys_copy(vir2phys(params), seg2phys(mon_ds) + params_offset, size);
	return OK;
}

void arch_do_syscall(struct proc *proc)
{
  /* do_ipc assumes that it's running because of the current process */
  assert(proc == get_cpulocal_var(proc_ptr));
  /* Make the system call, for real this time. */
  proc->p_reg.retreg =
	  do_ipc(proc->p_reg.cx, proc->p_reg.retreg, proc->p_reg.bx);
}

struct proc * arch_finish_switch_to_user(void)
{
	char * stk;
	struct proc * p;

#ifdef CONFIG_SMP
	stk = (char *)tss[cpuid].sp0;
#else
	stk = (char *)tss[0].sp0;
#endif
	/* set pointer to the process to run on the stack */
	p = get_cpulocal_var(proc_ptr);
	*((reg_t *)stk) = (reg_t) p;
	return p;
}

void fpu_sigcontext(struct proc *pr, struct sigframe *fr, struct sigcontext *sc)
{
	int fp_error;

	if (osfxsr_feature) {
		fp_error = sc->sc_fpu_state.xfp_regs.fp_status &
			~sc->sc_fpu_state.xfp_regs.fp_control;
	} else {
		fp_error = sc->sc_fpu_state.fpu_regs.fp_status &
			~sc->sc_fpu_state.fpu_regs.fp_control;
	}

	if (fp_error & 0x001) {      /* Invalid op */
		/*
		 * swd & 0x240 == 0x040: Stack Underflow
		 * swd & 0x240 == 0x240: Stack Overflow
		 * User must clear the SF bit (0x40) if set
		 */
		fr->sf_code = FPE_FLTINV;
	} else if (fp_error & 0x004) {
		fr->sf_code = FPE_FLTDIV; /* Divide by Zero */
	} else if (fp_error & 0x008) {
		fr->sf_code = FPE_FLTOVF; /* Overflow */
	} else if (fp_error & 0x012) {
		fr->sf_code = FPE_FLTUND; /* Denormal, Underflow */
	} else if (fp_error & 0x020) {
		fr->sf_code = FPE_FLTRES; /* Precision */
	} else {
		fr->sf_code = 0;  /* XXX - probably should be used for FPE_INTOVF or
				  * FPE_INTDIV */
	}
}

#if !CONFIG_OXPCIE
static void ser_init(void)
{
	unsigned char lcr;
	unsigned divisor;

	/* keep BIOS settings if cttybaud is not set */
	if (serial_debug_baud <= 0) return;

	/* set DLAB to make baud accessible */
	lcr = LCR_8BIT | LCR_1STOP | LCR_NPAR;
	outb(COM1_LCR, lcr | LCR_DLAB);

	/* set baud rate */
	divisor = UART_BASE_FREQ / serial_debug_baud;
	if (divisor < 1) divisor = 1;
	if (divisor > 65535) divisor = 65535;
	
	outb(COM1_DLL, divisor & 0xff);
	outb(COM1_DLM, (divisor >> 8) & 0xff);

	/* clear DLAB */
	outb(COM1_LCR, lcr);
}
#endif

