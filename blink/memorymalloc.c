/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/bus.h"
#include "blink/debug.h"
#include "blink/errno.h"
#include "blink/linux.h"
#include "blink/lock.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/map.h"
#include "blink/pml4t.h"
#include "blink/types.h"
#include "blink/util.h"
#include "blink/x86.h"

struct Allocator {
  pthread_mutex_t lock;
  _Atomic(u8 *) brk;
  struct HostPage *pages GUARDED_BY(lock);
} g_allocator = {
    PTHREAD_MUTEX_INITIALIZER,
};

static void FillPage(void *p, int c) {
  memset(p, c, 4096);
}

static void ClearPage(void *p) {
  FillPage(p, 0);
}

static struct HostPage *NewHostPage(void) {
  return (struct HostPage *)malloc(sizeof(struct HostPage));
}

static void FreeHostPage(struct HostPage *hp) {
  free(hp);
}

static void FreeAnonymousPage(struct System *s, u8 *page) {
  struct HostPage *h;
  unassert((h = NewHostPage()));
  LOCK(&g_allocator.lock);
  h->page = page;
  h->next = g_allocator.pages;
  g_allocator.pages = h;
  UNLOCK(&g_allocator.lock);
}

static void CleanupAllocator(void) {
  struct HostPage *h;
  while ((h = g_allocator.pages)) {
    g_allocator.pages = h->next;
    FreeHostPage(h);
  }
}

static size_t GetBigSize(size_t n) {
  unassert(n);
  long z = GetSystemPageSize();
  return ROUNDUP(n, z);
}

void FreeBig(void *p, size_t n) {
  if (!p) return;
  unassert(!Munmap(p, n));
}

void *AllocateBig(size_t n, int prot, int flags, int fd, off_t off) {
  void *p;
  static bool once;
  if (!once) {
    atexit(CleanupAllocator);
    once = true;
  }
#if defined(__CYGWIN__) || defined(__EMSCRIPTEN__)
  p = Mmap(0, n, prot, flags, fd, off, "big");
  return p != MAP_FAILED ? p : 0;
#else
  u8 *brk;
  size_t m;
  if (!(brk = atomic_load_explicit(&g_allocator.brk, memory_order_relaxed))) {
    // we're going to politely ask the kernel for addresses starting
    // arbitrary megabytes past the end of our own executable's .bss
    // section. we'll cross our fingers, and hope that gives us room
    // away from a brk()-based libc malloc() function which may have
    // already allocated memory in this space. the reason it matters
    // is because the x86 and arm isas impose limits on displacement
    atomic_compare_exchange_strong_explicit(
        &g_allocator.brk, &brk, (u8 *)kPreciousStart, memory_order_relaxed,
        memory_order_relaxed);
  }
  m = GetBigSize(n);
  do {
    brk = atomic_fetch_add_explicit(&g_allocator.brk, m, memory_order_relaxed);
    if (brk + m > (u8 *)kPreciousEnd) {
      enomem();
      return 0;
    }
    p = Mmap(brk, n, prot, flags | MAP_DEMAND, fd, off, "big");
  } while (p == MAP_FAILED && errno == MAP_DENIED);
  return p != MAP_FAILED ? p : 0;
#endif
}

static bool FreePageTables(struct System *s, u64 pt, long level) {
  u8 *mi;
  long i;
  bool canfree = true;
  mi = GetPageAddress(s, pt);
  for (i = 0; i < 512; ++i) {
    if (level == 4) {
      if (Read64(mi + i * 8)) {
        canfree = false;
      }
    } else {
      pt = Read64(mi + i * 8);
      if (pt & PAGE_V) {
        if (FreePageTables(s, pt, level + 1)) {
          Write64(mi + i * 8, 0);
        } else {
          canfree = false;
        }
      } else {
        unassert(!pt);
      }
    }
  }
  if (canfree) {
    FreeAnonymousPage(s, mi);
    --s->memstat.pagetables;
    --s->rss;
  }
  return canfree;
}

static void FreeHostPages(struct System *s) {
  if (!s->real && s->cr3) {
    unassert(!FreeVirtual(s, -0x800000000000, 0x1000000000000));
    unassert(FreePageTables(s, s->cr3, 1));
    s->cr3 = 0;
  }
  free(s->real);
  s->real = 0;
}

void CleanseMemory(struct System *s, size_t size) {
  i64 oldrss;
  if (s->memchurn >= s->rss / 2) {
    oldrss = s->rss;
    (void)oldrss;
    FreePageTables(s, s->cr3, 1);
    MEM_LOGF("freed %" PRId64 " page tables", oldrss - s->rss);
    s->memchurn = 0;
  }
}

i64 GetMaxVss(struct System *s) {
  return MIN(kMaxVirtual, Read64(s->rlim[RLIMIT_AS_LINUX].cur)) / 4096;
}

i64 GetMaxRss(struct System *s) {
  return MIN(kMaxResident, Read64(s->rlim[RLIMIT_AS_LINUX].cur)) / 4096;
}

struct System *NewSystem(int mode) {
  long i;
  struct System *s;
  unassert(mode == XED_MODE_REAL ||    //
           mode == XED_MODE_LEGACY ||  //
           mode == XED_MODE_LONG);
  if (posix_memalign((void **)&s, _Alignof(struct System), sizeof(*s))) {
    enomem();
    return 0;
  }
  memset(s, 0, sizeof(*s));
  s->mode = mode;
  if (s->mode == XED_MODE_REAL) {
    if (posix_memalign((void **)&s->real, 4096, kRealSize)) {
      free(s);
      enomem();
      return 0;
    }
  }
  InitJit(&s->jit);
  InitFds(&s->fds);
  pthread_mutex_init(&s->sig_lock, 0);
  pthread_mutex_init(&s->mmap_lock, 0);
  pthread_mutex_init(&s->exec_lock, 0);
  pthread_cond_init(&s->machines_cond, 0);
  pthread_mutex_init(&s->machines_lock, 0);
  s->blinksigs = 1ull << (SIGSYS_LINUX - 1) |   //
                 1ull << (SIGILL_LINUX - 1) |   //
                 1ull << (SIGFPE_LINUX - 1) |   //
                 1ull << (SIGSEGV_LINUX - 1) |  //
                 1ull << (SIGTRAP_LINUX - 1);
  for (i = 0; i < RLIM_NLIMITS_LINUX; ++i) {
    Write64(s->rlim[i].cur, RLIM_INFINITY_LINUX);
    Write64(s->rlim[i].max, RLIM_INFINITY_LINUX);
  }
  s->automap = kAutomapStart;
  s->pid = getpid();
  return s;
}

static void FreeMachineUnlocked(struct Machine *m) {
  THR_LOGF("pid=%d tid=%d FreeMachine", m->system->pid, m->tid);
  UnlockRobustFutexes(m);
  if (g_machine == m) {
    g_machine = 0;
  }
  if (IsMakingPath(m)) {
    AbandonJit(&m->system->jit, m->path.jb);
  }
  CollectGarbage(m);
  free(m->freelist.p);
  free(m);
}

bool IsOrphan(struct Machine *m) {
  bool res;
  LOCK(&m->system->machines_lock);
  if (m->system->machines == m->system->machines->next &&
      m->system->machines == m->system->machines->prev) {
    unassert(m == MACHINE_CONTAINER(m->system->machines));
    res = true;
  } else {
    res = false;
  }
  UNLOCK(&m->system->machines_lock);
  return res;
}

void KillOtherThreads(struct System *s) {
  struct Dll *e;
  struct Machine *m;
  unassert(s == g_machine->system);
  unassert(!dll_is_empty(s->machines));
  while (!IsOrphan(g_machine)) {
    LOCK(&s->machines_lock);
    for (e = dll_first(s->machines); e; e = dll_next(s->machines, e)) {
      if ((m = MACHINE_CONTAINER(e)) != g_machine) {
        THR_LOGF("pid=%d tid=%d is killing tid %d", s->pid, g_machine->tid,
                 m->tid);
        atomic_store_explicit(&m->killed, true, memory_order_release);
      }
    }
    unassert(!pthread_cond_wait(&s->machines_cond, &s->machines_lock));
    UNLOCK(&s->machines_lock);
  }
}

void RemoveOtherThreads(struct System *s) {
  struct Dll *e, *g;
  struct Machine *m;
  LOCK(&s->machines_lock);
  for (e = dll_first(s->machines); e; e = g) {
    g = dll_next(s->machines, e);
    m = MACHINE_CONTAINER(e);
    if (m != g_machine) {
      dll_remove(&s->machines, e);
      FreeMachineUnlocked(m);
    }
  }
  UNLOCK(&s->machines_lock);
}

void FreeSystem(struct System *s) {
  THR_LOGF("pid=%d FreeSystem", s->pid);
  unassert(dll_is_empty(s->machines));  // Use KillOtherThreads & FreeMachine
  FreeHostPages(s);
  unassert(!pthread_mutex_destroy(&s->machines_lock));
  unassert(!pthread_cond_destroy(&s->machines_cond));
  unassert(!pthread_mutex_destroy(&s->exec_lock));
  unassert(!pthread_mutex_destroy(&s->mmap_lock));
  unassert(!pthread_mutex_destroy(&s->sig_lock));
  DestroyFds(&s->fds);
  DestroyJit(&s->jit);
  free(s);
}

struct Machine *NewMachine(struct System *system, struct Machine *parent) {
  _Static_assert(IS2POW(kMaxThreadIds), "");
  struct Machine *m;
  unassert(system);
  unassert(!parent || system == parent->system);
  if (posix_memalign((void **)&m, _Alignof(struct Machine), sizeof(*m))) {
    enomem();
    return 0;
  }
  // TODO(jart): We shouldn't be doing expensive ops in an allocator.
  LOCK(&system->machines_lock);
  if (parent) {
    memcpy(m, parent, sizeof(*m));
    memset(&m->path, 0, sizeof(m->path));
    memset(&m->freelist, 0, sizeof(m->freelist));
    ResetInstructionCache(m);
  } else {
    memset(m, 0, sizeof(*m));
    ResetCpu(m);
  }
  m->ctid = 0;
  m->oplen = 0;
  m->system = system;
  m->mode = system->mode;
  m->thread = pthread_self();
  Write32(m->sigaltstack.flags, SS_DISABLE_LINUX);
  if (parent) {
    m->tid = (system->next_tid++ & (kMaxThreadIds - 1)) + kMinThreadId;
  } else {
    // TODO(jart): We shouldn't be doing system calls in an allocator.
    m->tid = m->system->pid;
  }
  dll_init(&m->elem);
  // TODO(jart): Child thread should add itself to system.
  dll_make_first(&system->machines, &m->elem);
  UNLOCK(&system->machines_lock);
  THR_LOGF("new machine thread pid=%d tid=%d", m->system->pid, m->tid);
  return m;
}

void CollectGarbage(struct Machine *m) {
  long i;
  for (i = 0; i < m->freelist.n; ++i) {
    free(m->freelist.p[i]);
  }
  m->freelist.n = 0;
}

void FreeMachine(struct Machine *m) {
  bool orphan;
  struct System *s;
  if (m) {
    unassert((s = m->system));
    LOCK(&s->machines_lock);
    dll_remove(&s->machines, &m->elem);
    if (!(orphan = dll_is_empty(s->machines))) {
      unassert(!pthread_cond_signal(&s->machines_cond));
    }
    UNLOCK(&s->machines_lock);
    FreeMachineUnlocked(m);
    if (orphan) {
      FreeSystem(s);
    } else {
      THR_LOGF("more threads remain in operation");
    }
  }
}

u64 AllocatePage(struct System *s) {
  u8 *page;
  size_t i, n;
  intptr_t real;
  struct HostPage *h;
  LOCK(&g_allocator.lock);
  if ((h = g_allocator.pages)) {
    g_allocator.pages = h->next;
    UNLOCK(&g_allocator.lock);
    page = h->page;
    FreeHostPage(h);
    --s->memstat.freed;
    ++s->memstat.committed;
    ++s->memstat.reclaimed;
    goto Finished;
  } else {
    UNLOCK(&g_allocator.lock);
  }
  n = 64;
  page = (u8 *)AllocateBig(n * 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (!page) return -1;
  s->memstat.allocated += n;
  s->memstat.committed += 1;
  s->memstat.freed += n - 1;
  LOCK(&g_allocator.lock);
  for (i = n; i-- > 1;) {
    unassert((h = NewHostPage()));
    h->page = page + i * 4096;
    h->next = g_allocator.pages;
    g_allocator.pages = h;
  }
  UNLOCK(&g_allocator.lock);
Finished:
  ++s->rss;
  real = (intptr_t)page;
  unassert(!(real & ~PAGE_TA));
  return real | PAGE_HOST | PAGE_U | PAGE_RW | PAGE_V;
}

u64 AllocatePageTable(struct System *s) {
  u64 res;
  if ((res = AllocatePage(s)) != -1) {
    res &= ~PAGE_U;
    ++s->memstat.pagetables;
  }
  return res;
}

bool OverlapsPrecious(i64 virt, i64 size) {
  uint64_t BegA, EndA, BegB, EndB;
  if (size <= 0) return false;
  BegA = virt + kSkew;
  EndA = virt + kSkew + (size - 1);
  BegB = kPreciousStart;
  EndB = kPreciousEnd - 1;
  return MAX(BegA, BegB) < MIN(EndA, EndB);
}

bool IsValidAddrSize(i64 virt, i64 size) {
  return size > 0 &&                 //
         !(virt & 4095) &&           //
         virt >= -0x800000000000 &&  //
         virt < 0x800000000000 &&    //
         size <= 0x1000000000000 &&  //
         virt + size <= 0x800000000000;
}

void InvalidateSystem(struct System *s, bool tlb, bool icache) {
  struct Dll *e;
  struct Machine *m;
  LOCK(&s->machines_lock);
  for (e = dll_first(s->machines); e; e = dll_next(s->machines, e)) {
    m = MACHINE_CONTAINER(e);
    if (tlb) {
      atomic_store_explicit(&m->invalidated, true, memory_order_relaxed);
    }
    if (icache) {
      atomic_store_explicit(&m->opcache->invalidated, true,
                            memory_order_relaxed);
    }
  }
  UNLOCK(&s->machines_lock);
}

static void TallyFreePage(struct System *s, u64 entry) {
  if (entry & PAGE_RSRV) {
    --s->memstat.reserved;
  } else {
    --s->memstat.committed;
  }
}

static bool FreePage(struct System *s, u64 entry, u64 size,
                     bool *address_space_was_mutated, long *rss_delta) {
  u8 *page;
  long pagesize;
  intptr_t real, mug;
  unassert(entry & PAGE_V);
  if ((entry & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) == PAGE_HOST) {
    unassert(~entry & PAGE_RSRV);
    ++s->memstat.freed;
    --s->memstat.committed;
    ClearPage((page = (u8 *)(intptr_t)(entry & PAGE_TA)));
    FreeAnonymousPage(s, page);
    *address_space_was_mutated = true;
    --*rss_delta;
    return false;
  } else if ((entry & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
             (PAGE_HOST | PAGE_MAP | PAGE_MUG)) {
    TallyFreePage(s, entry);
    pagesize = GetSystemPageSize();
    real = entry & PAGE_TA;
    mug = ROUNDDOWN(real, pagesize);
    unassert(!Munmap((void *)mug, real - mug + size));
    *address_space_was_mutated = true;
    if (~entry & PAGE_RSRV) --*rss_delta;
    return false;
  } else if ((entry & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
             (PAGE_HOST | PAGE_MAP)) {
    TallyFreePage(s, entry);
    if (~entry & PAGE_RSRV) --*rss_delta;
    return true;
  } else if (entry & PAGE_RSRV) {
    TallyFreePage(s, entry);
    return false;
  } else {
    unassert((entry & PAGE_TA) < kRealSize);
    return false;
  }
}

static void AddPageToRanges(struct ContiguousMemoryRanges *ranges, i64 virt,
                            i64 end) {
  if (!(ranges->i && ranges->p[ranges->i - 1].b == virt)) {
    unassert(ranges->p = (struct ContiguousMemoryRange *)realloc(
                 ranges->p, ++ranges->i * sizeof(*ranges->p)));
    ranges->p[ranges->i - 1].a = virt;
  }
  ranges->p[ranges->i - 1].b = virt + MIN(4096, end - virt);
}

// removes page table entries. anonymous pages will be added to the
// system's free list. mug pages will be freed one by one. linear pages
// won't be freed, and will instead have their intervals pooled in the
// ranges data structure; the caller is responsible for freeing those.
static void RemoveVirtual(struct System *s, i64 virt, i64 size,
                          struct ContiguousMemoryRanges *ranges,
                          bool *address_space_was_mutated,  //
                          long *vss_delta, long *rss_delta) {
  u8 *mi;
  i64 end;
  u64 i, pt;
  for (end = virt + size; virt < end; virt += 1ull << i) {
    for (pt = s->cr3, i = 39;; i -= 9) {
      mi = GetPageAddress(s, pt) + ((virt >> i) & 511) * 8;
      pt = Load64(mi);
      if (!(pt & PAGE_V)) {
        break;
      } else if (i == 12) {
        if (FreePage(s, pt, MIN(4096, end - virt), address_space_was_mutated,
                     rss_delta) &&
            HasLinearMapping(m)) {
          AddPageToRanges(ranges, virt, end);
        }
        Store64(mi, 0);
        --*vss_delta;
        break;
      }
    }
  }
}

_Noreturn static void PanicDueToMmap(void) {
#ifndef NDEBUG
  WriteErrorString(
      "unrecoverable mmap() crisis: see log for further details\n");
#else
  WriteErrorString(
      "unrecoverable mmap() crisis: Blink was built with NDEBUG\n");
#endif
  exit(250);
}

int ReserveVirtual(struct System *s, i64 virt, i64 size, u64 flags, int fd,
                   i64 offset, bool shared) {
  u8 *mi;
  int prot;
  int method;
  void *got, *want;
  long i, pagesize;
  long vss_delta, rss_delta;
  bool no_retreat_no_surrender;
  i64 ti, pt, end, level, entry;
  struct ContiguousMemoryRanges ranges;

  // we determine these
  unassert(!(flags & PAGE_TA));
  unassert(!(flags & PAGE_MAP));
  unassert(!(flags & PAGE_HOST));
  unassert(!(flags & PAGE_RSRV));
  unassert(s->mode == XED_MODE_LONG);

  if (!IsValidAddrSize(virt, size)) {
    LOGF("mmap(addr=%#" PRIx64 ", size=%#" PRIx64 ") is not a legal mapping",
         virt, size);
    return einval();
  }

  if (HasLinearMapping(s) && OverlapsPrecious(virt, size)) {
    LOGF("mmap(addr=%#" PRIx64 ", size=%#" PRIx64
         ") overlaps memory blink reserves for itself",
         virt, size);
    return enomem();
  }

  if (fd != -1 && (offset & 4095)) {
    LOGF("mmap(offset=%#" PRIx64 ") isn't 4096-byte page aligned", offset);
    return einval();
  }

  pagesize = GetSystemPageSize();

  if (HasLinearMapping(s)) {
    if (virt <= 0) {
      LOGF("app attempted to map %#" PRIx64 " in linear mode", virt);
      return enotsup();
    }
    if (virt & (pagesize - 1)) {
      LOGF("app chose mmap %s (%#" PRIx64 ") that's not aligned "
           "to the platform page size (%#lx) while using linear mode",
           "address (try using `blink -m`)", virt, pagesize);
      return einval();
    }
    if (offset & (pagesize - 1)) {
      LOGF("app chose mmap %s (%#" PRIx64 ") that's not aligned "
           "to the platform page size (%#lx) while using linear mode",
           "file offset (try using `blink -m`)", offset, pagesize);
      return einval();
    }
  }

  MEM_LOGF("reserving virtual [%#" PRIx64 ",%#" PRIx64 ") w/ %" PRId64 " kb",
           virt, virt + size, size / 1024);

  // remove existing mapping
  // this may be the point of no return
  vss_delta = 0;
  rss_delta = 0;
  no_retreat_no_surrender = false;
  memset(&ranges, 0, sizeof(ranges));
  RemoveVirtual(s, virt, size, &ranges, &no_retreat_no_surrender, &vss_delta,
                &rss_delta);
  if (HasLinearMapping(m) && ranges.i) {
    // linear mappings exist within the requested interval
    if (ranges.i == 1 &&          //
        ranges.p[0].a == virt &&  //
        ranges.p[0].b == virt + size) {
      // it should be 100% safe to let the kernel blow it away
      method = MAP_FIXED;
    } else {
      // holes exist; try to create a greenfield
      for (i = 0; i < ranges.i; ++i) {
        Munmap(ToHost(ranges.p[i].a), ranges.p[i].b - ranges.p[i].a);
        no_retreat_no_surrender = true;
      }
      // errors in Munmap() should propagate to Mmap() below
      method = MAP_DEMAND;
    }
    free(ranges.p);
  } else {
    // requested interval should be a greenfield
    method = MAP_DEMAND;
  }

  prot = ((flags & PAGE_U ? PROT_READ : 0) |
          ((flags & PAGE_RW) || fd == -1 ? PROT_WRITE : 0));

  if (HasLinearMapping(s)) {
    // create a linear mapping. doing this runs the risk of destroying
    // things the kernel put into our address space that blink doesn't
    // know about. systems like linux and freebsd have a feature which
    // lets us report a friendly error to the user, when that happens.
    // the solution is most likely to rebuild with -Wl,-Ttext-segment=
    // please note we need to take off the seatbelt after an execve().
    errno = 0;
    want = ToHost(virt);
    if ((got = Mmap(want, size, prot,                       //
                    (method |                               //
                     (fd == -1 ? MAP_ANONYMOUS : 0) |       //
                     (shared ? MAP_SHARED : MAP_PRIVATE)),  //
                    fd, offset, "linear")) != want) {
      if (got == MAP_FAILED && errno == ENOMEM && !no_retreat_no_surrender) {
        LOGF("host system returned ENOMEM");
        return -1;
      }
      ERRF("mmap(%#" PRIx64 "[%p], %#" PRIx64 ")"
           " -> %#" PRIx64 "[%p] crisis: %s",
           virt, want, size, ToGuest(got), got,
           (method == MAP_DEMAND && errno == MAP_DENIED)
               ? "requested memory overlapped blink image or system memory. "
                 "try using `blink -m` to disable memory optimizations, or "
                 "try compiling blink using -Wl,--image-base=0x23000000 or "
                 "possibly -Wl,-Ttext-segment=0x23000000 in LDFLAGS"
               : DescribeHostErrno(errno));
      PanicDueToMmap();
    }
    s->memstat.allocated += size / 4096;
    s->memstat.committed += size / 4096;
    flags |= PAGE_HOST | PAGE_MAP;
  } else if (fd != -1 || shared) {
    flags |= PAGE_HOST | PAGE_MAP | PAGE_MUG;
    s->memstat.reserved += size / 4096;
  } else {
    s->memstat.reserved += size / 4096;
  }

  // account for pre-existing memory that was just removed
  s->vss += vss_delta;
  s->rss += rss_delta;
  s->memchurn += -vss_delta;
  // TODO(jart): Figure out what's wrong with rss accounting.
  if (s->vss < 0) s->vss = 0;
  if (s->rss < 0) s->rss = 0;

  // add pml4t entries ensuring intermediary tables exist
  for (end = virt + size;;) {
    for (pt = s->cr3, level = 39; level >= 12; level -= 9) {
      ti = (virt >> level) & 511;
      mi = GetPageAddress(s, pt) + ti * 8;
      pt = Load64(mi);
      if (level > 12) {
        if (!(pt & PAGE_V)) {
          if ((pt = AllocatePageTable(s)) == -1) {
            WriteErrorString("mmap() crisis: ran out of page table memory\n");
            exit(250);
          }
          Store64(mi, pt);
        }
        continue;
      }
      for (;;) {
        unassert(~pt & PAGE_V);
        intptr_t real;
        if (flags & PAGE_MAP) {
          if (flags & PAGE_MUG) {
            void *mug;
            off_t mugoff;
            int mugflags;
            long mugsize;
            long mugskew;
            mugsize = MIN(4096, end - virt);
            if (fd != -1) {
              mugskew = offset - ROUNDDOWN(offset, pagesize);
              mugoff = ROUNDDOWN(offset, pagesize);
              mugsize += mugskew;
            } else {
              mugoff = 0;
              mugskew = 0;
            }
            mugflags = (shared ? MAP_SHARED : MAP_PRIVATE) |
                       (fd == -1 ? MAP_ANONYMOUS : 0);
            mug = AllocateBig(mugsize, prot, mugflags, fd, mugoff);
            if (!mug) {
              ERRF("mmap(virt=%" PRIx64
                   ", brk=%p size=%ld, flags=%#x, fd=%d, offset=%#" PRIx64
                   ") crisis: %s",
                   virt, g_allocator.brk, mugsize, mugflags, fd, (u64)mugoff,
                   DescribeHostErrno(errno));
              PanicDueToMmap();
            }
            real = (intptr_t)mug + mugskew;
            offset += 4096;
          } else {
            real = (intptr_t)ToHost(virt);
          }
          unassert(!(real & ~PAGE_TA));
          entry = real | flags | PAGE_V;
        } else {
          entry = flags | PAGE_V;
        }
        ++s->vss;
        if (HasLinearMapping(s)) {
          ++s->rss;
        } else {
          entry |= PAGE_RSRV;
        }
        if (fd != -1 && virt + 4096 >= end) {
          entry |= PAGE_EOF;
        }
        Store64(mi, entry);
        if ((virt += 4096) >= end) {
          return 0;
        }
        if (++ti == 512) break;
        pt = Load64((mi += 8));
      }
    }
  }
}

i64 FindVirtual(struct System *s, i64 virt, i64 size) {
  u64 i, pt, got, orig_virt = virt;
  (void)orig_virt;
StartOver:
  if (!IsValidAddrSize(virt, size)) {
    LOGF("FindVirtual [%#" PRIx64 ",%#" PRIx64 ") -> "
         "[%#" PRIx64 ",%#" PRIx64 ") not possible",
         orig_virt, orig_virt + size, virt, virt + size);
    return enomem();
  }
  if (HasLinearMapping(s) && OverlapsPrecious(virt, size)) {
    virt = kPreciousEnd + kSkew;
  }
  got = 0;
  do {
    for (i = 39, pt = s->cr3;; i -= 9) {
      pt = Load64(GetPageAddress(s, pt) + (((virt + got) >> i) & 511) * 8);
      if (i == 12 || !(pt & PAGE_V)) break;
    }
    got += 1ull << i;
    if ((pt & PAGE_V)) {
      virt += got;
      goto StartOver;
    }
  } while (got < size);
  return virt;
}

int FreeVirtual(struct System *s, i64 virt, i64 size) {
  int rc;
  long i;
  bool mutated;
  long vss_delta, rss_delta;
  struct ContiguousMemoryRanges ranges;
  MEM_LOGF("freeing virtual [%#" PRIx64 ",%#" PRIx64 ") w/ %" PRId64 " kb",
           virt, virt + size, size / 1024);
  if (!IsValidAddrSize(virt, size)) {
    LOGF("invalid addr size");
    return einval();
  }
  // TODO(jart): We should probably validate a PAGE_EOF exists at the
  //             end when size isn't a multiple of platform page size
  vss_delta = 0;
  rss_delta = 0;
  memset(&ranges, 0, sizeof(ranges));
  RemoveVirtual(s, virt, size, &ranges, &mutated, &vss_delta, &rss_delta);
  for (rc = i = 0; i < ranges.i; ++i) {
    if (Munmap(ToHost(ranges.p[i].a), ranges.p[i].b - ranges.p[i].a)) {
      LOGF("failed to %s subrange"
           " [%" PRIx64 ",%" PRIx64 ") within requested range"
           " [%" PRIx64 ",%" PRIx64 "): %s",
           "munmap", ranges.p[i].a, ranges.p[i].b, virt, virt + size,
           DescribeHostErrno(errno));
      rc = einval();
    }
  }
  free(ranges.p);
  s->vss += vss_delta;
  s->rss += rss_delta;
  s->memchurn += -vss_delta;
  // TODO(jart): Figure out what's wrong with rss accounting.
  if (s->vss < 0) s->vss = 0;
  if (s->rss < 0) s->rss = 0;
  InvalidateSystem(s, true, false);
  return rc;
}

int GetProtection(u64 key) {
  int prot = 0;
  if (key & PAGE_U) prot |= PROT_READ;
  if (key & PAGE_RW) prot |= PROT_WRITE;
  if (~key & PAGE_XD) prot |= PROT_EXEC;
  return prot;
}

u64 SetProtection(int prot) {
  u64 key = 0;
  if (prot & PROT_READ) key |= PAGE_U;
  if (prot & PROT_WRITE) key |= PAGE_RW;
  if (~prot & PROT_EXEC) key |= PAGE_XD;
  return key;
}

bool IsFullyMapped(struct System *s, i64 virt, i64 size) {
  u8 *mi;
  u64 pt;
  i64 ti, end, level;
  for (end = virt + size;;) {
    for (pt = s->cr3, level = 39; level >= 12; level -= 9) {
      ti = (virt >> level) & 511;
      mi = GetPageAddress(s, pt) + ti * 8;
      pt = Get64(mi);
      if (level > 12) {
        if (!(pt & PAGE_V)) {
          return false;
        }
        continue;
      }
      for (;;) {
        if (!(pt & PAGE_V)) {
          return false;
        }
        if ((virt += 4096) >= end) {
          return true;
        }
        if (++ti == 512) break;
        pt = Get64((mi += 8));
      }
    }
  }
}

bool IsFullyUnmapped(struct System *s, i64 virt, i64 size) {
  u8 *mi;
  i64 end;
  u64 i, pt;
  if (HasLinearMapping(s) && OverlapsPrecious(virt, size)) return false;
  for (end = virt + size; virt < end; virt += 1ull << i) {
    for (pt = s->cr3, i = 39;; i -= 9) {
      mi = GetPageAddress(s, pt) + ((virt >> i) & 511) * 8;
      pt = Load64(mi);
      if (!(pt & PAGE_V)) {
        break;
      } else if (i == 12) {
        return false;
      }
    }
  }
  return true;
}

int ProtectVirtual(struct System *s, i64 virt, i64 size, int prot) {
  int rc;
  int sysprot;
  u64 pt, key;
  u8 *mi, *real;
  long i, pagesize;
  i64 ti, end, level, orig_virt;
  struct ContiguousMemoryRanges ranges;
  orig_virt = virt;
  (void)orig_virt;
  pagesize = GetSystemPageSize();
  if (!IsValidAddrSize(virt, size)) {
    return einval();
  }
  if (!IsFullyMapped(s, virt, size)) {
    LOGF("mprotect(%#" PRIx64 ", %#" PRIx64 ") interval has unmapped pages",
         virt, size);
    return enomem();
  }
  key = SetProtection(prot);
  // some operating systems e.g. openbsd and apple M1, have a
  // W^X invariant. we don't need to execute guest memory so:
  sysprot = prot & ~PROT_EXEC;
  // in linear mode, the guest might try to do something like
  // set a 4096 byte guard page to PROT_NONE at the bottom of
  // its 64kb stack. if the host operating system has a 64 kb
  // page size, then that would be bad. we can't satisfy prot
  // unless the guest takes the page size into consideration.
  if (HasLinearMapping(s) &&
      ((virt & (pagesize - 1)) && (size & (pagesize - 1)))) {
    sysprot = PROT_READ | PROT_WRITE;
  }
  memset(&ranges, 0, sizeof(ranges));
  for (rc = 0, end = virt + size;;) {
    for (pt = s->cr3, level = 39; level >= 12; level -= 9) {
      ti = (virt >> level) & 511;
      mi = GetPageAddress(s, pt) + ti * 8;
      pt = Get64(mi);
      if (level > 12) {
        unassert(pt & PAGE_V);
        continue;
      }
      for (;;) {
        unassert(pt & PAGE_V);
        if (HasLinearMapping(s) && (pt & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
                                       (PAGE_HOST | PAGE_MAP)) {
          AddPageToRanges(&ranges, virt, end);
        } else if ((pt & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
                   (PAGE_HOST | PAGE_MAP | PAGE_MUG)) {
          real = (u8 *)ROUNDDOWN((intptr_t)(pt & PAGE_TA), pagesize);
          if (Mprotect(real, pagesize, sysprot, "mug")) {
            LOGF("mprotect(pt=%#" PRIx64
                 ", real=%p, size=%#lx, prot=%d) failed: %s",
                 pt, real, pagesize, prot, DescribeHostErrno(errno));
            rc = -1;
          }
        }
        pt &= ~(PAGE_U | PAGE_RW | PAGE_XD);
        pt |= key;
        Put64(mi, pt);
        if ((virt += 4096) >= end) {
          goto FinishedCrawling;
        }
        if (++ti == 512) break;
        pt = Get64((mi += 8));
      }
    }
  }
FinishedCrawling:
  if (HasLinearMapping(s)) {
    for (i = 0; i < ranges.i; ++i) {
      if (ranges.p[i].a & (pagesize - 1)) {
        LOGF("failed to %s subrange"
             " [%" PRIx64 ",%" PRIx64 ") within requested range"
             " [%" PRIx64 ",%" PRIx64 "): %s",
             "mprotect", ranges.p[i].a, ranges.p[i].b, orig_virt,
             orig_virt + size, "HOST_PAGE_MISALIGN");
      } else if (Mprotect(ToHost(ranges.p[i].a), ranges.p[i].b - ranges.p[i].a,
                          sysprot, "linear")) {
        LOGF("failed to %s subrange"
             " [%" PRIx64 ",%" PRIx64 ") within requested range"
             " [%" PRIx64 ",%" PRIx64 "): %s",
             "mprotect", ranges.p[i].a, ranges.p[i].b, orig_virt,
             orig_virt + size, DescribeHostErrno(errno));
        rc = -1;
      }
    }
    free(ranges.p);
  }
  InvalidateSystem(s, true, false);
  return rc;
}

int SyncVirtual(struct System *s, i64 virt, i64 size, int sysflags) {
  int rc;
  u8 *mi;
  u64 pt;
  i64 orig_virt;
  i64 ti, end, level;
  long i, skew, pagesize;
  struct ContiguousMemoryRanges ranges;
  if (!IsValidAddrSize(virt, size)) {
    return einval();
  }
  orig_virt = virt;
  (void)orig_virt;
  pagesize = GetSystemPageSize();
  if (HasLinearMapping(s) && (skew = virt & (pagesize - 1))) {
    size += skew;
    virt -= skew;
  }
  if (!IsFullyMapped(s, virt, size)) {
    LOGF("mprotect(%#" PRIx64 ", %#" PRIx64 ") interval has unmapped pages",
         virt, size);
    return enomem();
  }
  memset(&ranges, 0, sizeof(ranges));
  for (rc = 0, end = virt + size;;) {
    for (pt = s->cr3, level = 39; level >= 12; level -= 9) {
      ti = (virt >> level) & 511;
      mi = GetPageAddress(s, pt) + ti * 8;
      pt = Get64(mi);
      if (level > 12) {
        unassert(pt & PAGE_V);
        continue;
      }
      for (;;) {
        unassert(pt & PAGE_V);
        if (HasLinearMapping(s) && (pt & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
                                       (PAGE_HOST | PAGE_MAP)) {
          AddPageToRanges(&ranges, virt, end);
        } else if ((pt & (PAGE_HOST | PAGE_MAP | PAGE_MUG)) ==
                   (PAGE_HOST | PAGE_MAP | PAGE_MUG)) {
          intptr_t real = pt & PAGE_TA;
          intptr_t page = ROUNDDOWN(real, pagesize);
          long lilsize = (real - page) + MIN(4096, end - virt);
          if (Msync((void *)page, lilsize, sysflags, "mug")) {
            LOGF("msync(%p [pt=%#" PRIx64
                 "], size=%#lx, flags=%d) failed: %s\n%s",
                 (void *)page, pt, pagesize, sysflags, DescribeHostErrno(errno),
                 FormatPml4t(g_machine));
            rc = -1;
          }
        }
        if ((virt += 4096) >= end) {
          goto FinishedCrawling;
        }
        if (++ti == 512) break;
        pt = Get64((mi += 8));
      }
    }
  }
FinishedCrawling:
  if (HasLinearMapping(s)) {
    for (i = 0; i < ranges.i; ++i) {
      if (Msync(ToHost(ranges.p[i].a), ranges.p[i].b - ranges.p[i].a, sysflags,
                "linear")) {
        LOGF("failed to %s subrange"
             " [%" PRIx64 ",%" PRIx64 ") within requested range"
             " [%" PRIx64 ",%" PRIx64 "): %s",
             "msync", ranges.p[i].a, ranges.p[i].b, orig_virt, orig_virt + size,
             DescribeHostErrno(errno));
        rc = -1;
      }
    }
    free(ranges.p);
  }
  return rc;
}
