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
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "blink/assert.h"
#include "blink/endian.h"
#include "blink/errno.h"
#include "blink/fds.h"
#include "blink/lock.h"
#include "blink/log.h"
#include "blink/syscall.h"

int SysPipe2(struct Machine *m, i64 pipefds_addr, i32 flags) {
  int rc;
  int fds[2];
  int oflags;
  int supported;
  u8 fds_linux[2][4];
  supported = O_CLOEXEC_LINUX | O_NDELAY_LINUX;
  if (flags & ~supported) {
    LOGF("%s() unsupported flags: %d", "pipe2", flags & ~supported);
    return einval();
  }
  if (!IsValidMemory(m, pipefds_addr, sizeof(fds_linux), PROT_WRITE)) {
    return efault();
  }
  if (flags) LOCK(&m->system->exec_lock);
  if (pipe(fds) != -1) {
    oflags = 0;
    if (flags & O_CLOEXEC_LINUX) {
      oflags |= O_CLOEXEC;
      unassert(!fcntl(fds[0], F_SETFD, FD_CLOEXEC));
      unassert(!fcntl(fds[1], F_SETFD, FD_CLOEXEC));
    }
    if (flags & O_NDELAY_LINUX) {
      oflags |= O_NDELAY;
      unassert(!fcntl(fds[0], F_SETFL, O_NDELAY));
      unassert(!fcntl(fds[1], F_SETFL, O_NDELAY));
    }
    LOCK(&m->system->fds.lock);
    unassert(AddFd(&m->system->fds, fds[0], O_RDONLY | oflags));
    unassert(AddFd(&m->system->fds, fds[1], O_WRONLY | oflags));
    UNLOCK(&m->system->fds.lock);
    Write32(fds_linux[0], fds[0]);
    Write32(fds_linux[1], fds[1]);
    unassert(!CopyToUserWrite(m, pipefds_addr, fds_linux, sizeof(fds_linux)));
    rc = 0;
  } else {
    rc = -1;
  }
  if (flags) UNLOCK(&m->system->exec_lock);
  return rc;
}
