
#include "syslib.h"

#include <minix/vm.h>

/*===========================================================================*
 *                                vm_forgetblocks		     	*
 *===========================================================================*/
void vm_forgetblocks(void)
{
	message m;
	_taskcall(VM_PROC_NR, VM_FORGETBLOCKS, &m);
	return;
}

/*===========================================================================*
 *                                vm_forgetblock		     	*
 *===========================================================================*/
int vm_forgetblock(u64_t id)
{
	message m;

	m.VMFB_IDHI = (unsigned long)(id>>32);
	m.VMFB_IDLO = (unsigned long)id;

	return _taskcall(VM_PROC_NR, VM_FORGETBLOCK, &m);
}

/*===========================================================================*
 *                                vm_yield_block_get_block	     	*
 *===========================================================================*/
int vm_yield_block_get_block(u64_t yieldid, u64_t getid,
	void *mem, vir_bytes len)
{
	message m;

	m.VMYBGB_VADDR = mem;
	m.VMYBGB_GETIDHI = (unsigned long)(getid>>32);
	m.VMYBGB_GETIDLO = (unsigned long)getid;
	m.VMYBGB_LEN = len;
	m.VMYBGB_YIELDIDHI = (unsigned long)(yieldid>>32);
	m.VMYBGB_YIELDIDLO = (unsigned long)yieldid;

	return _taskcall(VM_PROC_NR, VM_YIELDBLOCKGETBLOCK, &m);
}

