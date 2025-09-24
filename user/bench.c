// user/bench.c — pipe 分配/读写基准（xv6-riscv）
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static int nowticks(void) {
  return uptime();   // 1 tick = 10ms（100Hz）
}

static void bench_pipe_alloc(int iters) {
  int i, fds[2];
  int t0 = nowticks();
  for (i = 0; i < iters; i++) {
    if (pipe(fds) < 0) {
      printf("bench: pipe failed at %d\n", i);
      exit(1);
    }
    close(fds[0]);
    close(fds[1]);
  }
  int dt = nowticks() - t0;
  if (dt == 0) dt = 1;
  int opsps = (iters * 100) / dt; // 100 ticks/s
  printf("[bench] pipe_alloc: iters=%d ticks=%d ops/s=%d\n", iters, dt, opsps);
}

static void bench_pipe_io(int iters, int msize) {
  if (msize <= 0 || msize > 512) msize = 64; // PIPESIZE=512
  char buf[512];
  for (int j = 0; j < msize; j++) buf[j] = (char)j;

  int i, fds[2];
  int t0 = nowticks();
  for (i = 0; i < iters; i++) {
    if (pipe(fds) < 0) {
      printf("bench: pipe failed\n");
      exit(1);
    }
    if (write(fds[1], buf, msize) != msize) {
      printf("bench: write failed\n");
      exit(1);
    }
    if (read(fds[0], buf, msize) != msize) {
      printf("bench: read failed\n");
      exit(1);
    }
    close(fds[0]);
    close(fds[1]);
  }
  int dt = nowticks() - t0;
  if (dt == 0) dt = 1;
  int opsps = (iters * 100) / dt;
  int bps   = (iters * msize * 100) / dt; // bytes/s
  printf("[bench] pipe_io: iters=%d msize=%d ticks=%d ops/s=%d bytes/s=%d\n",
         iters, msize, dt, opsps, bps);
}

static void usage(void) {
  printf("usage: bench [-p procs] [-n iters] [-io] [-msize bytes]\n");
  exit(1);
}

int
main(int argc, char **argv)
{
  int procs = 1;
  int iters = 5000;
  int do_io = 0;
  int msize = 64;

  // 简陋参数解析
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
      procs = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) {
      iters = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-io") == 0) {
      do_io = 1;
    } else if (strcmp(argv[i], "-msize") == 0 && i+1 < argc) {
      msize = atoi(argv[++i]);
    } else {
      usage();
    }
  }
  if (procs < 1) procs = 1;
  if (iters < procs) iters = procs;

  // 均分给子进程
  int per = iters / procs;
  int rem = iters % procs;

  int t0 = nowticks();

  for (int i = 0; i < procs; i++) {
    int thisn = per + (i < rem ? 1 : 0);
    int pid = fork();
    if (pid < 0) {
      printf("bench: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      if (do_io) bench_pipe_io(thisn, msize);
      else       bench_pipe_alloc(thisn);
      exit(0);
    }
  }

  // 等待全部子进程结束
  while (wait(0) > 0) { }

  int dt = nowticks() - t0;
  if (dt == 0) dt = 1;
  int opsps = (iters * 100) / dt;
  printf("[bench] total: procs=%d iters=%d ticks=%d ops/s=%d\n",
         procs, iters, dt, opsps);
  exit(0);
}
