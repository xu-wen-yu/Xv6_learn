// 物理内存分配器，用于用户进程、
// 内核栈、页表页和管道缓冲区。按整页（4096 字节）分配。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end, int i);

extern char end[]; // 内核之后的第一个地址。
                   // 由 kernel.ld 定义。

// 空闲页链表节点
struct run {
  struct run *next; // 指向下一个空闲页
};

// 全局内存管理结构
struct kmem {
  struct spinlock lock; // 保护空闲链表的自旋锁
  struct run *freelist; // 空闲物理页链表头
};

struct kmem kmems[NCPU];

void
kinit()
{
  char *start = end;
  uint64 total = PHYSTOP - (uint64)end;
  uint64 period = total / NCPU;
  for(int i = 0; i < NCPU; i++){
    initlock(&kmems[i].lock, "kmem");
    char *endptr = start + period;
    // 把该段内存加入到对应 CPU 的 freelist 中
    freerange(start, endptr, i);
    start = endptr;
  }
}

void
freerange(void *pa_start, void *pa_end, int i)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  memset(p, 1, PGSIZE);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){//将kfree功能移到此处，因为kfree传参无法传进i
    struct run *r = (struct run*)p;
    acquire(&kmems[i].lock);
    r->next = kmems[i].freelist;
    kmems[i].freelist = r;
    release(&kmems[i].lock);//freerange()则是通过调用kfree()把每个空闲页（地址范围从pa_start至pa_end）逐一加到链表里来实现此功能的
  }
}

// 释放由 v 指向的物理内存页，将其添加至freelist中
// 通常应该是由 kalloc() 返回的页。
// （例外情况是在初始化分配器时；见上面的 kinit。）
void
kfree(void *pa)//参数pa为需要释放的物理页页号，即物理页的首地址，它被看作一个没有类型的指针
                      //defs.h中声明为void kfree(void *);
{
  struct run *r;
  int id;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 用垃圾数据填充以捕获悬挂引用。
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 将释放的页(cpuid)加入到当前 CPU 的 freelist 中。
  // 避免全局分配，减少锁争用
  id = cpuid();
  acquire(&kmems[id].lock);
  r->next = kmems[id].freelist;
  kmems[id].freelist = r;
  release(&kmems[id].lock);
}

// 分配一个 4096 字节的物理内存页。
// 返回内核可用的指针。
// 若无法分配则返回 0。
void *
kalloc(void)
{
  struct run *r;
  int local = cpuid();

  // 优先从本地 CPU 的空闲链表分配
  acquire(&kmems[local].lock);
  r = kmems[local].freelist;
  if(r)
    kmems[local].freelist = r->next;
  release(&kmems[local].lock);

  // 若本cpu无空闲页，则尝试从其他 CPU "偷取" 一页
  if(r == 0){
    for(int i = 0; i < NCPU; i++){
      int steal = (local + i) % NCPU;
      if(steal == local) continue; // 已尝试过本cpu,直接跳过
      acquire(&kmems[steal].lock);
      r = kmems[steal].freelist;
      if(r)
        kmems[steal].freelist = r->next;
      release(&kmems[steal].lock);
      if(r) break;//偷取完跳出循环
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // 用垃圾数据填充
  return (void*)r;
}
