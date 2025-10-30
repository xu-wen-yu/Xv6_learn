// 缓冲区缓存（Buffer cache）。
//
// 缓冲区缓存是一个由 buf 结构组成的链表，保存了磁盘块内容的缓存副本。
// 将磁盘块缓存在内存中可以减少磁盘读取次数，并为多个进程使用的磁盘块提供同步点。
//
// 接口：
// * 要获取某个磁盘块对应的缓冲区，调用 bread。
// * 修改缓冲区数据后，调用 bwrite 将其写回磁盘。
// * 使用完缓冲区后，调用 brelse 释放它。
// * 在调用 brelse 之后不要再使用该缓冲区。
// * 同一时间只有一个进程可以使用某个缓冲区，
//     因此不要长时间占用缓冲区。


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 31//增大哈希桶的个数能有效减少争用的发生

// 每个哈希桶：一把自旋锁 + 一条以 head 为哨兵的双向循环链表
struct hbucket {
  struct spinlock lock; // bcache 开头命名的桶锁，仅保护该桶
  struct buf head;      // head.next 为 MRU，head.prev 为 LRU
};

struct {
  struct buf buf[NBUF];             // 缓存块静态数组
  struct hbucket hashbucket[NBUCKETS]; // 每个哈希组一个链表和一把锁
} bcache;

// 将设备号和块号混合后取模得到桶索引的简单哈希函数。
static uint
hash(uint dev, uint blockno)
{
  return (dev ^ blockno) % NBUCKETS;
}

void
binit(void)
{
  struct buf *b;

  // 初始化每个哈希桶的自旋锁与链表头
  for(int i = 0; i < NBUCKETS; i++){
    initlock(&bcache.hashbucket[i].lock, "bcache.bucket");
    bcache.hashbucket[i].head.prev = &bcache.hashbucket[i].head;
    bcache.hashbucket[i].head.next = &bcache.hashbucket[i].head;
  }

  // 初始化每个缓冲区，并把它们分散到各个哈希桶中（避免重复加入所有桶）
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    int idx = (int)((b - bcache.buf) % NBUCKETS);
    b->next = bcache.hashbucket[idx].head.next;
    b->prev = &bcache.hashbucket[idx].head;
    initsleeplock(&b->lock, "buffer");
    b->lastuse = 0;
    bcache.hashbucket[idx].head.next->prev = b;
    bcache.hashbucket[idx].head.next = b;
  }
}

// 在缓冲区缓存中查找设备 dev 上的指定块。
// 如果未找到，则分配一个缓冲区（回收空闲的缓冲区）。
// 无论哪种情况，返回的缓冲区都是已锁定的。
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int h = hash(dev, blockno);

  // 该块是否已被缓存？在目标桶内原子查找
  acquire(&bcache.hashbucket[h].lock);
  for(b = bcache.hashbucket[h].head.next; b != &bcache.hashbucket[h].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->lastuse = ticks;
      release(&bcache.hashbucket[h].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 未缓存。
  // 在目标哈希桶中选择“最后一次被访问时间最早”的空闲块（refcnt==0）。
  struct buf *oldest = 0;
  for(struct buf *c = bcache.hashbucket[h].head.next; c != &bcache.hashbucket[h].head; c = c->next){
    if(c->refcnt == 0) {
      if(oldest == 0 || c->lastuse < oldest->lastuse)
        oldest = c;
    }
  }
  if(oldest){
    b = oldest;
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    b->lastuse = ticks;
    release(&bcache.hashbucket[h].lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.hashbucket[h].lock);

  // 如果目标桶没有可回收的缓冲区，遍历其它哈希桶寻找可回收的缓冲区，
  // 找到后从原桶移除并插入到目标桶，再返回该缓冲区用于使用。否则程序panic。
  // 从不同桶开始遍历，降低热点：按 h 的下一个桶开始，环形扫描
  for(int off = 1; off < NBUCKETS; off++){
    int i = (h + off) % NBUCKETS;

    int first = i < h ? i : h;
    int second = i < h ? h : i;

    // 固定的加锁顺序避免死锁
    acquire(&bcache.hashbucket[first].lock);
    acquire(&bcache.hashbucket[second].lock);

    // 在持有目标桶锁的情况下再次检查是否已被插入（避免重复条目）
    for(struct buf *b2 = bcache.hashbucket[h].head.next; b2 != &bcache.hashbucket[h].head; b2 = b2->next){
      if(b2->dev == dev && b2->blockno == blockno){
        b2->refcnt++;
        release(&bcache.hashbucket[second].lock);
        release(&bcache.hashbucket[first].lock);
        acquiresleep(&b2->lock);
        return b2;
      }
    }

    // 在桶 i 内选择“最后一次被访问时间最早”的空闲块
    struct buf *ih = &bcache.hashbucket[i].head;
    struct buf *victim = 0;
    for(struct buf *c = ih->next; c != ih; c = c->next){
      if(c->refcnt == 0){
        if(victim == 0 || c->lastuse < victim->lastuse)
          victim = c;
      }
    }
    if(victim){
      b = victim;
      // 从源桶中摘除
      b->next->prev = b->prev;
      b->prev->next = b->next;

      // 插入到目标桶（位置不再表达 LRU，仅作为集合）
      b->next = bcache.hashbucket[h].head.next;
      b->prev = &bcache.hashbucket[h].head;
      bcache.hashbucket[h].head.next->prev = b;
      bcache.hashbucket[h].head.next = b;

      // 赋予新身份
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      b->lastuse = ticks;

      release(&bcache.hashbucket[second].lock);
      release(&bcache.hashbucket[first].lock);
      acquiresleep(&b->lock);
      return b;
    }

    release(&bcache.hashbucket[second].lock);
    release(&bcache.hashbucket[first].lock);
  }
  panic("bget: no buffers");
}

// 返回一个已锁定的 buf，包含指定磁盘块的内容。
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// 将 b 的内容写入磁盘。调用时 buf 必须已被锁定。
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// 释放一个已锁定的缓冲区。
// 使用时间戳维护近似 LRU，不再依赖链表顺序表达 LRU。
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  // 用桶锁保证 refcnt/lastuse 的原子更新
  int h = hash(b->dev, b->blockno);
  acquire(&bcache.hashbucket[h].lock);
  b->lastuse = ticks;
  b->refcnt--;
  release(&bcache.hashbucket[h].lock);
}

void
bpin(struct buf *b) {
  int h = hash(b->dev, b->blockno);
  acquire(&bcache.hashbucket[h].lock);
  b->refcnt++;
  release(&bcache.hashbucket[h].lock);
}

void
bunpin(struct buf *b) {
  int h = hash(b->dev, b->blockno);
  acquire(&bcache.hashbucket[h].lock);
  b->refcnt--;
  release(&bcache.hashbucket[h].lock);
}


