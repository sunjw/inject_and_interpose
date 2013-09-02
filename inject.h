#ifndef _INJECT_H_
#define _INJECT_H_

#include <unistd.h>
#include <mach/kern_return.h>

#ifdef __cplusplus
extern "C" {
#endif
    kern_return_t inject(pid_t pid, const char *path);
#ifdef __cplusplus
}
#endif

#endif

// The behavior is synchronous: when it returns, constructors have
// already been called.

// Bugs: Will fail, crash the target process, or even crash this process if the target task is weird.
