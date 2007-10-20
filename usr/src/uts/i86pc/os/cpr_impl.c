/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Platform specific implementation code
 * Currently only suspend to RAM is supported (ACPI S3)
 */

#define	SUNDDI_IMPL

#include <sys/types.h>
#include <sys/promif.h>
#include <sys/prom_isa.h>
#include <sys/prom_plat.h>
#include <sys/cpuvar.h>
#include <sys/pte.h>
#include <vm/hat.h>
#include <vm/page.h>
#include <vm/as.h>
#include <sys/cpr.h>
#include <sys/kmem.h>
#include <sys/clock.h>
#include <sys/kmem.h>
#include <sys/panic.h>
#include <vm/seg_kmem.h>
#include <sys/cpu_module.h>
#include <sys/callb.h>
#include <sys/machsystm.h>
#include <sys/vmsystm.h>
#include <sys/systm.h>
#include <sys/archsystm.h>
#include <sys/stack.h>
#include <sys/fs/ufs_fs.h>
#include <sys/memlist.h>
#include <sys/bootconf.h>
#include <sys/thread.h>
#include <sys/x_call.h>
#include <sys/smp_impldefs.h>
#include <vm/vm_dep.h>
#include <sys/psm.h>
#include <sys/epm.h>
#include <sys/cpr_wakecode.h>
#include <sys/x86_archext.h>
#include <sys/reboot.h>
#include <sys/acpi/acpi.h>
#include <sys/acpica.h>

#define	AFMT	"%lx"

extern int	flushes_require_xcalls;
extern cpuset_t	cpu_ready_set;

#if defined(__amd64)
extern void	*wc_long_mode_64(void);
#endif	/* __amd64 */
extern int	tsc_gethrtime_enable;
extern	void	i_cpr_start_cpu(void);

ushort_t	cpr_mach_type = CPR_MACHTYPE_X86;
void		(*cpr_start_cpu_func)(void) = i_cpr_start_cpu;

static wc_cpu_t	*wc_other_cpus = NULL;
static cpuset_t procset = 1;

static void
init_real_mode_platter(int cpun, uint32_t offset, uint_t cr4, wc_desctbr_t gdt);

static int i_cpr_platform_alloc(psm_state_request_t *req);
static void i_cpr_platform_free(psm_state_request_t *req);
static int i_cpr_save_apic(psm_state_request_t *req);
static int i_cpr_restore_apic(psm_state_request_t *req);

#if defined(__amd64)
static void restore_stack(wc_cpu_t *cpup);
static void save_stack(wc_cpu_t *cpup);
void (*save_stack_func)(wc_cpu_t *) = save_stack;
#endif	/* __amd64 */

/*
 * restart paused slave cpus
 */
void
i_cpr_machdep_setup(void)
{
	if (ncpus > 1) {
		CPR_DEBUG(CPR_DEBUG1, ("MP restarted...\n"));
		mutex_enter(&cpu_lock);
		start_cpus();
		mutex_exit(&cpu_lock);
	}
}


/*
 * Stop all interrupt activities in the system
 */
void
i_cpr_stop_intr(void)
{
	(void) spl7();
}

/*
 * Set machine up to take interrupts
 */
void
i_cpr_enable_intr(void)
{
	(void) spl0();
}

/*
 * Save miscellaneous information which needs to be written to the
 * state file.  This information is required to re-initialize
 * kernel/prom handshaking.
 */
void
i_cpr_save_machdep_info(void)
{
	int notcalled = 0;
	ASSERT(notcalled);
}


void
i_cpr_set_tbr(void)
{
}


processorid_t
i_cpr_bootcpuid(void)
{
	return (0);
}

/*
 * cpu0 should contain bootcpu info
 */
cpu_t *
i_cpr_bootcpu(void)
{
	ASSERT(MUTEX_HELD(&cpu_lock));

	return (cpu_get(i_cpr_bootcpuid()));
}

/*
 *	Save context for the specified CPU
 */
void *
i_cpr_save_context(void *arg)
{
	long	index = (long)arg;
	psm_state_request_t *papic_state;
	int resuming;
	int	ret;

	PMD(PMD_SX, ("i_cpr_save_context() index = %ld\n", index))

	ASSERT(index < NCPU);

	papic_state = &(wc_other_cpus + index)->wc_apic_state;

	ret = i_cpr_platform_alloc(papic_state);
	ASSERT(ret == 0);

	ret = i_cpr_save_apic(papic_state);
	ASSERT(ret == 0);

	/*
	 * wc_save_context returns twice, once when susending and
	 * once when resuming,  wc_save_context() returns 0 when
	 * suspending and non-zero upon resume
	 */
	resuming = (wc_save_context(wc_other_cpus + index) == 0);

	PMD(PMD_SX, ("i_cpr_save_context: wc_save_context returns %d\n",
	    resuming))

	/*
	 * do NOT call any functions after this point, because doing so
	 * will modify the stack that we are running on
	 */

	if (resuming) {

		ret = i_cpr_restore_apic(papic_state);
		ASSERT(ret == 0);

		i_cpr_platform_free(papic_state);

		/*
		 * Setting the bit in cpu_ready_set must be the last operation
		 * in processor initialization; the boot CPU will continue to
		 * boot once it sees this bit set for all active CPUs.
		 */
		CPUSET_ATOMIC_ADD(cpu_ready_set, CPU->cpu_id);

		PMD(PMD_SX,
		    ("cpu_release() cpu_ready_set = %lx, CPU->cpu_id = %d\n",
		    cpu_ready_set, CPU->cpu_id))
	}
	return (NULL);
}

static ushort_t *warm_reset_vector = NULL;

static ushort_t *
map_warm_reset_vector()
{
	/*LINTED*/
	if (!(warm_reset_vector = (ushort_t *)psm_map_phys(WARM_RESET_VECTOR,
	    sizeof (ushort_t *), PROT_READ|PROT_WRITE)))
		return (NULL);

	/*
	 * setup secondary cpu bios boot up vector
	 */
	*warm_reset_vector = (ushort_t)((caddr_t)
	    /*LINTED*/
	    ((struct rm_platter *)rm_platter_va)->rm_code - rm_platter_va
	    + ((ulong_t)rm_platter_va & 0xf));
	warm_reset_vector++;
	*warm_reset_vector = (ushort_t)(rm_platter_pa >> 4);

	--warm_reset_vector;
	return (warm_reset_vector);
}

void
i_cpr_pre_resume_cpus()
{
	/*
	 * this is a cut down version of start_other_cpus()
	 * just do the initialization to wake the other cpus
	 */
	unsigned who;
	int cpuid = i_cpr_bootcpuid();
	int started_cpu;
	uint32_t		code_length = 0;
	caddr_t			wakevirt = rm_platter_va;
	/*LINTED*/
	wakecode_t		*wp = (wakecode_t *)wakevirt;
	char *str = "i_cpr_pre_resume_cpus";
	extern int get_tsc_ready();
	int err;

	/*LINTED*/
	rm_platter_t *real_mode_platter = (rm_platter_t *)rm_platter_va;

	/*
	 * Copy the real mode code at "real_mode_start" to the
	 * page at rm_platter_va.
	 */
	warm_reset_vector = map_warm_reset_vector();
	if (warm_reset_vector == NULL) {
		PMD(PMD_SX, ("i_cpr_pre_resume_cpus() returning #2\n"))
		return;
	}

	flushes_require_xcalls = 1;

	/*
	 * We lock our affinity to the master CPU to ensure that all slave CPUs
	 * do their TSC syncs with the same CPU.
	 */

	affinity_set(CPU_CURRENT);

	cpu_ready_set = 0;

	for (who = 0; who < ncpus; who++) {

		wc_cpu_t	*cpup = wc_other_cpus + who;
		wc_desctbr_t	gdt;

		if (who == cpuid)
			continue;

		if (!CPU_IN_SET(mp_cpus, who))
			continue;

		PMD(PMD_SX, ("%s() waking up %d cpu\n", str, who))

		bcopy(cpup, &(wp->wc_cpu), sizeof (wc_cpu_t));

		gdt.base = cpup->wc_gdt_base;
		gdt.limit = cpup->wc_gdt_limit;

#if defined(__amd64)
		code_length = (uint32_t)wc_long_mode_64 - (uint32_t)wc_rm_start;
#else
		code_length = 0;
#endif

		init_real_mode_platter(who, code_length, cpup->wc_cr4, gdt);

		started_cpu = 1;

		if ((err = mach_cpuid_start(who, rm_platter_va)) != 0) {
			cmn_err(CE_WARN, "cpu%d: failed to start during "
			    "suspend/resume error %d", who, err);
			continue;
		}

		PMD(PMD_SX, ("%s() #1 waiting for procset 0x%lx\n", str,
		    (ulong_t)procset))

/*
 * This conditional compile only affects the MP case.
 */
#ifdef	MP_PM
		for (delays = 0; !CPU_IN_SET(procset, who); delays++) {
			if (delays == 500) {
				/*
				 * After five seconds, things are probably
				 * looking a bit bleak - explain the hang.
				 */
				cmn_err(CE_NOTE, "cpu%d: started, "
				    "but not running in the kernel yet", who);
				PMD(PMD_SX, ("%s() %d cpu started "
				    "but not running in the kernel yet\n",
				    str, who))
			} else if (delays > 2000) {
				/*
				 * We waited at least 20 seconds, bail ..
				 */
				cmn_err(CE_WARN, "cpu%d: timed out", who);
				PMD(PMD_SX, ("%s() %d cpu timed out\n",
				    str, who))
				started_cpu = 0;
			}

			/*
			 * wait at least 10ms, then check again..
			 */
			delay(USEC_TO_TICK_ROUNDUP(10000));
		}
#else
		while (!CPU_IN_SET(procset, who)) {
			;
		}

#endif	/*	MP_PM	*/

		PMD(PMD_SX, ("%s() %d cpu started\n", str, who))

		if (!started_cpu)
			continue;

		PMD(PMD_SX, ("%s() tsc_ready = %d\n", str,
		    get_tsc_ready()))

		if (tsc_gethrtime_enable) {
			PMD(PMD_SX, ("%s() calling tsc_sync_master\n", str))
			tsc_sync_master(who);
		}


		PMD(PMD_SX, ("%s() waiting for cpu_ready_set %ld\n", str,
		    cpu_ready_set))
		/*
		 * Wait for cpu to declare that it is ready, we want the
		 * cpus to start serially instead of in parallel, so that
		 * they do not contend with each other in wc_rm_start()
		 */
		while (!CPU_IN_SET(cpu_ready_set, who)) {
			PMD(PMD_SX, ("%s() waiting for "
			    "cpu_ready_set %ld\n", str, cpu_ready_set))
			;
		}

		/*
		 * do not need to re-initialize dtrace using dtrace_cpu_init
		 * function
		 */
		PMD(PMD_SX, ("%s() cpu %d now ready\n", str, who))
	}

	affinity_clear();

	PMD(PMD_SX, ("%s() all cpus now ready\n", str))
}

static void
unmap_warm_reset_vector(ushort_t *warm_reset_vector)
{
	psm_unmap_phys((caddr_t)warm_reset_vector, sizeof (ushort_t *));
}

/*
 * We need to setup a 1:1 (virtual to physical) mapping for the
 * page containing the wakeup code.
 */
static struct as *save_as;	/* when switching to kas */

static void
unmap_wakeaddr_1to1(uint64_t wakephys)
{
	uintptr_t	wp = (uintptr_t)wakephys;
	hat_setup(save_as->a_hat, 0);	/* switch back from kernel hat */
	hat_unload(kas.a_hat, (caddr_t)wp, PAGESIZE, HAT_UNLOAD);
}

void
i_cpr_post_resume_cpus()
{
	uint64_t	wakephys = rm_platter_pa;

	if (warm_reset_vector != NULL)
		unmap_warm_reset_vector(warm_reset_vector);

	hat_unload(kas.a_hat, (caddr_t)(uintptr_t)rm_platter_pa, MMU_PAGESIZE,
	    HAT_UNLOAD);

	/*
	 * cmi_post_mpstartup() is only required upon boot not upon
	 * resume from RAM
	 */

	PT(PT_UNDO1to1);
	/* Tear down 1:1 mapping for wakeup code */
	unmap_wakeaddr_1to1(wakephys);
}

/* ARGSUSED */
void
i_cpr_handle_xc(int flag)
{
}

int
i_cpr_reusable_supported(void)
{
	return (0);
}
static void
map_wakeaddr_1to1(uint64_t wakephys)
{
	uintptr_t	wp = (uintptr_t)wakephys;
	hat_devload(kas.a_hat, (caddr_t)wp, PAGESIZE, btop(wakephys),
	    (PROT_READ|PROT_WRITE|PROT_EXEC|HAT_STORECACHING_OK|HAT_NOSYNC),
	    HAT_LOAD);
	save_as = curthread->t_procp->p_as;
	hat_setup(kas.a_hat, 0);	/* switch to kernel-only hat */
}


void
prt_other_cpus()
{
	int	who;

	if (ncpus == 1) {
		PMD(PMD_SX, ("prt_other_cpus() other cpu table empty for "
		    "uniprocessor machine\n"))
		return;
	}

	for (who = 0; who < ncpus; who++) {

		wc_cpu_t	*cpup = wc_other_cpus + who;

		PMD(PMD_SX, ("prt_other_cpus() who = %d, gdt=%p:%x, "
		    "idt=%p:%x, ldt=%lx, tr=%lx, kgsbase="
		    AFMT ", sp=%lx\n", who,
		    (void *)cpup->wc_gdt_base, cpup->wc_gdt_limit,
		    (void *)cpup->wc_idt_base, cpup->wc_idt_limit,
		    (long)cpup->wc_ldt, (long)cpup->wc_tr,
		    (long)cpup->wc_kgsbase, (long)cpup->wc_rsp))
	}
}

/*
 * Power down the system.
 */
int
i_cpr_power_down(int sleeptype)
{
	caddr_t		wakevirt = rm_platter_va;
	uint64_t	wakephys = rm_platter_pa;
	uint_t		saved_intr;
	uint32_t	code_length = 0;
	wc_desctbr_t	gdt;
	/*LINTED*/
	wakecode_t	*wp = (wakecode_t *)wakevirt;
	/*LINTED*/
	rm_platter_t	*wcpp = (rm_platter_t *)wakevirt;
	wc_cpu_t	*cpup = &(wp->wc_cpu);
	dev_info_t	*ppm;
	int		ret = 0;
	power_req_t	power_req;
	char *str =	"i_cpr_power_down";
#if defined(__amd64)
	/*LINTED*/
	rm_platter_t *real_mode_platter = (rm_platter_t *)rm_platter_va;
#endif
	extern int	cpr_suspend_succeeded;
	extern void	kernel_wc_code();
	extern ulong_t	intr_clear(void);
	extern void	intr_restore(ulong_t);

	ASSERT(sleeptype == CPR_TORAM);
	ASSERT(CPU->cpu_id == 0);

	if ((ppm = PPM(ddi_root_node())) == NULL) {
		PMD(PMD_SX, ("%s: root node not claimed\n", str))
		return (ENOTTY);
	}

	PMD(PMD_SX, ("Entering %s()\n", str))

	PT(PT_IC);
	saved_intr = intr_clear();

	PT(PT_1to1);
	/* Setup 1:1 mapping for wakeup code */
	map_wakeaddr_1to1(wakephys);

	PMD(PMD_SX, ("ncpus=%d\n", ncpus))

	PMD(PMD_SX, ("wc_rm_end - wc_rm_start=%lx WC_CODESIZE=%x\n",
	    ((size_t)((uint_t)wc_rm_end - (uint_t)wc_rm_start)), WC_CODESIZE))

	PMD(PMD_SX, ("wakevirt=%p, wakephys=%x\n",
	    (void *)wakevirt, (uint_t)wakephys))

	ASSERT(((size_t)((uint_t)wc_rm_end - (uint_t)wc_rm_start)) <
	    WC_CODESIZE);

	bzero(wakevirt, PAGESIZE);

	/* Copy code to rm_platter */
	bcopy((caddr_t)wc_rm_start, wakevirt,
	    (size_t)((uint_t)wc_rm_end - (uint_t)wc_rm_start));

	prt_other_cpus();

#if defined(__amd64)

	PMD(PMD_SX, ("real_mode_platter->rm_cr4=%lx, getcr4()=%lx\n",
	    (ulong_t)real_mode_platter->rm_cr4, (ulong_t)getcr4()))
	PMD(PMD_SX, ("real_mode_platter->rm_pdbr=%lx, getcr3()=%lx\n",
	    (ulong_t)real_mode_platter->rm_pdbr, getcr3()))

	real_mode_platter->rm_cr4 = getcr4();
	real_mode_platter->rm_pdbr = getcr3();

	rmp_gdt_init(real_mode_platter);

	/*
	 * Since the CPU needs to jump to protected mode using an identity
	 * mapped address, we need to calculate it here.
	 */
	real_mode_platter->rm_longmode64_addr = rm_platter_pa +
	    ((uint32_t)wc_long_mode_64 - (uint32_t)wc_rm_start);

	PMD(PMD_SX, ("real_mode_platter->rm_cr4=%lx, getcr4()=%lx\n",
	    (ulong_t)real_mode_platter->rm_cr4, getcr4()))

	PMD(PMD_SX, ("real_mode_platter->rm_pdbr=%lx, getcr3()=%lx\n",
	    (ulong_t)real_mode_platter->rm_pdbr, getcr3()))

	PMD(PMD_SX, ("real_mode_platter->rm_longmode64_addr=%lx\n",
	    (ulong_t)real_mode_platter->rm_longmode64_addr))

#endif

	PMD(PMD_SX, ("mp_cpus=%lx\n", (ulong_t)mp_cpus))

	PT(PT_SC);
	if (wc_save_context(cpup)) {

		ret = i_cpr_platform_alloc(&(wc_other_cpus->wc_apic_state));
		if (ret != 0)
			return (ret);

		ret = i_cpr_save_apic(&(wc_other_cpus->wc_apic_state));
		PMD(PMD_SX, ("%s: i_cpr_save_apic() returned %d\n", str, ret))
		if (ret != 0)
			return (ret);

		PMD(PMD_SX, ("wakephys=%x, kernel_wc_code=%p\n",
		    (uint_t)wakephys, (void *)&kernel_wc_code))
		PMD(PMD_SX, ("virtaddr=%lx, retaddr=%lx\n",
		    (long)cpup->wc_virtaddr, (long)cpup->wc_retaddr))
		PMD(PMD_SX, ("ebx=%x, edi=%x, esi=%x, ebp=%x, esp=%x\n",
		    cpup->wc_ebx, cpup->wc_edi, cpup->wc_esi, cpup->wc_ebp,
		    cpup->wc_esp))
		PMD(PMD_SX, ("cr0=%lx, cr3=%lx, cr4=%lx\n",
		    (long)cpup->wc_cr0, (long)cpup->wc_cr3,
		    (long)cpup->wc_cr4))
		PMD(PMD_SX, ("cs=%x, ds=%x, es=%x, ss=%x, fs=%lx, gs=%lx, "
		    "flgs=%lx\n", cpup->wc_cs, cpup->wc_ds, cpup->wc_es,
		    cpup->wc_ss, (long)cpup->wc_fs, (long)cpup->wc_gs,
		    (long)cpup->wc_eflags))

		PMD(PMD_SX, ("gdt=%p:%x, idt=%p:%x, ldt=%lx, tr=%lx, "
		    "kgbase=%lx\n", (void *)cpup->wc_gdt_base,
		    cpup->wc_gdt_limit, (void *)cpup->wc_idt_base,
		    cpup->wc_idt_limit, (long)cpup->wc_ldt,
		    (long)cpup->wc_tr, (long)cpup->wc_kgsbase))

		gdt.base = cpup->wc_gdt_base;
		gdt.limit = cpup->wc_gdt_limit;

#if defined(__amd64)
		code_length = (uint32_t)wc_long_mode_64 -
		    (uint32_t)wc_rm_start;
#else
		code_length = 0;
#endif

		init_real_mode_platter(0, code_length, cpup->wc_cr4, gdt);

#if defined(__amd64)
		PMD(PMD_SX, ("real_mode_platter->rm_cr4=%lx, getcr4()=%lx\n",
		    (ulong_t)wcpp->rm_cr4, getcr4()))

		PMD(PMD_SX, ("real_mode_platter->rm_pdbr=%lx, getcr3()=%lx\n",
		    (ulong_t)wcpp->rm_pdbr, getcr3()))

		PMD(PMD_SX, ("real_mode_platter->rm_longmode64_addr=%lx\n",
		    (ulong_t)wcpp->rm_longmode64_addr))

		PMD(PMD_SX,
		    ("real_mode_platter->rm_temp_gdt[TEMPGDT_KCODE64]=%lx\n",
		    (ulong_t)wcpp->rm_temp_gdt[TEMPGDT_KCODE64]))
#endif

		PMD(PMD_SX, ("gdt=%p:%x, idt=%p:%x, ldt=%lx, tr=%lx, "
		    "kgsbase=%lx\n", (void *)wcpp->rm_gdt_base,
		    wcpp->rm_gdt_lim, (void *)wcpp->rm_idt_base,
		    wcpp->rm_idt_lim, (long)cpup->wc_ldt, (long)cpup->wc_tr,
		    (long)cpup->wc_kgsbase))

		power_req.request_type = PMR_PPM_ENTER_SX;
		power_req.req.ppm_power_enter_sx_req.sx_state = S3;
		power_req.req.ppm_power_enter_sx_req.test_point =
		    cpr_test_point;
		power_req.req.ppm_power_enter_sx_req.wakephys = wakephys;

		PMD(PMD_SX, ("%s: pm_ctlops PMR_PPM_ENTER_SX\n", str))
		PT(PT_PPMCTLOP);
		(void) pm_ctlops(ppm, ddi_root_node(), DDI_CTLOPS_POWER,
		    &power_req, &ret);
		PMD(PMD_SX, ("%s: returns %d\n", str, ret))

		/*
		 * If it works, we get control back to the else branch below
		 * If we get control back here, it didn't work.
		 * XXX return EINVAL here?
		 */

		unmap_wakeaddr_1to1(wakephys);
		intr_restore(saved_intr);

		return (ret);
	} else {
		cpr_suspend_succeeded = 1;

		power_req.request_type = PMR_PPM_EXIT_SX;
		power_req.req.ppm_power_enter_sx_req.sx_state = S3;

		PMD(PMD_SX, ("%s: pm_ctlops PMR_PPM_EXIT_SX\n", str))
		PT(PT_PPMCTLOP);
		(void) pm_ctlops(ppm, ddi_root_node(), DDI_CTLOPS_POWER,
		    &power_req, &ret);
		PMD(PMD_SX, ("%s: returns %d\n", str, ret))

		ret = i_cpr_restore_apic(&(wc_other_cpus->wc_apic_state));
		/*
		 * the restore should never fail, if the saved suceeded
		 */
		ASSERT(ret == 0);

		i_cpr_platform_free(&(wc_other_cpus->wc_apic_state));

		PT(PT_INTRRESTORE);
		intr_restore(saved_intr);
		PT(PT_CPU);

		return (ret);
	}
}

/*
 * Stop all other cpu's before halting or rebooting. We pause the cpu's
 * instead of sending a cross call.
 * Stolen from sun4/os/mp_states.c
 */

static int cpu_are_paused;	/* sic */

void
i_cpr_stop_other_cpus(void)
{
	mutex_enter(&cpu_lock);
	if (cpu_are_paused) {
		mutex_exit(&cpu_lock);
		return;
	}
	pause_cpus(NULL);
	cpu_are_paused = 1;

	mutex_exit(&cpu_lock);
}

int
i_cpr_is_supported(int sleeptype)
{
	extern int cpr_supported_override;
	extern int cpr_platform_enable;
	extern int pm_S3_enabled;

	if (sleeptype != CPR_TORAM)
		return (0);

	/*
	 * The next statement tests if a specific platform has turned off
	 * cpr support.
	 */
	if (cpr_supported_override)
		return (0);

	/*
	 * If a platform has specifically turned on cpr support ...
	 */
	if (cpr_platform_enable)
		return (1);

	return (pm_S3_enabled);
}

void
i_cpr_bitmap_cleanup(void)
{
}

void
i_cpr_free_memory_resources(void)
{
}

/*
 * Needed only for S3 so far
 */
static int
i_cpr_platform_alloc(psm_state_request_t *req)
{
	char	*str = "i_cpr_platform_alloc";

	PMD(PMD_SX, ("cpu = %d, %s(%p) \n", CPU->cpu_id, str, (void *)req))

	if (ncpus == 1) {
		PMD(PMD_SX, ("%s() : ncpus == 1\n", str))
		return (0);
	}

	req->psr_cmd = PSM_STATE_ALLOC;
	return ((*psm_state)(req));
}

/*
 * Needed only for S3 so far
 */
static void
i_cpr_platform_free(psm_state_request_t *req)
{
	char	*str = "i_cpr_platform_free";

	PMD(PMD_SX, ("cpu = %d, %s(%p) \n", CPU->cpu_id, str, (void *)req))

	if (ncpus == 1) {
		PMD(PMD_SX, ("%s() : ncpus == 1\n", str))
	}

	req->psr_cmd = PSM_STATE_FREE;
	(void) (*psm_state)(req);
}

static int
i_cpr_save_apic(psm_state_request_t *req)
{
	char	*str = "i_cpr_save_apic";

	if (ncpus == 1) {
		PMD(PMD_SX, ("%s() : ncpus == 1\n", str))
		return (0);
	}

	req->psr_cmd = PSM_STATE_SAVE;
	return ((*psm_state)(req));
}

static int
i_cpr_restore_apic(psm_state_request_t *req)
{
	char	*str = "i_cpr_restore_apic";

	if (ncpus == 1) {
		PMD(PMD_SX, ("%s() : ncpus == 1\n", str))
		return (0);
	}

	req->psr_cmd = PSM_STATE_RESTORE;
	return ((*psm_state)(req));
}


/* stop lint complaining about offset not being used in 32bit mode */
#if !defined(__amd64)
/*ARGSUSED*/
#endif
static void
init_real_mode_platter(int cpun, uint32_t offset, uint_t cr4, wc_desctbr_t gdt)
{
	/*LINTED*/
	rm_platter_t *real_mode_platter = (rm_platter_t *)rm_platter_va;

	/*
	 * Fill up the real mode platter to make it easy for real mode code to
	 * kick it off. This area should really be one passed by boot to kernel
	 * and guaranteed to be below 1MB and aligned to 16 bytes. Should also
	 * have identical physical and virtual address in paged mode.
	 */

	real_mode_platter->rm_pdbr = getcr3();
	real_mode_platter->rm_cpu = cpun;
	real_mode_platter->rm_cr4 = cr4;

	real_mode_platter->rm_gdt_base = gdt.base;
	real_mode_platter->rm_gdt_lim = gdt.limit;

#if defined(__amd64)
	real_mode_platter->rm_x86feature = x86_feature;

	if (getcr3() > 0xffffffffUL)
		panic("Cannot initialize CPUs; kernel's 64-bit page tables\n"
		    "located above 4G in physical memory (@ 0x%llx).",
		    (unsigned long long)getcr3());

	/*
	 * Setup pseudo-descriptors for temporary GDT and IDT for use ONLY
	 * by code in real_mode_start():
	 *
	 * GDT[0]:  NULL selector
	 * GDT[1]:  64-bit CS: Long = 1, Present = 1, bits 12, 11 = 1
	 *
	 * Clear the IDT as interrupts will be off and a limit of 0 will cause
	 * the CPU to triple fault and reset on an NMI, seemingly as reasonable
	 * a course of action as any other, though it may cause the entire
	 * platform to reset in some cases...
	 */
	real_mode_platter->rm_temp_gdt[0] = 0ULL;
	real_mode_platter->rm_temp_gdt[TEMPGDT_KCODE64] = 0x20980000000000ULL;

	real_mode_platter->rm_temp_gdt_lim = (ushort_t)
	    (sizeof (real_mode_platter->rm_temp_gdt) - 1);
	real_mode_platter->rm_temp_gdt_base = rm_platter_pa +
	    (uint32_t)(&((rm_platter_t *)0)->rm_temp_gdt);

	real_mode_platter->rm_temp_idt_lim = 0;
	real_mode_platter->rm_temp_idt_base = 0;

	/*
	 * Since the CPU needs to jump to protected mode using an identity
	 * mapped address, we need to calculate it here.
	 */
	real_mode_platter->rm_longmode64_addr = rm_platter_pa + offset;
#endif	/* __amd64 */

	/* return; */
}

void
i_cpr_start_cpu(void)
{

	struct cpu *cp = CPU;

	char *str = "i_cpr_start_cpu";
	extern void init_cpu_syscall(struct cpu *cp);

#if defined(__amd64)
	wc_cpu_t	*cpup = wc_other_cpus + cp->cpu_id;
#endif	/*	__amd64	*/

	PMD(PMD_SX, ("%s() called\n", str))

	PMD(PMD_SX, ("%s() #0 cp->cpu_base_spl %d\n", str,
	    cp->cpu_base_spl))

	mutex_enter(&cpu_lock);
	if (cp == i_cpr_bootcpu()) {
		mutex_exit(&cpu_lock);
		PMD(PMD_SX,
		    ("%s() called on bootcpu nothing to do!\n", str))
		return;
	}
	mutex_exit(&cpu_lock);

	/*
	 * We need to Sync PAT with cpu0's PAT. We have to do
	 * this with interrupts disabled.
	 */
	if (x86_feature & X86_PAT)
		pat_sync();

	/*
	 * Initialize this CPU's syscall handlers
	 */
	init_cpu_syscall(cp);

	PMD(PMD_SX, ("%s() #1 cp->cpu_base_spl %d\n", str, cp->cpu_base_spl))

	/*
	 * Do not need to call cpuid_pass2(), cpuid_pass3(), cpuid_pass4() or
	 * init_cpu_info(), since the work that they do is only needed to
	 * be done once at boot time
	 */


	mutex_enter(&cpu_lock);

#if defined(__amd64)
	restore_stack(cpup);
#endif	/*	__amd64	*/

	CPUSET_ADD(procset, cp->cpu_id);
	mutex_exit(&cpu_lock);

	PMD(PMD_SX, ("%s() #2 cp->cpu_base_spl %d\n", str,
	    cp->cpu_base_spl))

	/* XXX remove before integration */
	PMD(PMD_SX, ("%s() procset 0x%lx\n", str, (ulong_t)procset))

	if (tsc_gethrtime_enable) {
		PMD(PMD_SX, ("%s() calling tsc_sync_slave\n", str))
		tsc_sync_slave();
	}

	PMD(PMD_SX, ("%s() cp->cpu_id %d, cp->cpu_intr_actv %d\n", str,
	    cp->cpu_id, cp->cpu_intr_actv))
	PMD(PMD_SX, ("%s() #3 cp->cpu_base_spl %d\n", str,
	    cp->cpu_base_spl))

	(void) spl0();		/* enable interrupts */

	PMD(PMD_SX, ("%s() #4 cp->cpu_base_spl %d\n", str,
	    cp->cpu_base_spl))

	/*
	 * Set up the CPU module for this CPU.  This can't be done before
	 * this CPU is made CPU_READY, because we may (in heterogeneous systems)
	 * need to go load another CPU module.  The act of attempting to load
	 * a module may trigger a cross-call, which will ASSERT unless this
	 * cpu is CPU_READY.
	 */

	/*
	 * cmi already been init'd (during boot), so do not need to do it again
	 */
#ifdef PM_REINITMCAONRESUME
	if (x86_feature & X86_MCA)
		cmi_mca_init();
#endif

	PMD(PMD_SX, ("%s() returning\n", str))

	/* return; */
}

#if defined(__amd64)
/*
 * we only need to do this for amd64!
 */

/*
 * save the stack
 */
void
save_stack(wc_cpu_t *cpup)
{
	char *str = "save_stack";
	caddr_t base = curthread->t_stk;
	caddr_t sp = (caddr_t)cpup->wc_rsp;


	PMD(PMD_SX, ("%s() CPU->cpu_id %d\n", str, CPU->cpu_id))
	PMD(PMD_SX, ("save_stack() curthread->t_stk = %p, sp = %p\n",
	    (void *)base, (void *)sp))

	ASSERT(base > sp);
	/*LINTED*/
	bcopy(sp, cpup->wc_stack, base - sp);

}

/*
 * restore the stack
 */
static	void
restore_stack(wc_cpu_t *cpup)
{
	/*
	 * we only need to do this for amd64!
	 */

	char *str = "restore_stack";
	caddr_t base = curthread->t_stk;
	caddr_t sp = (caddr_t)cpup->wc_rsp;

	PMD(PMD_SX, ("%s() CPU->cpu_id %d\n", str, CPU->cpu_id))
	PMD(PMD_SX, ("%s() curthread->t_stk = %p, sp = %p\n", str,
	    (void *)base, (void *)sp))

	ASSERT(base > sp);
	/*LINTED*/
	bcopy(cpup->wc_stack, sp, base - sp);

}

#endif	/*	__amd64	*/


void
i_cpr_alloc_cpus(void)
{
	char *str = "i_cpr_alloc_cpus";

	PMD(PMD_SX, ("%s() CPU->cpu_id %d\n", str, CPU->cpu_id))
	/*
	 * we allocate this only when we actually need it to save on
	 * kernel memory
	 */

	if (wc_other_cpus == NULL) {
		wc_other_cpus = kmem_zalloc(ncpus * sizeof (wc_cpu_t),
		    KM_SLEEP);
	}

}

void
i_cpr_free_cpus(void)
{
	if (wc_other_cpus != NULL) {
		kmem_free((void *) wc_other_cpus, ncpus * sizeof (wc_cpu_t));
		wc_other_cpus = NULL;
	}
}

/*
 * wrapper for acpica_ddi_save_resources()
 */
void
i_cpr_save_configuration(dev_info_t *dip)
{
	acpica_ddi_save_resources(dip);
}

/*
 * wrapper for acpica_ddi_restore_resources()
 */
void
i_cpr_restore_configuration(dev_info_t *dip)
{
	acpica_ddi_restore_resources(dip);
}
