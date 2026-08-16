/* Turns a runaway-recursion stack overflow into a clean diagnostic instead
   of a silent SIGSEGV. Installed automatically (ELF constructor) in every
   compiled Cobra binary, with zero per-call overhead: the guard is a signal
   handler, not a check in every function prologue. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>
#include <unistd.h>

/* A fixed literal, not SIGSTKSZ: on modern glibc SIGSTKSZ is a runtime
   sysconf() value, not a compile-time constant, so it cannot size a
   file-scope array. 64 KiB is comfortably larger than glibc's own minimum
   (MINSIGSTKSZ) and enough room for the handler's own small stack frame. */
static char g_cobra_altstack[65536];

static void cobra_sigsegv_handler(int sig, siginfo_t *info, void *ucontext_v) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)ucontext_v;
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    uintptr_t rsp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    uintptr_t diff = (rsp > fault_addr) ? (rsp - fault_addr) : (fault_addr - rsp);

    /* A real stack-overflow fault lands within a few pages of the current
       stack pointer -- the faulting access is itself a push or a
       local-variable store just past the guard page. An unrelated segfault
       (null dereference, wild heap pointer) lands far from rsp, so this
       distinguishes the two without walking /proc/self/maps. */
    if (diff < 65536) {
        static const char msg[] = "[cobra] stack overflow (possible unbounded recursion)\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(3);
    }

    /* Not a stack-overflow-shaped fault: restore the default handler and
       re-raise, so the OS produces its normal segfault behavior instead of
       this guard masking a real, unrelated bug with a misleading message. */
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

__attribute__((constructor))
static void cobra_install_stack_guard(void) {
    stack_t ss;
    ss.ss_sp = g_cobra_altstack;
    ss.ss_size = sizeof(g_cobra_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) return;

    struct sigaction sa;
    sa.sa_sigaction = cobra_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}
