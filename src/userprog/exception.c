#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/syscall.h"
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
static bool stack_access (void *, void *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      sys_exit (-1); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      sys_exit (-1);
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */
  void *fault_page;  /* Fault page. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));
  fault_page = pg_round_down (fault_addr);

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

#ifdef VM
  struct thread *cur = thread_current ();
  struct page *p;
  void *esp;

  /* We need to obtain the current value of the user program's
     stack pointer.

     If a page fault occurs in the user program, we can retrieve it
     from the ESP member of the intr_frame F.  On the other hand,
     we cannot retrieve it from F if a page fault occured in the
     kernel, because the processor only saves the stack pointer when
     an exception causes a "switch" from user to kernel mode.
     So we've saved ESP into struct thread on the initial transition
     from user to kernel mode. */
  esp = user ? f->esp : cur->saved_esp;

  /* Stack growth.
     Control flow reaches the below code segment which supports
     lazy loading. */
  if (stack_access (fault_addr, esp))
    {
      p = page_make_entry (fault_page);
      if (p != NULL)
        {
          /* Add supplemental information. */
          p->type = PG_ZERO;
          p->writable = true;
        }
    }

  /* Because executable code and data segments are not immediately
     loaded in memory during process setup, a not-present page fault
     occurs when a process accesses unloaded segments later.
     In this case, the page fault handler should load the user virtual
     pages and resume the process's execution.

     In order for the handler to know how to load the fault page, each
     process has already created SPTEs.
     See load_segment() defined in userprog/process.c.
     
     Similarly, stack growth is considered as lazy loading. */
  if (not_present)
    {
      if (!page_load (fault_page))
        sys_exit (-1);
      return;
    }
#endif

  /* A page fault in the kernel merely sets EAX to 0xffffffff and
     copies its former value into EIP. This enables returning a -1
     error code from an invalid memory access, which is required
     by `get_user' and `copy_from_user' defined in "userprog/syscall.c". */
  if (!user)
    {
      /* When a page fault occurs in a function F, EAX holds the
         address where F is called. EIP points the next instruction
         to execute. */
      f->eip = (void (*) (void)) f->eax;
      /* EAX is a return value, -1. */
      f->eax = (uint32_t) SYS_BAD_ADDR;
      return;
    }

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
  printf ("Page fault at %p: %s error %s page in %s context.\n",
          fault_addr,
          not_present ? "not present" : "rights violation",
          write ? "writing" : "reading",
          user ? "user" : "kernel");
  kill (f);
}

/* Absolute limit on stack growth size, 8 MB. */
#define STACK_MAX_SIZE (8 * 1024 * 1024)

/* Checks whether the stack growth is needed or not.

   Additional stack pages must be allocated only if they "appear" to be
   stack accesses.
   
   Notice that the 80x86 PUSH instruction checks access permissions before
   it adjusts the stack pointer, so it may cause a page fault 4 bytes
   below the stack pointer.  Similarly, the PUSHA instruction pushes 32 bytes
   at once, so it can fault 32 bytes below the stack pointer. */
static bool
stack_access (void *vaddr, void *esp)
{
  return vaddr > (PHYS_BASE - STACK_MAX_SIZE)
         && vaddr <= (PHYS_BASE - 1)
         && vaddr >= (esp - 32);
}