// kernel/slab.c  —— 单 cache + slab 页切分 + freelist + alloc/free + 调试/评测
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "slab.h"
// 允许先 printf 再 panic 的便捷宏（修复 panic 只能接 1 个参数的问题）
#define PANICF(fmt, ...) do { printf(fmt, ##__VA_ARGS__); panic("panicf"); } while (0)


// —— 常量与工具 —— //
#define SLAB_MAGIC 0x51AB
#define CACHE_DEFAULT_ALIGN 16

// ====== 调试与评测开关 ======
#ifndef SLAB_DEBUG
#define SLAB_DEBUG 1        // 1=启用红区/毒化/状态机；0=关闭
#endif
#ifndef SLAB_EVAL
#define SLAB_EVAL  1        // 1=启动时跑 slab 评测；0=不跑
#endif

#if SLAB_DEBUG
#define RZ_BYTES   16       // 对象尾部红区长度
#define PAT_ALLOC  0xA5     // 分配后填充
#define PAT_FREE   0xCC     // 释放后填充
#define PAT_RZ     0xDE     // 红区图案
#endif

#if SLAB_EVAL
#define EVAL_SLOTS        10000   // 每次最多在堆上挂多少对象
#define OOB_INJECT_RATE   0    // 尾部越界注入概率 ~1/1024（SLAB_DEBUG=1 时生效）
#define DFREE_INJECT_RATE 0    // double-free 注入概率 ~1/2048
static inline uint r32(void){ static uint64 s=88172645463325252ULL; s^=s<<7; s^=s>>9; return (uint)s; }
#endif

static inline uint maxu(uint a, uint b) { return a > b ? a : b; }
static inline uint align_up(uint n, uint a) { return (n + a - 1) & ~(a - 1); }

// —— size-class 表 —— //
static uint size_classes[] = {8,16,32,64,128,256,512,1024,2048};
#define N_CLASS (sizeof(size_classes)/sizeof(size_classes[0]))
static struct kmem_cache *class_cache[N_CLASS];
static const char *class_names[N_CLASS] = {
  "km-8","km-16","km-32","km-64","km-128","km-256","km-512","km-1024","km-2048"
};

static int class_index_for(uint size){
  for(int i=0;i<N_CLASS;i++) if(size <= size_classes[i]) return i;
  return -1;
}

// —— 结构体 —— //
struct slab;
struct slabobj_hdr {
  struct slab *slab;
  ushort idx;
  ushort magic;
#if SLAB_DEBUG
  uchar  state;   // 0=free, 1=alloc
#endif
};

// slab：占用一整页，把页内切成多个对象位
struct slab {
  struct slab *next;
  struct kmem_cache *cache;
  char  *page;       // 页首（用于 kfree(page) 归还整页）
  char  *mem;        // 对象区域起始地址（在页内）
  uint   objsize;    // 对象 payload 大小（不含hdr）
  uint   stride;     // 单对象步长 = align(hdr+obj)
  uint   nr_objs;    // 总对象数
  uint   nr_free;    // 空闲对象数
  void  *free_head;  // freelist 头指针（单链），链接在对象payload的前 sizeof(void*) 处
};

// kmem_cache：某种尺寸对象的池，维护三条链
struct kmem_cache {
  char   name[32];
  uint   objsize;
  uint   align;
  void (*ctor)(void *);
  void (*dtor)(void *);
  struct slab *partial; // 还有空闲
  struct slab *full;    // 没空闲
  struct slab *empty;   // 全空
  struct spinlock lock;
};

// —— 小链表工具 —— //
static void list_push(struct slab **head, struct slab *s) { s->next = *head; *head = s; }
static void list_remove(struct slab **head, struct slab *s) {
  struct slab **pp = head;
  while (*pp) {
    if (*pp == s) { *pp = s->next; break; }
    pp = &(*pp)->next;
  }
  s->next = 0;
}

// —— 页 → slab 切分 —— //
static struct slab *slab_new(struct kmem_cache *c) {
  char *page = (char *)kalloc();
  if (!page) return 0;

  struct slab *s = (struct slab *)page;
  s->cache = c; s->page = page; s->next = 0;

  uint align = c->align ? c->align : CACHE_DEFAULT_ALIGN;
  uint stride = align_up(sizeof(struct slabobj_hdr) + c->objsize
#if SLAB_DEBUG
                         + RZ_BYTES
#endif
                         , maxu(align, sizeof(void *)));
  uint space  = PGSIZE - sizeof(struct slab);
  uint nobj   = space / stride;
  if (nobj == 0) { kfree(page); return 0; }

  s->objsize = c->objsize;
  s->stride  = stride;
  s->nr_objs = nobj;
  s->nr_free = nobj;
  s->mem     = page + sizeof(struct slab);
  s->free_head = 0;

  // 初始化 freelist：把每个对象的开头当作“next 指针”链接起来
  for (uint i = 0; i < nobj; i++) {
    char *slot = s->mem + i * stride;
    struct slabobj_hdr *h = (struct slabobj_hdr *)slot;
    h->slab = s; h->idx = i; h->magic = SLAB_MAGIC;
#if SLAB_DEBUG
    h->state = 0;
#endif
    void *obj = (void *)(slot + sizeof(struct slabobj_hdr));
#if SLAB_DEBUG
    memset((char*)obj + c->objsize, PAT_RZ, RZ_BYTES); // 尾红区
#endif
    *(void **)obj = s->free_head;
    s->free_head = obj;
  }
  return s;
}

static void slab_free_page(struct slab *s) { kfree(s->page); }

// —— 面向类型的接口 —— //
struct kmem_cache *kmem_cache_create(const char *name, uint objsize,
                                     void (*ctor)(void *), void (*dtor)(void *),
                                     uint align) {
  struct kmem_cache *c = (struct kmem_cache *)kalloc();
  if (!c) return 0;
  memset(c, 0, PGSIZE);
  safestrcpy(c->name, name, sizeof(c->name));
  c->objsize = objsize;
  c->align   = align ? align : CACHE_DEFAULT_ALIGN;
  c->ctor    = ctor;
  c->dtor    = dtor;
  initlock(&c->lock, c->name);
  return c;
}

void kmem_cache_destroy(struct kmem_cache *c) {
  acquire(&c->lock);
  struct slab *s;
  for (s = c->full; s;)    { struct slab *n = s->next; slab_free_page(s); s = n; }
  for (s = c->partial; s;) { struct slab *n = s->next; slab_free_page(s); s = n; }
  for (s = c->empty; s;)   { struct slab *n = s->next; slab_free_page(s); s = n; }
  release(&c->lock);
  kfree((void *)c);
}

void *kmem_cache_alloc(struct kmem_cache *c) {
  acquire(&c->lock);
  struct slab *s = c->partial ? c->partial : c->empty;
  if (!s) {
    s = slab_new(c);
    if (!s) { release(&c->lock); return 0; }
    list_push(&c->partial, s);
  } else if (s == c->empty) {
    list_remove(&c->empty, s);
    list_push(&c->partial, s);
  }

  void *obj = s->free_head;
  s->free_head = *(void **)obj;
  s->nr_free--;

  if (s->nr_free == 0) {
    list_remove(&c->partial, s);
    list_push(&c->full, s);
  }
  release(&c->lock);

#if SLAB_DEBUG
  // 调试：校验状态、毒化 & 刷新红区
  struct slabobj_hdr *h = (struct slabobj_hdr *)((char *)obj - sizeof(struct slabobj_hdr));
  if (h->magic != SLAB_MAGIC || h->state != 0) panic("slab: corrupt/double-alloc");
  h->state = 1;
  memset(obj, PAT_ALLOC, c->objsize);
  memset((char*)obj + c->objsize, PAT_RZ, RZ_BYTES);
#endif

  if (c->ctor) c->ctor(obj);
  return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
  if (!obj) return;
  struct slabobj_hdr *h = (struct slabobj_hdr *)((char *)obj - sizeof(struct slabobj_hdr));
  if (h->magic != SLAB_MAGIC || !h->slab || h->slab->cache != c) {
    // 基本保护：不是我们家的对象就忽略
    return;
  }
  struct slab *s = h->slab;
  if (c->dtor) c->dtor(obj);

#if SLAB_DEBUG
  // 检测 double-free/UAF 与尾部越界
  if (h->state != 1) panic("slab: double free / UAF");
  for (int i = 0; i < RZ_BYTES; i++) {
    if (((uchar*)obj)[c->objsize + i] != PAT_RZ)
      panic("slab: tail OOB write");
  }
  memset(obj, PAT_FREE, c->objsize);
  h->state = 0;
  memset((char*)obj + c->objsize, PAT_RZ, RZ_BYTES);
#endif

  acquire(&c->lock);
  if (s->nr_free == 0) { // full -> partial
    list_remove(&c->full, s);
    list_push(&c->partial, s);
  }
  *(void **)obj = s->free_head;
  s->free_head = obj;
  s->nr_free++;
  if (s->nr_free == s->nr_objs) { // partial -> empty（全空）
    list_remove(&c->partial, s);
    list_push(&c->empty, s);
    // 可在 Step 3 加入阈值回收：slab_free_page(s);
  }
  release(&c->lock);
}

// —— 通用接口（Step 2） —— //
void kmalloc_init(void) {
  // 1) 预建所有 size-class 的 cache
  for (int i = 0; i < N_CLASS; i++) {
    class_cache[i] = kmem_cache_create(class_names[i], size_classes[i], 0, 0, 16);
    if (!class_cache[i]) {
      printf("[slab] kmalloc_init: create %s failed\n", class_names[i]);
    }
  }

  // 2) 简单冒烟测试
  void *arr[64]; int pos = 0;
  for (int s = 1; s <= 2000; s += 31) {
    int idx = class_index_for((uint)s);
    if (idx >= 0) {
      void *p = kmem_cache_alloc(class_cache[idx]);
      if (p && pos < 64) arr[pos++] = p;
    }
  }
  for (int i = 0; i < pos; i++) kfree_slab(arr[i]);

  printf("[slab] kmalloc init OK (size-classes ready)\n");

#if SLAB_EVAL
  // 启动即跑一次 slab 评测（deterministic + fuzz）
  extern void slab_eval(void);
  slab_eval();
#endif
}

void *kmalloc(uint size) {
  if (size == 0) return 0;
  int idx = class_index_for(size);
  if (idx < 0) {
    // 先只支持 ≤ 2048B；更大块后续可扩展
    return 0;
  }
  return kmem_cache_alloc(class_cache[idx]);
}

void kfree_slab(void *p) {
  if (!p) return;
  struct slabobj_hdr *h = (struct slabobj_hdr *)((char*)p - sizeof(struct slabobj_hdr));
  if (h->magic != SLAB_MAGIC || !h->slab || !h->slab->cache) {
    // 非 slab 对象（或野指针），忽略
    return;
  }
  kmem_cache_free(h->slab->cache, p);
}

void kmalloc_stats(void) {
  printf("[slab] stats:\n");
  for (int i = 0; i < N_CLASS; i++) {
    struct kmem_cache *c = class_cache[i];
    if (!c) continue;

    uint slabs = 0, objs = 0, freec = 0;
    acquire(&c->lock);
    for (struct slab *s = c->full; s; s = s->next)    { slabs++; objs += s->nr_objs; }
    for (struct slab *s = c->partial; s; s = s->next) { slabs++; objs += s->nr_objs; freec += s->nr_free; }
    for (struct slab *s = c->empty; s; s = s->next)   { slabs++; objs += s->nr_objs; freec += s->nr_free; }
    release(&c->lock);

    uint pages   = slabs * 1;                  // 每 slab 1 页
    uint bytes   = pages * PGSIZE;             // 粗略总字节
    uint payload = objs * c->objsize;          // 有效载荷
    uint used    = objs - freec;
    uint util    = (bytes ? (payload * 100) / bytes : 0);

    printf("  %-8s obj=%4u  pages=%2u  objs=%4u  used=%4u  free=%4u  util=%3u%%\n",
           class_names[i], c->objsize, pages, objs, used, freec, util);
  }
}

// ====== 启动评测：确定性边界 + fuzz（含低概率注入） ======
#if SLAB_EVAL
static const short det_sizes[] = {
  1,2,3,4,7,8,15,16,17,32,48,64,65,96,128,192,256,257,384,512,513,1024,1536,2040
};

static inline int pick_size(void){
  int r = r32() & 255, z = 0;
  if (r < 127) { z = r32()%48 + 17; z = align_up(z, 4); }
  else if (r < 181) { z = r32()%16 + 1; }
  else if (r < 235) { z = r32()%192 + 65; z = align_up(z, 8); }
  else if (r < 255) { z = r32()%256 + 257; z = align_up(z, 8); }
  else { z = r32()%1528 + 513; z = align_up(z, 8); }
  return z;
}

void slab_eval(void){
  printf("[eval] slab deterministic + fuzz\n");

  // Phase 1: 确定性边界集
  for (int t=0; t<sizeof(det_sizes)/sizeof(det_sizes[0]); t++){
    int z = det_sizes[t];
    void *q = kmalloc(z);
    if (!q) PANICF("eval: kmalloc(%d)=0", z);
    uint64 ptr = (uint64)q;
    if (((z&1)==0 && (ptr&1)) || ((z&3)==0 && (ptr&3)) || ((z&7)==0 && (ptr&7)))
      PANICF("eval: align %d bad %p", z, q);
    memset(q, (uchar)(z ^ 0x5A), z);
    for (int k=0; k<z; k++) if (((uchar*)q)[k] != (uchar)(z ^ 0x5A))
      PANICF("eval: det payload %d mismatch", z);
    kfree_slab(q);
  }
  printf("[eval] phase1 ok\n");

  // Phase 2: fuzz（随机分配/释放 + 低概率注入）
  static void *slots[EVAL_SLOTS];
  static short  sizes[EVAL_SLOTS];
int j = 0;
const int MAX_OPS = 60000; // 总操作数（够大以覆盖“装满/释放”两种路径）

for (int ops = 0; ops < MAX_OPS; ops++) {
  // ~9/16 概率做分配；当 j 过大时也会自然进入释放分支
  if (j == 0 || j < 1000 || ((r32() & 15) > 6)) {
    int z = pick_size();
    void *q = kmalloc(z);
    if (!q) PANICF("eval: kmalloc fuzz %d=0\n", z);
    uint64 a = (uint64)q;
    if (((z&1)==0 && (a&1)) || ((z&3)==0 && (a&3)) || ((z&7)==0 && (a&7)))
      PANICF("eval: fuzz align %d bad %p\n", z, q);
    memset(q, (uchar)(z ^ 0xA5), z);
    slots[j] = q; sizes[j] = z; j++;
    if (j >= EVAL_SLOTS) { // 防止越界，达到上限就强制释放一个
      kfree_slab(slots[--j]);
    }
  } else {
    int k = r32() % j;
    void *q = slots[k];
    int   z = sizes[k];
    if (!q) panic("eval: null slot");
    for (int t = 0; t < z; t++)
      if (((uchar*)q)[t] != (uchar)(z ^ 0xA5))
        panic("eval: fuzz payload mismatch");

#if SLAB_DEBUG
    if (OOB_INJECT_RATE && ((r32() & (OOB_INJECT_RATE - 1)) == 0)) {
      ((uchar*)q)[z] = 0xFF;   // 尾越界 1 字节
    }
    if (DFREE_INJECT_RATE && ((r32() & (DFREE_INJECT_RATE - 1)) == 0)) {
      kfree_slab(q);           // 第一次释放
      kfree_slab(q);           // 第二次释放 → 期望 panic（如开启注入）
      slots[k] = 0;
      continue;
    }
#endif
    kfree_slab(q);
    slots[k]  = slots[--j];
    sizes[k]  = sizes[j];
  }
}
// 收尾：释放所有残留对象
for (int t = 0; t < j; t++) kfree_slab(slots[t]);
printf("[eval] phase2 fuzz ok\n");

}
#endif // SLAB_EVAL
