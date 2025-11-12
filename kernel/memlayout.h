// 物理内存布局

// qemu 使用 -machine virt 时的内存布局如下，
// 布局依据 qemu 的 hw/riscv/virt.c：
//
// 00001000 -- 启动 ROM，由 qemu 提供
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio 磁盘 
// 80000000 -- 启动 ROM 在机器模式下跳转到此
//             -kernel 在此处加载内核
// 80000000 之后是未使用的 RAM。

// 内核以如下方式使用物理内存：
// 80000000 -- entry.S，然后是内核的文本和数据
// end -- 内核页分配区域的起始地址
// PHYSTOP -- 内核使用的 RAM 结束位置

// qemu 在物理内存中的这个位置放置 UART 寄存器。
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio 接口
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// 本地中断控制器，内部包含定时器。
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.

// qemu 在此放置可编程中断控制器。
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// 内核假定在物理地址 0x80000000 到 PHYSTOP 之间存在 RAM，
// 供内核和用户页使用。
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// 将 trampoline 页面映射到最高地址，
// 用户态和内核态共享。
#define TRAMPOLINE (MAXVA - PGSIZE)

// 在 trampoline 下方映射内核栈，
// 每个栈两侧都由无效保护页包围。
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// 用户内存布局。
// 从地址零开始：
//   文本段
//   原始数据段和 bss
//   固定大小的栈
//   可扩展的堆
//   ...
//   TRAPFRAME（p->trapframe，由 trampoline 使用）
//   TRAMPOLINE（与内核中相同的页面）
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
