#include <syscall.h>
#include <scheduler.h>
#include <debug.h>
#include <mystring.h>
#include <vm.h>

static uint64 argraw(int n)
{
        process_t p = cur_proc(); 
        uint64 *args = &p->trapframe->a0;
        if(n <= 5)
                return args[n];

        PANIC("argraw");
        return -1;
}

int fetch_str_from_user(uint64 user_addr, char* buf, int max)
{
        process_t p = cur_proc();
        if(copyinstr(p->pagetable, buf, user_addr, max) < 0) {
                return -1;
        }
        return strlen(buf);
}

void argint(int n, int *ip)
{
        *ip = argraw(n);
}

void argaddr(int n, uint64 *ap)
{
        *ap = argraw(n);
}

int argstr(int n, char *buf, int max)
{
        uint64 addr;
        /* Get the address of string */
        argaddr(n, &addr);
        return fetch_str_from_user(addr, buf, max);
}