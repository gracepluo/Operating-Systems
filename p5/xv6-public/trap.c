// trap.c
#define MAX_PAGES_PER_MAPPING 4096 

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// Function to handle page faults
int handle_pagefault(uint addr) {
    struct proc *curproc = myproc();
    int handled = 0;

    cprintf("handle_pagefault: Handling page fault at address 0x%x for process %d\n", addr, curproc->pid);

    // Iterate through all mappings to find the one containing the faulting address
    for (int i = 0; i < curproc->num_mappings; i++) {
        struct mapping *m = &curproc->mappings[i];
        uint map_start = m->addr;
        uint map_end = map_start + m->length;

        if (addr >= map_start && addr < map_end) {
            cprintf("handle_pagefault: Address 0x%x is within mapping %d\n", addr, i);
            handled = 1;

            // Check if it's a file-backed mapping
            if (m->file != 0) {
                struct file *f = m->file;
                if (f == 0 || f->type != FD_INODE) {
                    cprintf("ERROR: handle_pagefault: Invalid file for mapping %d (fd=%d)\n", i + 1, m->fd);
                    goto segfault;
                }

                // **Load Data from File into Physical Page**

                // Calculate the page number and file offset
                int pg_num = (addr - m->addr) / PGSIZE;
                uint file_offset = pg_num * PGSIZE;

                // Allocate a physical page
                char *pa = kalloc();
                if (pa == 0) {
                    goto segfault;
                }

                // Convert kernel virtual address to physical address
                uint physical_addr = V2P(pa);

                // Read data from the file into the allocated memory
                int n = readi(f->ip, pa, file_offset, PGSIZE);

                if (n < 0) {
                    cprintf("ERROR: handle_pagefault: readi failed for mapping %d (fd=%d)\n", i + 1, m->fd);
                    kfree(pa);
                    goto segfault;
                }

                if (n < PGSIZE) {
                    memset(pa + n, 0, PGSIZE - n);
                }

                // **Retrieve the Page Table Entry (PTE)**
                pte_t *pte = walkpgdir(curproc->pgdir, (const void*)addr, 0);
                if (pte == 0) {
                    kfree(pa);
                    goto segfault;
                }

                // **Check if the Page is Already Present**
                if (*pte & PTE_P) {
                    kfree(pa);
                    goto segfault;
                }

                // **Map the Physical Page to the Virtual Address**
                *pte = physical_addr | PTE_U | PTE_W | PTE_P;

                // **Verify the Mapping**
                pte_t *verify_pte = walkpgdir(curproc->pgdir, (const void*)addr, 0);
                if (verify_pte == 0 || !(*verify_pte & PTE_P)) {
                    kfree(pa);
                    goto segfault;
                }

                // **Flush the TLB**
                lcr3(V2P(curproc->pgdir));

                // **Increment the Number of Loaded Pages with Overflow Check**
                if (m->n_loaded_pages < MAX_PAGES_PER_MAPPING) {
                    m->n_loaded_pages++;
                } else {
                    cprintf("ERROR: handle_pagefault: n_loaded_pages overflow for mapping %d\n", i + 1);
                    kfree(pa);
                    goto segfault;
                }

                // Successful handling
                return 1; // Success
            } else {
                // **Anonymous Mapping Handling**
                // Allocate a physical page
                char *pa = kalloc();
                if (pa == 0) {
                    goto segfault;
                }

                // Convert kernel virtual address to physical address
                uint physical_addr = V2P(pa);

                // Zero out the allocated memory
                memset(pa, 0, PGSIZE);

                // **Retrieve the Page Table Entry (PTE)**
                pte_t *pte = walkpgdir(curproc->pgdir, (const void*)addr, 0);
                if (pte == 0) {
                    kfree(pa);
                    goto segfault;
                }

                // **Check if the Page is Already Present**
                if (*pte & PTE_P) {
                    kfree(pa);
                    goto segfault;
                }

                // **Map the Physical Page to the Virtual Address**
                *pte = physical_addr | PTE_U | PTE_W | PTE_P;

                // **Verify the Mapping**
                pte_t *verify_pte = walkpgdir(curproc->pgdir, (const void*)addr, 0);
                if (verify_pte == 0 || !(*verify_pte & PTE_P)) {
                    kfree(pa);
                    goto segfault;
                }

                // **Flush the TLB**
                lcr3(V2P(curproc->pgdir));

                // **Increment the Number of Loaded Pages with Overflow Check**
                if (m->n_loaded_pages < MAX_PAGES_PER_MAPPING) {
                    m->n_loaded_pages++;
                } else {
                    cprintf("ERROR: handle_pagefault: n_loaded_pages overflow for mapping %d\n", i + 1);
                    kfree(pa);
                    goto segfault;
                }

                // Successful handling
                return 1; // Success
            }
        }
    }

    if (!handled) {
        // **Address Not Within Any Mapping**
        // No logging to minimize output
    }

segfault:
    // **Handle as a Standard Segmentation Fault**
    cprintf("Segmentation Fault: 0x%x\n", addr);
    curproc->killed = 1;
    return 0; // Failure
}

// Trap handler
void
trap(struct trapframe *tf)
{
        cprintf("trap: Entered trap handler for trap number %d\n", tf->trapno);
    if(tf->trapno == T_SYSCALL){
        if(myproc()->killed)
            exit();
        myproc()->tf = tf;
        syscall();
        if(myproc()->killed)
            exit();
        return;
    }

    switch(tf->trapno){
    case T_IRQ0 + IRQ_TIMER:
        if(cpuid() == 0){
            acquire(&tickslock);
            ticks++;
            wakeup(&ticks);
            release(&tickslock);
        }
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_IDE:
        ideintr();
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_KBD:
        kbdintr();
        lapiceoi();
        break;
    case T_IRQ0 + IRQ_COM1:
        uartintr();
        lapiceoi();
        break;
    case T_IRQ0 + 7:
    case T_IRQ0 + IRQ_SPURIOUS:
        cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
        lapiceoi();
        break;

case T_PGFLT:
{
    uint fault_addr = rcr2(); // Get the faulting address
    uint a = PGROUNDDOWN(fault_addr); // Round down to page boundary
    struct proc *curproc = myproc();

    cprintf("trap: Page fault in process %d at address 0x%x\n", curproc->pid, fault_addr);

    if (curproc == 0) {
        panic("Page fault with no current process");
    }

    pte_t *pte = walkpgdir(curproc->pgdir, (void*)a, 0);
    if (pte && (*pte & PTE_P)) {
        cprintf("trap: Page is present in page table\n");
        // Page is present in page table
        // Check if it's a write fault
        if ((tf->err & 0x2) != 0) { // Write fault
            cprintf("trap: Write fault on address 0x%x\n", fault_addr);
            // Check if COW bit is set
            if ((*pte & PTE_COW) != 0) {
                cprintf("trap: Handling Copy-On-Write for address 0x%x\n", fault_addr);
                // Handle Copy-On-Write
                uint pa = PTE_ADDR(*pte);
                // uint flags = PTE_FLAGS(*pte);
                char *mem = kalloc();
                if (mem == 0) {
                    cprintf("trap: Out of memory\n");
                    curproc->killed = 1;
                    break;
                }

                // Copy the contents from the old page to the new page
                memmove(mem, (char*)P2V(pa), PGSIZE);

                // Update PTE to point to new page, make it writable, and remove COW bit
                *pte = V2P(mem) | PTE_P | PTE_W | PTE_U;

                // Decrement reference count of old page
                decref(pa);

                // Invalidate the TLB for this page
                invlpg((void*)a);

                cprintf("trap: COW handled for address 0x%x, new page at 0x%x\n", fault_addr, V2P(mem));
            } else {
                // Not a COW page, segmentation fault
                cprintf("trap: Segmentation Fault: pid %d at address 0x%x (not COW)\n", curproc->pid, fault_addr);
                curproc->killed = 1;
            }
        } else {
            // Read fault on a present page, shouldn't happen
            cprintf("trap: Segmentation Fault: pid %d at address 0x%x (read fault)\n", curproc->pid, fault_addr);
            curproc->killed = 1;
        }
    } else {
        cprintf("trap: Page not present, checking mappings\n");
        // Page not present
        // Handle lazy allocation or segmentation fault
        // You can call handle_pagefault() if you have it implemented
        if (!handle_pagefault(fault_addr)) {
            cprintf("trap: Segmentation Fault: pid %d at address 0x%x (no mapping)\n", curproc->pid, fault_addr);
            curproc->killed = 1;
        }
    }
    break;
}


    default:
        if(myproc() == 0 || (tf->cs&3) == 0){
            // In kernel, unexpected trap: print and halt.
            cprintf("unexpected trap %d from cpu %d eip %x addr %x\n",
                tf->trapno, cpuid(), tf->eip, 0);
            panic("trap");
        }
        // In user space, assume process misbehaved.
        cprintf("pid %d %s: trap %d err %d on cpu %d eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpuid(), tf->eip, 0);
        myproc()->killed = 1;
    }

    // Force process exit if it has been killed and is in user space.
    // (If it is still executing in the kernel, let it keep running
    // until it gets to the regular system call return.)
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();

    // Force process to yield CPU on clock tick.
    if(myproc() && myproc()->state == RUNNING &&
        tf->trapno == T_IRQ0+IRQ_TIMER)
        yield();

    // Check if the process has been killed since we yielded
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();
}