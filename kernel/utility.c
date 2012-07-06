/* This file contains a collection of miscellaneous procedures:
 *   panic:    abort MINIX due to a fatal error
 *   kputc:          buffered putc used by kernel printf
 */

#include "kernel.h"
#include "proc.h"
#include "arch_proto.h"

#include <minix/syslib.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>

#include <minix/sys_config.h>

#define ARE_PANICING 0xDEADC0FF

/*===========================================================================*
 *			panic                                          *
 *===========================================================================*/
void panic(const char *fmt, ...)
{
  va_list arg;
  /* The system has run aground of a fatal kernel error. Terminate execution. */
  if (minix_panicing == ARE_PANICING) {
  	reset();
  }
  minix_panicing = ARE_PANICING;
  if (fmt != NULL) {
	printf("kernel panic: ");
  	va_start(arg, fmt);
	vprintf(fmt, arg);
	printf("\n");
  }

  printf("kernel on CPU %d: ", cpuid);
  util_stacktrace();

  printf("current process : ");
  proc_stacktrace(get_cpulocal_var(proc_ptr));

  /* Abort MINIX. */
  minix_shutdown(NULL);
}

/*===========================================================================*
 *				kputc				     	     *
 *===========================================================================*/
void kputc(c)
int c;					/* character to append */
{
/* Accumulate a single character for a kernel message. Send a notification
 * to the output driver if an END_OF_KMESS is encountered. 
 */
  if (c != END_OF_KMESS) {
      static int blpos = 0;
      int maxblpos = sizeof(kmess_buf) - 2;
#ifdef DEBUG_SERIAL
      if (do_serial_debug) {
	if(c == '\n')
      		ser_putc('\r');
      	ser_putc(c);
      }
#endif
      kmess.km_buf[kmess.km_next] = c;	/* put normal char in buffer */
      kmess_buf[blpos] = c;
      if (kmess.km_size < sizeof(kmess.km_buf))
          kmess.km_size += 1;		
      kmess.km_next = (kmess.km_next + 1) % _KMESS_BUF_SIZE;
      if(blpos == maxblpos) {
      	memmove(kmess_buf, kmess_buf+1, sizeof(kmess_buf)-1);
      } else blpos++;
  } else {
      int p;
      endpoint_t outprocs[] = OUTPUT_PROCS_ARRAY;
      if(!(minix_panicing || do_serial_debug)) {
	      for(p = 0; outprocs[p] != NONE; p++) {
		 if(isokprocn(outprocs[p]) && !isemptyn(outprocs[p])) {
       	    send_sig(outprocs[p], SIGKMESS);
		 }
      	}
     }
  }
  return;
}

void cpu_print_freq(unsigned cpu)
{
	u64_t freq;

	freq = cpu_get_freq(cpu);
	printf("CPU %d freq %lu MHz\n", cpu, (unsigned long)(freq
		/ 1000000));
}

int is_fpu(void)
{
	return get_cpulocal_var(fpu_presence);
}
