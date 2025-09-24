#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"          // ★ 新增：引入 slab 接口（如已在 defs.h 声明可省略）

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");

    kinit();             // physical page allocator（页级分配器）
    kvminit();           // create kernel page table
    kvminithart();       // turn on paging

    kmalloc_init();      // ★ 新增：初始化 slab 的各个 size-class
    pipe_cache_init();

#ifdef LAB1_SLAB_TEST
    slab_selftest();     // ★ 可选：启用自测（需在 CFLAGS 里 -DLAB1_SLAB_TEST）
#endif

    procinit();          // process table
    trapinit();          // trap vectors
    trapinithart();      // install kernel trap vector
    plicinit();          // set up interrupt controller
    plicinithart();      // ask PLIC for device interrupts
    binit();             // buffer cache
    iinit();             // inode table
    fileinit();          // file table
    virtio_disk_init();  // emulated hard disk
    userinit();          // first user process

    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();
}
