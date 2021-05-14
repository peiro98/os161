#include <types.h>
#include <lib.h>
#include <vm.h>
#include <cpu.h>
#include <spl.h>
#include <spinlock.h>
#include <addrspace.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <kern/errno.h>

#define KVADDR_TO_PADDR(kvaddr) ((kvaddr) - MIPS_KSEG0)

static uint8_t *bitmap;
static unsigned int *allocations;

static unsigned int n_frames;
static char vm_initialized = 0;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

/* Initialization function */
void vm_bootstrap(void) {
    unsigned int i;
    static paddr_t firstpaddr;
    static paddr_t lastpaddr;

    // get the total physical size of the ram
    lastpaddr = ram_getsize();
    // compute the total number of frames
    n_frames = lastpaddr / 4096;

    // compute the size of the bitmap
    bitmap = (uint8_t *)kmalloc(n_frames * sizeof(uint8_t));
    allocations = (unsigned int *)kmalloc(n_frames * sizeof(unsigned int));

    // check how much memory is occupied
    firstpaddr = ram_getfirstfree();
    for (i = 0; i < (firstpaddr / 4096); i++)
    {
        bitmap[i] = 1;
        allocations[i] = 1;
    }

    DEBUG(DB_VM, "VM init: firstpaddr=0x%x, lastpaddr=0x%x\n", lastpaddr, firstpaddr);
    DEBUG(DB_VM, "VM init: creating a bitmap of size %d...\n", n_frames);

    vm_initialized = 1;

    (void)0;
}

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - 18 * PAGE_SIZE; //AAAAAAAAA
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}


static void dumbvm_can_sleep(void) {
    if (CURCPU_EXISTS())
    {
        /* must not hold spinlocks */
        KASSERT(curcpu->c_spinlocks == 0);

        /* must not be in an interrupt handler */
        KASSERT(curthread->t_in_interrupt == 0);
    }
}

static paddr_t getppages(unsigned long npages) {
    paddr_t addr;

    spinlock_acquire(&stealmem_lock);

    addr = ram_stealmem(npages);

    spinlock_release(&stealmem_lock);
    return addr;
}

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages) {
    unsigned i, k, size;
    paddr_t pa;

    if (!vm_initialized)
    {
        dumbvm_can_sleep();
        pa = getppages(npages);
        if (pa == 0)
        {
            return 0;
        }
        return PADDR_TO_KVADDR(pa);
    }

    // find the first suitable memory slot
    size = 0;

    for (i = 0; i < n_frames; i++)
    {
        size = bitmap[i] == 0 ? (size + 1) : 0;

        if (size == npages)
        {
            for (k = 0; k < size; k++)
            {
                bitmap[i + k] = 1;
            }

			allocations[i - size + 1] = npages;

            return PADDR_TO_KVADDR((i - size + 1) * PAGE_SIZE);
        }
    }

    return 0;
}

void free_kpages(vaddr_t addr) {
    paddr_t paddr;
    unsigned int idx_ff; // index of the first frame

    // compute the physical address of the allocation
    paddr = KVADDR_TO_PADDR(addr);

    // compute the frame number
    idx_ff = (unsigned int)paddr / PAGE_SIZE;
    KASSERT(idx_ff < n_frames);
    KASSERT(idx_ff + allocations[idx_ff] < n_frames);

    for (; allocations[idx_ff]; allocations[idx_ff]--) {
        bitmap[idx_ff + allocations[idx_ff] - 1] = 0;
    }

    KASSERT(allocations[idx_ff] == 0);
}

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *c) {
    (void)c;
}