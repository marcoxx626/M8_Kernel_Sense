/*
** I/O Sapic Driver - PCI interrupt line support
**
**      (c) Copyright 1999 Grant Grundler
**      (c) Copyright 1999 Hewlett-Packard Company
**
**      This program is free software; you can redistribute it and/or modify
**      it under the terms of the GNU General Public License as published by
**      the Free Software Foundation; either version 2 of the License, or
**      (at your option) any later version.
**
** The I/O sapic driver manages the Interrupt Redirection Table which is
** the control logic to convert PCI line based interrupts into a Message
** Signaled Interrupt (aka Transaction Based Interrupt, TBI).
**
** Acronyms
** --------
** HPA  Hard Physical Address (aka MMIO address)
** IRQ  Interrupt ReQuest. Implies Line based interrupt.
** IRT	Interrupt Routing Table (provided by PAT firmware)
** IRdT Interrupt Redirection Table. IRQ line to TXN ADDR/DATA
**      table which is implemented in I/O SAPIC.
** ISR  Interrupt Service Routine. aka Interrupt handler.
** MSI	Message Signaled Interrupt. PCI 2.2 functionality.
**      aka Transaction Based Interrupt (or TBI).
** PA   Precision Architecture. HP's RISC architecture.
** RISC Reduced Instruction Set Computer.
**
**
** What's a Message Signalled Interrupt?
** -------------------------------------
** MSI is a write transaction which targets a processor and is similar
** to a processor write to memory or MMIO. MSIs can be generated by I/O
** devices as well as processors and require *architecture* to work.
**
** PA only supports MSI. So I/O subsystems must either natively generate
** MSIs (e.g. GSC or HP-PB) or convert line based interrupts into MSIs
** (e.g. PCI and EISA).  IA64 supports MSIs via a "local SAPIC" which
** acts on behalf of a processor.
**
** MSI allows any I/O device to interrupt any processor. This makes
** load balancing of the interrupt processing possible on an SMP platform.
** Interrupts are also ordered WRT to DMA data.  It's possible on I/O
** coherent systems to completely eliminate PIO reads from the interrupt
** path. The device and driver must be designed and implemented to
** guarantee all DMA has been issued (issues about atomicity here)
** before the MSI is issued. I/O status can then safely be read from
** DMA'd data by the ISR.
**
**
** PA Firmware
** -----------
** PA-RISC platforms have two fundamentally different types of firmware.
** For PCI devices, "Legacy" PDC initializes the "INTERRUPT_LINE" register
** and BARs similar to a traditional PC BIOS.
** The newer "PAT" firmware supports PDC calls which return tables.
** PAT firmware only initializes the PCI Console and Boot interface.
** With these tables, the OS can program all other PCI devices.
**
** One such PAT PDC call returns the "Interrupt Routing Table" (IRT).
** The IRT maps each PCI slot's INTA-D "output" line to an I/O SAPIC
** input line.  If the IRT is not available, this driver assumes
** INTERRUPT_LINE register has been programmed by firmware. The latter
** case also means online addition of PCI cards can NOT be supported
** even if HW support is present.
**
** All platforms with PAT firmware to date (Oct 1999) use one Interrupt
** Routing Table for the entire platform.
**
** Where's the iosapic?
** --------------------
** I/O sapic is part of the "Core Electronics Complex". And on HP platforms
** it's integrated as part of the PCI bus adapter, "lba".  So no bus walk
** will discover I/O Sapic. I/O Sapic driver learns about each device
** when lba driver advertises the presence of the I/O sapic by calling
** iosapic_register().
**
**
** IRQ handling notes
** ------------------
** The IO-SAPIC can indicate to the CPU which interrupt was asserted.
** So, unlike the GSC-ASIC and Dino, we allocate one CPU interrupt per
** IO-SAPIC interrupt and call the device driver's handler directly.
** The IO-SAPIC driver hijacks the CPU interrupt handler so it can
** issue the End Of Interrupt command to the IO-SAPIC.
**
** Overview of exported iosapic functions
** --------------------------------------
** (caveat: code isn't finished yet - this is just the plan)
**
** iosapic_init:
**   o initialize globals (lock, etc)
**   o try to read IRT. Presence of IRT determines if this is
**     a PAT platform or not.
**
** iosapic_register():
**   o create iosapic_info instance data structure
**   o allocate vector_info array for this iosapic
**   o initialize vector_info - read corresponding IRdT?
**
** iosapic_xlate_pin: (only called by fixup_irq for PAT platform)
**   o intr_pin = read cfg (INTERRUPT_PIN);
**   o if (device under PCI-PCI bridge)
**               translate slot/pin
**
** iosapic_fixup_irq:
**   o if PAT platform (IRT present)
**	   intr_pin = iosapic_xlate_pin(isi,pcidev):
**         intr_line = find IRT entry(isi, PCI_SLOT(pcidev), intr_pin)
**         save IRT entry into vector_info later
**         write cfg INTERRUPT_LINE (with intr_line)?
**     else
**         intr_line = pcidev->irq
**         IRT pointer = NULL
**     endif
**   o locate vector_info (needs: isi, intr_line)
**   o allocate processor "irq" and get txn_addr/data
**   o request_irq(processor_irq,  iosapic_interrupt, vector_info,...)
**
** iosapic_enable_irq:
**   o clear any pending IRQ on that line
**   o enable IRdT - call enable_irq(vector[line]->processor_irq)
**   o write EOI in case line is already asserted.
**
** iosapic_disable_irq:
**   o disable IRdT - call disable_irq(vector[line]->processor_irq)
*/


#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>

#include <asm/byteorder.h>	
#include <asm/pdc.h>
#include <asm/pdcpat.h>
#include <asm/page.h>
#include <asm/io.h>		
#ifdef CONFIG_SUPERIO
#include <asm/superio.h>
#endif

#include <asm/ropes.h>
#include "./iosapic_private.h"

#define MODULE_NAME "iosapic"

#undef PCI_BRIDGE_FUNCS
#undef DEBUG_IOSAPIC
#undef DEBUG_IOSAPIC_IRT


#ifdef DEBUG_IOSAPIC
#define DBG(x...) printk(x)
#else 
#define DBG(x...)
#endif 

#ifdef DEBUG_IOSAPIC_IRT
#define DBG_IRT(x...) printk(x)
#else
#define DBG_IRT(x...)
#endif

#ifdef CONFIG_64BIT
#define COMPARE_IRTE_ADDR(irte, hpa)	((irte)->dest_iosapic_addr == (hpa))
#else
#define COMPARE_IRTE_ADDR(irte, hpa)	\
		((irte)->dest_iosapic_addr == ((hpa) | 0xffffffff00000000ULL))
#endif

#define IOSAPIC_REG_SELECT              0x00
#define IOSAPIC_REG_WINDOW              0x10
#define IOSAPIC_REG_EOI                 0x40

#define IOSAPIC_REG_VERSION		0x1

#define IOSAPIC_IRDT_ENTRY(idx)		(0x10+(idx)*2)
#define IOSAPIC_IRDT_ENTRY_HI(idx)	(0x11+(idx)*2)

static inline unsigned int iosapic_read(void __iomem *iosapic, unsigned int reg)
{
	writel(reg, iosapic + IOSAPIC_REG_SELECT);
	return readl(iosapic + IOSAPIC_REG_WINDOW);
}

static inline void iosapic_write(void __iomem *iosapic, unsigned int reg, u32 val)
{
	writel(reg, iosapic + IOSAPIC_REG_SELECT);
	writel(val, iosapic + IOSAPIC_REG_WINDOW);
}

#define IOSAPIC_VERSION_MASK	0x000000ff
#define	IOSAPIC_VERSION(ver)	((int) (ver & IOSAPIC_VERSION_MASK))

#define IOSAPIC_MAX_ENTRY_MASK          0x00ff0000
#define IOSAPIC_MAX_ENTRY_SHIFT         0x10
#define	IOSAPIC_IRDT_MAX_ENTRY(ver)	\
	(int) (((ver) & IOSAPIC_MAX_ENTRY_MASK) >> IOSAPIC_MAX_ENTRY_SHIFT)

#define IOSAPIC_IRDT_ENABLE       0x10000
#define IOSAPIC_IRDT_PO_LOW       0x02000
#define IOSAPIC_IRDT_LEVEL_TRIG   0x08000
#define IOSAPIC_IRDT_MODE_LPRI    0x00100

#define IOSAPIC_IRDT_ID_EID_SHIFT              0x10


static DEFINE_SPINLOCK(iosapic_lock);

static inline void iosapic_eoi(void __iomem *addr, unsigned int data)
{
	__raw_writel(data, addr);
}

static struct irt_entry *irt_cell;
static size_t irt_num_entry;

static struct irt_entry *iosapic_alloc_irt(int num_entries)
{
	unsigned long a;

	a = (unsigned long)kmalloc(sizeof(struct irt_entry) * num_entries + 8, GFP_KERNEL);
	a = (a + 7UL) & ~7UL;
	return (struct irt_entry *)a;
}



static int __init
iosapic_load_irt(unsigned long cell_num, struct irt_entry **irt)
{
	long status;              
	struct irt_entry *table;  
	unsigned long num_entries = 0UL;

	BUG_ON(!irt);

	if (is_pdc_pat()) {
		
		DBG("calling get_irt_size (cell %ld)\n", cell_num);
		status = pdc_pat_get_irt_size(&num_entries, cell_num);
		DBG("get_irt_size: %ld\n", status);

		BUG_ON(status != PDC_OK);
		BUG_ON(num_entries == 0);

		table = iosapic_alloc_irt(num_entries);
		if (table == NULL) {
			printk(KERN_WARNING MODULE_NAME ": read_irt : can "
					"not alloc mem for IRT\n");
			return 0;
		}

		
		status = pdc_pat_get_irt(table, cell_num);
		DBG("pdc_pat_get_irt: %ld\n", status);
		WARN_ON(status != PDC_OK);
	} else {
		if (irt_cell)
			return 0;

		
		status = pdc_pci_irt_size(&num_entries, 0);
		DBG("pdc_pci_irt_size: %ld\n", status);

		if (status != PDC_OK) {
			
			return 0;
		}

		BUG_ON(num_entries == 0);

		table = iosapic_alloc_irt(num_entries);
		if (!table) {
			printk(KERN_WARNING MODULE_NAME ": read_irt : can "
					"not alloc mem for IRT\n");
			return 0;
		}

		
		status = pdc_pci_irt(num_entries, 0, table);
		BUG_ON(status != PDC_OK);
	}

	
	*irt = table;

#ifdef DEBUG_IOSAPIC_IRT
{
	struct irt_entry *p = table;
	int i;

	printk(MODULE_NAME " Interrupt Routing Table (cell %ld)\n", cell_num);
	printk(MODULE_NAME " start = 0x%p num_entries %ld entry_size %d\n",
		table,
		num_entries,
		(int) sizeof(struct irt_entry));

	for (i = 0 ; i < num_entries ; i++, p++) {
		printk(MODULE_NAME " %02x %02x %02x %02x %02x %02x %02x %02x %08x%08x\n",
		p->entry_type, p->entry_length, p->interrupt_type,
		p->polarity_trigger, p->src_bus_irq_devno, p->src_bus_id,
		p->src_seg_id, p->dest_iosapic_intin,
		((u32 *) p)[2],
		((u32 *) p)[3]
		);
	}
}
#endif 

	return num_entries;
}



void __init iosapic_init(void)
{
	unsigned long cell = 0;

	DBG("iosapic_init()\n");

#ifdef __LP64__
	if (is_pdc_pat()) {
		int status;
		struct pdc_pat_cell_num cell_info;

		status = pdc_pat_cell_get_number(&cell_info);
		if (status == PDC_OK) {
			cell = cell_info.cell_num;
		}
	}
#endif

	
	irt_num_entry = iosapic_load_irt(cell, &irt_cell);
	if (irt_num_entry == 0)
		irt_cell = NULL;	
}


static struct irt_entry *
irt_find_irqline(struct iosapic_info *isi, u8 slot, u8 intr_pin)
{
	struct irt_entry *i = irt_cell;
	int cnt;	
	u8 irq_devno = (slot << IRT_DEV_SHIFT) | (intr_pin-1);

	DBG_IRT("irt_find_irqline() SLOT %d pin %d\n", slot, intr_pin);

	for (cnt=0; cnt < irt_num_entry; cnt++, i++) {

		if (i->entry_type != IRT_IOSAPIC_TYPE) {
			DBG_IRT(KERN_WARNING MODULE_NAME ":find_irqline(0x%p): skipping entry %d type %d\n", i, cnt, i->entry_type);
			continue;
		}
		
		if (i->entry_length != IRT_IOSAPIC_LENGTH) {
			DBG_IRT(KERN_WARNING MODULE_NAME ":find_irqline(0x%p): skipping entry %d  length %d\n", i, cnt, i->entry_length);
			continue;
		}

		if (i->interrupt_type != IRT_VECTORED_INTR) {
			DBG_IRT(KERN_WARNING MODULE_NAME ":find_irqline(0x%p): skipping entry  %d interrupt_type %d\n", i, cnt, i->interrupt_type);
			continue;
		}

		if (!COMPARE_IRTE_ADDR(i, isi->isi_hpa))
			continue;

		if ((i->src_bus_irq_devno & IRT_IRQ_DEVNO_MASK) != irq_devno)
			continue;


		
		return i;
	}

	printk(KERN_WARNING MODULE_NAME ": 0x%lx : no IRT entry for slot %d, pin %d\n",
			isi->isi_hpa, slot, intr_pin);
	return NULL;
}


static struct irt_entry *
iosapic_xlate_pin(struct iosapic_info *isi, struct pci_dev *pcidev)
{
	u8 intr_pin, intr_slot;

	pci_read_config_byte(pcidev, PCI_INTERRUPT_PIN, &intr_pin);

	DBG_IRT("iosapic_xlate_pin(%s) SLOT %d pin %d\n",
		pcidev->slot_name, PCI_SLOT(pcidev->devfn), intr_pin);

	if (intr_pin == 0) {
		
		return NULL;
	}

	
	if (pcidev->bus->parent) {
#ifdef PCI_BRIDGE_FUNCS
		if (pci_bridge_funcs->xlate_intr_line) {
			intr_pin = pci_bridge_funcs->xlate_intr_line(pcidev);
		}
#else	
		struct pci_bus *p = pcidev->bus;
		intr_pin = pci_swizzle_interrupt_pin(pcidev, intr_pin);
#endif 

		while (p->parent->parent)
			p = p->parent;

		intr_slot = PCI_SLOT(p->self->devfn);
	} else {
		intr_slot = PCI_SLOT(pcidev->devfn);
	}
	DBG_IRT("iosapic_xlate_pin:  bus %d slot %d pin %d\n",
				pcidev->bus->secondary, intr_slot, intr_pin);

	return irt_find_irqline(isi, intr_slot, intr_pin);
}

static void iosapic_rd_irt_entry(struct vector_info *vi , u32 *dp0, u32 *dp1)
{
	struct iosapic_info *isp = vi->iosapic;
	u8 idx = vi->irqline;

	*dp0 = iosapic_read(isp->addr, IOSAPIC_IRDT_ENTRY(idx));
	*dp1 = iosapic_read(isp->addr, IOSAPIC_IRDT_ENTRY_HI(idx));
}


static void iosapic_wr_irt_entry(struct vector_info *vi, u32 dp0, u32 dp1)
{
	struct iosapic_info *isp = vi->iosapic;

	DBG_IRT("iosapic_wr_irt_entry(): irq %d hpa %lx 0x%x 0x%x\n",
		vi->irqline, isp->isi_hpa, dp0, dp1);

	iosapic_write(isp->addr, IOSAPIC_IRDT_ENTRY(vi->irqline), dp0);

	
	dp0 = readl(isp->addr+IOSAPIC_REG_WINDOW);

	iosapic_write(isp->addr, IOSAPIC_IRDT_ENTRY_HI(vi->irqline), dp1);

	
	dp1 = readl(isp->addr+IOSAPIC_REG_WINDOW);
}

static void
iosapic_set_irt_data( struct vector_info *vi, u32 *dp0, u32 *dp1)
{
	u32 mode = 0;
	struct irt_entry *p = vi->irte;

	if ((p->polarity_trigger & IRT_PO_MASK) == IRT_ACTIVE_LO)
		mode |= IOSAPIC_IRDT_PO_LOW;

	if (((p->polarity_trigger >> IRT_EL_SHIFT) & IRT_EL_MASK) == IRT_LEVEL_TRIG)
		mode |= IOSAPIC_IRDT_LEVEL_TRIG;


	*dp0 = mode | (u32) vi->txn_data;

	if (is_pdc_pat()) {
		*dp1 = (u32) (vi->txn_addr);
	} else {
		*dp1 = (((u32)vi->txn_addr & 0x0ff00000) >> 4) |
			(((u32)vi->txn_addr & 0x000ff000) << 12);
	}
	DBG_IRT("iosapic_set_irt_data(): 0x%x 0x%x\n", *dp0, *dp1);
}


static void iosapic_mask_irq(struct irq_data *d)
{
	unsigned long flags;
	struct vector_info *vi = irq_data_get_irq_chip_data(d);
	u32 d0, d1;

	spin_lock_irqsave(&iosapic_lock, flags);
	iosapic_rd_irt_entry(vi, &d0, &d1);
	d0 |= IOSAPIC_IRDT_ENABLE;
	iosapic_wr_irt_entry(vi, d0, d1);
	spin_unlock_irqrestore(&iosapic_lock, flags);
}

static void iosapic_unmask_irq(struct irq_data *d)
{
	struct vector_info *vi = irq_data_get_irq_chip_data(d);
	u32 d0, d1;

	
	WARN_ON(vi->txn_irq  == 0);

	iosapic_set_irt_data(vi, &d0, &d1);
	iosapic_wr_irt_entry(vi, d0, d1);

#ifdef DEBUG_IOSAPIC_IRT
{
	u32 *t = (u32 *) ((ulong) vi->eoi_addr & ~0xffUL);
	printk("iosapic_enable_irq(): regs %p", vi->eoi_addr);
	for ( ; t < vi->eoi_addr; t++)
		printk(" %x", readl(t));
	printk("\n");
}

printk("iosapic_enable_irq(): sel ");
{
	struct iosapic_info *isp = vi->iosapic;

	for (d0=0x10; d0<0x1e; d0++) {
		d1 = iosapic_read(isp->addr, d0);
		printk(" %x", d1);
	}
}
printk("\n");
#endif

	DBG(KERN_DEBUG "enable_irq(%d): eoi(%p, 0x%x)\n", d->irq,
			vi->eoi_addr, vi->eoi_data);
	iosapic_eoi(vi->eoi_addr, vi->eoi_data);
}

static void iosapic_eoi_irq(struct irq_data *d)
{
	struct vector_info *vi = irq_data_get_irq_chip_data(d);

	iosapic_eoi(vi->eoi_addr, vi->eoi_data);
	cpu_eoi_irq(d);
}

#ifdef CONFIG_SMP
static int iosapic_set_affinity_irq(struct irq_data *d,
				    const struct cpumask *dest, bool force)
{
	struct vector_info *vi = irq_data_get_irq_chip_data(d);
	u32 d0, d1, dummy_d0;
	unsigned long flags;
	int dest_cpu;

	dest_cpu = cpu_check_affinity(d, dest);
	if (dest_cpu < 0)
		return -1;

	cpumask_copy(d->affinity, cpumask_of(dest_cpu));
	vi->txn_addr = txn_affinity_addr(d->irq, dest_cpu);

	spin_lock_irqsave(&iosapic_lock, flags);
	iosapic_rd_irt_entry(vi, &d0, &d1);
	iosapic_set_irt_data(vi, &dummy_d0, &d1);
	iosapic_wr_irt_entry(vi, d0, d1);
	spin_unlock_irqrestore(&iosapic_lock, flags);

	return 0;
}
#endif

static struct irq_chip iosapic_interrupt_type = {
	.name		=	"IO-SAPIC-level",
	.irq_unmask	=	iosapic_unmask_irq,
	.irq_mask	=	iosapic_mask_irq,
	.irq_ack	=	cpu_ack_irq,
	.irq_eoi	=	iosapic_eoi_irq,
#ifdef CONFIG_SMP
	.irq_set_affinity =	iosapic_set_affinity_irq,
#endif
};

int iosapic_fixup_irq(void *isi_obj, struct pci_dev *pcidev)
{
	struct iosapic_info *isi = isi_obj;
	struct irt_entry *irte = NULL;  
	struct vector_info *vi;
	int isi_line;	

	if (!isi) {
		printk(KERN_WARNING MODULE_NAME ": hpa not registered for %s\n",
			pci_name(pcidev));
		return -1;
	}

#ifdef CONFIG_SUPERIO
	if (is_superio_device(pcidev)) {
		
		pcidev->irq = superio_fixup_irq(pcidev);

		
		if (PCI_FUNC(pcidev->devfn) != SUPERIO_USB_FN)
			return pcidev->irq;
	}
#endif 

	
	irte = iosapic_xlate_pin(isi, pcidev);
	if (!irte) {
		printk("iosapic: no IRTE for %s (IRQ not connected?)\n",
				pci_name(pcidev));
		return -1;
	}
	DBG_IRT("iosapic_fixup_irq(): irte %p %x %x %x %x %x %x %x %x\n",
		irte,
		irte->entry_type,
		irte->entry_length,
		irte->polarity_trigger,
		irte->src_bus_irq_devno,
		irte->src_bus_id,
		irte->src_seg_id,
		irte->dest_iosapic_intin,
		(u32) irte->dest_iosapic_addr);
	isi_line = irte->dest_iosapic_intin;

	
	vi = isi->isi_vector + isi_line;
	DBG_IRT("iosapic_fixup_irq:  line %d vi 0x%p\n", isi_line, vi);

	
	if (vi->irte)
		goto out;

	vi->irte = irte;

	vi->txn_irq = txn_alloc_irq(8);

	if (vi->txn_irq < 0)
		panic("I/O sapic: couldn't get TXN IRQ\n");

	
	vi->txn_addr = txn_alloc_addr(vi->txn_irq);
	vi->txn_data = txn_alloc_data(vi->txn_irq);

	vi->eoi_addr = isi->addr + IOSAPIC_REG_EOI;
	vi->eoi_data = cpu_to_le32(vi->txn_data);

	cpu_claim_irq(vi->txn_irq, &iosapic_interrupt_type, vi);

 out:
	pcidev->irq = vi->txn_irq;

	DBG_IRT("iosapic_fixup_irq() %d:%d %x %x line %d irq %d\n",
		PCI_SLOT(pcidev->devfn), PCI_FUNC(pcidev->devfn),
		pcidev->vendor, pcidev->device, isi_line, pcidev->irq);

	return pcidev->irq;
}


static unsigned int
iosapic_rd_version(struct iosapic_info *isi)
{
	return iosapic_read(isi->addr, IOSAPIC_REG_VERSION);
}


void *iosapic_register(unsigned long hpa)
{
	struct iosapic_info *isi = NULL;
	struct irt_entry *irte = irt_cell;
	struct vector_info *vip;
	int cnt;	

	for (cnt=0; cnt < irt_num_entry; cnt++, irte++) {
		WARN_ON(IRT_IOSAPIC_TYPE != irte->entry_type);
		if (COMPARE_IRTE_ADDR(irte, hpa))
			break;
	}

	if (cnt >= irt_num_entry) {
		DBG("iosapic_register() ignoring 0x%lx (NOT FOUND)\n", hpa);
		return NULL;
	}

	isi = kzalloc(sizeof(struct iosapic_info), GFP_KERNEL);
	if (!isi) {
		BUG();
		return NULL;
	}

	isi->addr = ioremap_nocache(hpa, 4096);
	isi->isi_hpa = hpa;
	isi->isi_version = iosapic_rd_version(isi);
	isi->isi_num_vectors = IOSAPIC_IRDT_MAX_ENTRY(isi->isi_version) + 1;

	vip = isi->isi_vector = kcalloc(isi->isi_num_vectors,
					sizeof(struct vector_info), GFP_KERNEL);
	if (vip == NULL) {
		kfree(isi);
		return NULL;
	}

	for (cnt=0; cnt < isi->isi_num_vectors; cnt++, vip++) {
		vip->irqline = (unsigned char) cnt;
		vip->iosapic = isi;
	}
	return isi;
}


#ifdef DEBUG_IOSAPIC

static void
iosapic_prt_irt(void *irt, long num_entry)
{
	unsigned int i, *irp = (unsigned int *) irt;


	printk(KERN_DEBUG MODULE_NAME ": Interrupt Routing Table (%lx entries)\n", num_entry);

	for (i=0; i<num_entry; i++, irp += 4) {
		printk(KERN_DEBUG "%p : %2d %.8x %.8x %.8x %.8x\n",
					irp, i, irp[0], irp[1], irp[2], irp[3]);
	}
}


static void
iosapic_prt_vi(struct vector_info *vi)
{
	printk(KERN_DEBUG MODULE_NAME ": vector_info[%d] is at %p\n", vi->irqline, vi);
	printk(KERN_DEBUG "\t\tstatus:	 %.4x\n", vi->status);
	printk(KERN_DEBUG "\t\ttxn_irq:  %d\n",  vi->txn_irq);
	printk(KERN_DEBUG "\t\ttxn_addr: %lx\n", vi->txn_addr);
	printk(KERN_DEBUG "\t\ttxn_data: %lx\n", vi->txn_data);
	printk(KERN_DEBUG "\t\teoi_addr: %p\n",  vi->eoi_addr);
	printk(KERN_DEBUG "\t\teoi_data: %x\n",  vi->eoi_data);
}


static void
iosapic_prt_isi(struct iosapic_info *isi)
{
	printk(KERN_DEBUG MODULE_NAME ": io_sapic_info at %p\n", isi);
	printk(KERN_DEBUG "\t\tisi_hpa:       %lx\n", isi->isi_hpa);
	printk(KERN_DEBUG "\t\tisi_status:    %x\n", isi->isi_status);
	printk(KERN_DEBUG "\t\tisi_version:   %x\n", isi->isi_version);
	printk(KERN_DEBUG "\t\tisi_vector:    %p\n", isi->isi_vector);
}
#endif 
