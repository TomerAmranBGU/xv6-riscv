#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "bsem.h"
extern void *sigretStart;
extern void *sigretEnd;
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct bsem bsem[MAX_BSEM];
struct spinlock bsem_lock;

struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
struct spinlock pid_lock;
struct spinlock tid_lock;

extern char trampoline[]; // trampoline.S
extern void *call_start;
extern void *call_end;
extern void forkret(void);
static void freeproc(struct proc *p);
static void freethread(struct thread *); // freethread

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void procinit(void)
{
  struct proc *p;
  struct thread *t;
  initlock(&pid_lock, "nextpid");
  initlock(&tid_lock, "tidlock");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    for (t = p->threads; t < &p->threads[NTHREADS]; t++)
    {
      initlock(&t->lock, "thread");
    }
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// ADDED: mythread
struct thread *
mythread(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct thread *cur_thread = c->thread;
  pop_off();
  return cur_thread;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// ADDED: allocating thread id
int alloctid()
{
  int tid;
  acquire(&tid_lock);
  tid = nexttid;
  nexttid = nexttid + 1;
  release(&tid_lock);
  return tid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
// ADDED: overhauled the allocproc function with threads.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->exiting_from_the_system = 0;
  p->killed = 0;
  for (int i = 0; i < 32; i++)
  {
    p->sighandlers[i] = (void *)SIG_DFL;
    p->sighandlers_mask[i] = 0;
  }

  // Create the main thread
  struct thread *t = &p->threads[0];
  // Allocate a trapframe page that will hold all the threads trap frames an the backup trapframe.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  t->trapframe = p->trapframe;

  t->cid = 0;
  t->tid = alloctid();
  t->state = T_USED;

  // Allocate a trapframe_backup page.
  p->trapframe_backup = t->trapframe + NTHREADS;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  if ((t->kstack = (uint64)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  t->context.sp = t->kstack + PGSIZE;

  t->parent = p;

  // Set main thread field
  p->main_thread = t;

  //we need to ignoring sigcont signal at the start
  p->sighandlers[SIGCONT] = (void *)SIG_IGN;

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  return p;
}

static struct thread *
allocthread(struct proc *p)
{
  int thread_index = 0;
  struct thread *t;
  for (t = p->threads; t < &p->threads[NTHREADS]; t++)
  {
    if (t == mythread())
    {
      thread_index++;
      continue;
    }
    acquire(&t->lock);
    if (t->state == T_UNUSED)
    {
      goto found;
    }
    else
    {
      release(&t->lock);
    }
    thread_index++; // updated for set the field cid
  }
  return 0;

found:
  t->cid = thread_index;
  t->tid = alloctid();
  t->chan = 0;
  t->state = T_USED;
  t->trapframe = p->trapframe + thread_index;
  t->parent = p;
  t->killed = 0;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&t->context, 0, sizeof(t->context));
  t->context.ra = (uint64)forkret;
  if ((t->kstack = (uint64)kalloc()) == 0)
  {
    freethread(t);
    release(&t->lock);
    return 0;
  }
  t->context.sp = t->kstack + PGSIZE;
  return t;
}

int kthread_create(uint64 start_func, uint64 stack)
{
  struct proc *p = myproc();
  struct thread *kt;
  acquire(&p->lock);
  if (p->exiting_from_the_system)
  {
    release(&p->lock);
    return -1;
  }
  release(&p->lock);

  if ((kt = allocthread(p)) == 0)
  {
    return -1;
  }
  *kt->trapframe = *mythread()->trapframe;
  kt->trapframe->epc = start_func;
  kt->trapframe->sp = stack + 4000;
  kt->state = T_RUNNABLE;

  release(&kt->lock);
  return kt->tid;
}

struct thread *find_thread()
{
  struct proc *p = myproc();
  for (struct thread *t = p->threads; t < &p->threads[NTHREADS]; t++)
  {
    if (t != p->main_thread)
    {
      acquire(&t->lock);
      if (t->state != T_UNUSED && t->state != T_ZOMBIE)
      {
        release(&t->lock);
        return t;
      }
      release(&t->lock);
    }
  }
  return 0;
}

void kthread_exit(int status)
{
  struct thread *t = mythread();
  struct proc *p = myproc();
  acquire(&p->lock);
  acquire(&t->lock);
  if (t == p->main_thread && (p->main_thread = find_thread()) == 0)
  {
    p->main_thread = p->threads;
    p->state = ZOMBIE;
    release(&t->lock);
    release(&p->lock);
    exit(status);
  }
  release(&t->lock);
  struct thread *tr;
  for (tr = p->threads; tr < &p->threads[NTHREADS]; tr++)
  {
    acquire(&tr->lock);
    if (tr->state == T_SLEEPING && tr->chan == t)
    {
      tr->state = T_RUNNABLE;
      release(&tr->lock);
      break;
    }
    release(&tr->lock);
  }
  t->xstate = status;
  t->state = T_ZOMBIE;
  acquire(&t->lock);
  release(&p->lock);
  sched();
  panic(" need to exit and for my inner peace");
}

// a new function that frees the thread sent as an argument
static void
freethread(struct thread *t)
{
  if (t->kstack)
    kfree((void *)t->kstack);
  t->kstack = 0;
  t->trapframe = 0;
  t->cid = 0;
  t->chan = 0;
  t->killed = 0;
  t->chan = 0;
  t->parent = 0;
  t->tid = 0;
  t->state = T_UNUSED;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  // Free all threads to stay on the safe side.
  for (struct thread *t = p->threads; t < &p->threads[NTHREADS]; t++)
    freethread(t);

  // ADDED: freeing trapframe page
  if (p->trapframe)
    kfree(p->trapframe);
  p->trapframe = 0;
  p->trapframe_backup = 0;

  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;

  p->exiting_from_the_system = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->xstate = 0;
  for (int i = 0; i < 32; i++)
  {
    p->sighandlers_mask[i] = 0;
    p->sighandlers[i] = (void *)SIG_DFL;
  }

  p->stopped_non_zero = 0;
  p->handling_signal = 0;
  p->pending_signals = 0;
  p->signal_mask = 0;
  p->killed = 0;
  p->signal_mask_backup = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }
  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME(0), PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
//ADDED: support modifying the TRAPFRAME macro
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME(0), 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{

  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->main_thread->trapframe->epc = 0;     // user program counter
  p->main_thread->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->main_thread->state = T_RUNNABLE; // change in the code
  p->state = RUNNABLE;
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  acquire(&p->lock);
  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0)
    {
      release(&p->lock);
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  release(&p->lock);
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.

// ADDED: overhauled the fork syscall to support thread
int fork(void)
{
  int signal, pid;
  struct proc *np;
  struct thread *t = mythread();
  struct proc *p = myproc();

  // Allocate process (and its main thread)
  if ((np = allocproc()) == 0)
  {
    return -1;
  }
  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->main_thread->trapframe) = *(t->trapframe);

  // Cause fork to return 0 in the child.
  np->main_thread->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (signal = 0; signal < NOFILE; signal++)
    if (p->ofile[signal])
      np->ofile[signal] = filedup(p->ofile[signal]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  np->signal_mask = p->signal_mask;
  memmove(np->sighandlers, p->sighandlers, sizeof(p->sighandlers));
  memmove(np->sighandlers_mask, p->sighandlers_mask, sizeof(p->sighandlers_mask));

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->main_thread->lock);
  np->main_thread->state = T_RUNNABLE;
  np->state = RUNNABLE;
  release(&np->main_thread->lock);
  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

void exit(int status)
{
  struct proc *p = myproc();
  struct thread *t = mythread();
  // printf("exit: process %d with status %d\n", p->pid, status);
  if (p == initproc)
    panic("init exiting");
  acquire(&p->lock);
  // printf("exit: acquired process lock\n");
  if (!p->exiting_from_the_system)
  {
    // printf("exit: exiting from the system\n");
    p->exiting_from_the_system = 1;
    release(&p->lock);
  }
  else
  {
    release(&p->lock);
    kthread_exit(-1);
  }
  for (struct thread *t_iterator = p->threads; t_iterator < &p->threads[NTHREADS]; t_iterator++)
  {
    if (t_iterator == t)
      continue;
    acquire(&t_iterator->lock);
    t_iterator->killed = 1;
    if (t_iterator->state == T_SLEEPING)
      t_iterator->state = T_RUNNABLE;
    release(&t_iterator->lock);
  }
  for (struct thread *t_iterator = p->threads; t_iterator < &p->threads[NTHREADS]; t_iterator++)
  {
    if (t_iterator->tid == t->tid)
      continue;
    kthread_join(t_iterator->tid, 0);
  }
  p->main_thread = t;
  // printf("exit: original exit begin\n");
  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);
  acquire(&t->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  t->xstate = status;
  t->state = T_ZOMBIE;
  release(&p->lock);
  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np;
  int have_kids_in_proc;
  struct proc *p = myproc();
  int pid;
  struct thread *t = mythread();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    have_kids_in_proc = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        acquire(&np->main_thread->lock);
        have_kids_in_proc = 1;
        if (np->state == ZOMBIE && np->main_thread->state == T_ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->main_thread->lock);
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->main_thread->lock);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->main_thread->lock);
        release(&np->lock);
      }
    }

    if (!have_kids_in_proc || p->killed || t->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);
  }
}

// kthread join.
int kthread_join(int thread_id, uint64 status)
{
  struct thread *t = mythread();
  struct proc *p = myproc();
  struct thread *target = 0;
  if (thread_id <= 0)
  {
    return -1;
  }

  acquire(&p->lock);
  for (struct thread *t_iter = p->threads; t_iter < &p->threads[NTHREADS]; t_iter++)
  {
    if (t_iter == t)
      continue;
    acquire(&t_iter->lock);
    if (t_iter->tid == thread_id)
    {
      target = t_iter;

      release(&target->lock);
      break;
    }
    release(&t_iter->lock);
  }
  if (target == 0)
  {

    release(&p->lock);
    return -1;
  }
  while (!t->killed && target->tid == thread_id && target->state != T_ZOMBIE && target->state != T_UNUSED)
  {
    sleep(target, &p->lock);
  }
  acquire(&target->lock);
  release(&p->lock);

  if (t->killed)
  {
    release(&target->lock);
    return -1;
  }
  if (target->tid == thread_id && target->state == T_ZOMBIE)
  {
    if (status != 0 && copyout(p->pagetable, status, (char *)&target->xstate, sizeof(int)) < 0)
    {
      release(&target->lock);
      return -1;
    }
    freethread(target);
  }
  release(&target->lock);

  return 0;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

// ADDED: changed the scheduler run over threads and not processes
void scheduler(void)
{
  struct proc *p;
  struct thread *t;
  struct cpu *c = mycpu();
  c->thread = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state != RUNNABLE)
      {
        release(&p->lock);
        continue;
      }
      release(&p->lock);

      // TODO: check if we could lock the process here instead of down there.
      for (t = p->threads; t < &p->threads[NTHREADS]; t++)
      {

        acquire(&t->lock);
        if (t->state == T_RUNNABLE)
        {
          // Switch to chosen thread.  It is the threads's job
          // to release its lock and then reacquire it
          // before jumping back to us.

          t->state = T_RUNNING;
          c->thread = t;
          c->proc = p;

          swtch(&c->context, &t->context);
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->thread = 0;
          c->proc = 0;
        }
        release(&t->lock);
      }
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
// ADDED: changed to dealin with thread instead of process
void sched(void)
{
  int intena;
  struct thread *t = mythread();
  if (!holding(&t->lock))
  {
    panic("sched t->lock");
  }
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (t->state == T_RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&t->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
//ADDED: changed yield with mythread
void yield(void)
{
  struct thread *t = mythread();
  acquire(&t->lock);
  t->state = T_RUNNABLE;
  sched();
  release(&t->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  // ADDED: mythread instead of myproc
  release(&mythread()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
//ADDED: changed to support threads.
void sleep(void *chan, struct spinlock *lk)
{
  struct thread *t = mythread();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&t->lock); //DOC: sleeplock1
  release(lk);
  // Go to sleep.
  t->chan = chan;
  t->state = T_SLEEPING;

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  release(&t->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
//ADDED: waking up all threads.
void wakeup(void *chan)
{
  struct proc *p;
  struct thread *t;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    for (t = p->threads; t < &p->threads[NTHREADS]; t++)
    {
      acquire(&t->lock);
      if (t->state == T_SLEEPING && t->chan == chan)
      {
        t->state = T_RUNNABLE;
      }
      release(&t->lock);
    }
    release(&p->lock);
  }
}
//arg1: pid, arg2: signum
//sends signal signum to proccess pid
int kill(int pid, int signum)
{
  if (signum < 0 || signum > 31)
    return -1;
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      printf("pid: %d got signal: %d\n", pid, signum);
      p->pending_signals |= 1 << signum;
      return 0;
    }
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
// TODO: consider giving an f about this.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "sleep ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// ADDED: sigprocmask system call
uint sigprocmask(uint sigmask)
{
  if (sigmask & 1 << SIGKILL)
  {
    printf("you are not allowed to block SIGKILL!\n");
    return -1;
  };
  if (sigmask & 1 << SIGSTOP)
  {
    printf("you are not allowed to block SIGSTOP!\n");
    return -1;
  };

  struct proc *p = myproc();
  uint prev_signalmask;
  acquire(&p->lock);
  prev_signalmask = p->signal_mask;
  p->signal_mask = sigmask;
  release(&p->lock);
  return prev_signalmask;
}

// ADDED: sigaction system call
int sigaction(int signum, uint64 act, uint64 oldact)
{
  printf("sigaction\n");
  struct proc *p = myproc();
  struct sigaction_ new, old;
  acquire(&p->lock);
  if (signum < 0 || signum > (32 - 1) || signum == SIGKILL || signum == SIGSTOP)
  {
    release(&p->lock);
    return -1;
  }
  if (oldact != 0)
  {
    old.sa_handler_ = p->sighandlers[signum];
    old.sigmask = p->sighandlers_mask[signum];
    if (copyout(p->pagetable, oldact, (char *)&old,
                sizeof(struct sigaction_)) < 0)
    {
      release(&p->lock);
      return -1;
    }
  }
  if (act != 0)
  {
    //check validity of mask

    if (copyin(p->pagetable, (char *)&new, act,
               sizeof(struct sigaction_)) < 0)
    {
      release(&p->lock);
      return -1;
    }
    if (new.sigmask & 1 << SIGKILL)
    {
      printf("sigmask of handler can't block SIGKILL\n");
      release(&p->lock);
      return -1;
    }
    if (new.sigmask & 1 << SIGSTOP)
    {
      printf("sigmask of handler can't block SIGSTOP\n");
      release(&p->lock);
      return -1;
    }

    p->sighandlers[signum] = new.sa_handler_;
    p->sighandlers_mask[signum] = new.sigmask;
  }
  release(&p->lock);
  return 0;
}

// ADDED: sigret system call
void sigret(void)
{
  struct proc *p = myproc();
  struct thread *t = mythread();
  acquire(&p->lock);
  acquire(&t->lock);
  memmove(t->trapframe, p->trapframe_backup, sizeof(struct trapframe));
  p->signal_mask = p->signal_mask_backup;
  p->handling_signal = 0;
}

void sigcont(struct proc *p)
{
  acquire(&p->lock);
  p->stopped_non_zero = 0;
  release(&p->lock);
}
void sigkill()
{
  struct proc *p = myproc();
  p->killed = 1;
}
void sigstop()
{
  struct proc *p = myproc();
  printf("pid:%d got SIGSTOP\n", p->pid);
  p->stopped_non_zero = 1;
  while ((p->pending_signals & 1 << SIGCONT) == 0)
  {
    yield();
  }
  p->stopped_non_zero = 0;
  p->pending_signals ^= 1 << SIGCONT;
}
void init_userhandler(int signum)
{
  struct proc *p = myproc();
  printf("initiatig userhandler\n");
  p->signal_mask_backup = p->signal_mask;
  p->handling_signal = 1;
  memmove(p->trapframe_backup, p->trapframe, sizeof(struct trapframe));
  p->trapframe->epc = (uint64)p->sighandlers[signum];                                     // after trampoline user space will start from what is in epc
  p->trapframe->sp -= sigretEnd - sigretStart;                                            //freeeing space to put sigret on users stack
  copyout(p->pagetable, p->trapframe->sp, (char *)&sigretStart, sigretEnd - sigretStart); //put sigret in userspace so we can run it
  p->trapframe->a0 = signum;
  p->trapframe->ra = p->trapframe->sp; // after doing user_handler return address is the sigret call
  p->pending_signals = p->pending_signals ^ (1 << signum);
  //after this we go back to trap.c, loading the modifieded trapframe and we happy.
}
void handle_pending_signals()
{

  struct proc *p = myproc();
  //kernel handlers
  if (p->pending_signals & 1 << SIGKILL)
  {
    p->killed = 1;
    p->pending_signals ^= 1 << SIGKILL;
    return;
  }
  if (p->pending_signals & 1 << SIGSTOP)
  {
    sigstop();
    p->pending_signals ^= 1 << SIGSTOP;
  }
  // if (p->pending_signals & 1<<SIGCONT){
  //   p->stopped =0;
  //   p->pending_signals ^= 1<<SIGCONT;
  // }
  for (int signum = 0; signum < 32; signum++)
  {

    int signal_is_pending = p->pending_signals & 1 << signum;
    int signal_is_blocked = p->signal_mask & 1 << signum;
    // printf("signum:%d, pending:%d, blocked:%d\n",signum,signal_is_pending,signal_is_blocked);
    if (signal_is_pending)
      if (!signal_is_blocked)
      {
        printf("signal is pending and is not blocked\n");
        switch (signum)
        {
        case SIGKILL:
          break;
        case SIGSTOP:
          break;
        case SIGCONT:
          break;
        default:
          //do user handler
          printf("do userhandler\n");
          p->pending_signals ^= 1 << signum;
          void *handler = p->sighandlers[signum];
          switch ((uint64)handler)
          {
          case SIGKILL:
            sigkill();
            break;
          case SIGSTOP:
            sigstop();
            break;
          case SIGCONT:
            sigcont(p);
          case SIG_DFL:
            sigkill();
          case SIG_IGN:
            break;
          default:
            init_userhandler(signum);
          }
        }
      }
  }
}
void bseminit()
{
  initlock(&bsem_lock, "bsem_lock");
  acquire(&bsem_lock);
  for (struct bsem *bs = bsem; bs < bsem + MAX_BSEM; bs++)
  {
    initlock(&bs->bslock, "bsem");
  }
  release(&bsem_lock);
}
int bsem_alloc()
{
  // here we need a lock for the whole bsem array
  // this to a vooid a situation where 2 proccess find the same
  // index as unused bsem. then both of them will try to acuire
  acquire(&bsem_lock);
  for (struct bsem *bs = bsem; bs < bsem + MAX_BSEM; bs++)
  {
    if (bs->state == BSUNUSED)
    {
      bs->state = BSFREE;
      release(&bsem_lock);
      int descriptor = (bs - bsem) / sizeof(struct bsem);
      return descriptor;
    }
  }
  release(&bsem_lock);
  return -1;
};
void bsem_free(int descriptor)
{
  if (descriptor < 0 || descriptor > MAX_BSEM)
    return;
  // lock for the whole array is needed like in bsem_alloc
  acquire(&bsem_lock);
  struct bsem bs = bsem[descriptor];
  if (bs.state == BSACQUIRED)
  {
    release(&bsem_lock);
    panic("freeing ACQUIRED bsem is unsupported\n");
    return;
  }
  bs.state = BSFREE;
  release(&bsem_lock);
};
void bsem_down(int descriptor)
{
  if (descriptor < 0 || descriptor > MAX_BSEM)
    return;
  struct bsem bs = bsem[descriptor];
  acquire(&bs.bslock);
  if (bs.state == BSUNUSED)
  {
    release(&bs.bslock);
    printf("can't down on pre-allocated bsem\n");
    return;
  }
  while (bs.state == BSACQUIRED)
  {
    sleep(&bs, &bs.bslock);
    if (myproc()->killed)
    {
      return;
    }
  }
  bs.state = BSACQUIRED;
  release(&bs.bslock);
};
void bsem_up(int descriptor)
{
  if (descriptor < 0 || descriptor > MAX_BSEM)
    return;
  struct bsem bs = bsem[descriptor];
  acquire(&bs.bslock);
  if (bs.state != BSACQUIRED)
  {
    release(&bs.bslock);
    return;
  }
  bs.state = BSFREE;
  wakeup(&bs);
  release(&bs.bslock);
};

int kthread_id()
{
  return mythread()->tid;
};