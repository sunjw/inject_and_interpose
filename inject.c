#include "inject.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h> // xx remove this

struct dyld_all_image_infos {
    uint32_t version;
    uint32_t infoArrayCount;
    uint32_t infoArray;
    uint32_t notification;
    uint8_t processDetachedFromSharedRegion;
    uint8_t libSystemInitialized;
    uint8_t pad[2];
    uint32_t dyldImageLoadAddress;
};

struct dyld_all_image_infos_64 {
    uint32_t version;
    uint32_t infoArrayCount;
    uint64_t infoArray;
    uint64_t notification;
    uint8_t processDetachedFromSharedRegion;
    uint8_t libSystemInitialized;
    uint8_t pad[6];
    uint64_t dyldImageLoadAddress;
};

#define ARM_THREAD_STATE 1
struct arm_thread_state {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
};

#define x86_THREAD_STATE32 1
struct x86_thread_state32 {
    uint32_t eax, ebx, ecx, edx,
             edi, esi, ebp, esp,
             ss, eflags, eip,
             cs, ds, es, fs, gs;
};

#define x86_THREAD_STATE64 4
struct x86_thread_state64 {
    uint64_t rax, rbx, rcx, rdx,
             rdi, rsi, rbp, rsp,
             r8, r9, r10, r11,
             r12, r13, r14, r15,
             rip, rflags,
             cs, fs, gs;
};

#define PPC_THREAD_STATE64 5
struct ppc_thread_state64 {
    uint64_t srr0, srr1;
    uint64_t r[32];
    uint32_t cr;
    uint64_t xer, lr, ctr;
    uint32_t vrsave;
};

// covers multiple flavors - some contain the state but not all
#pragma pack(4)
struct exception_message {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    integer_t code[2];
    int flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t old_state[144];
};

struct exception_reply {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
    int flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t new_state[144];
};
#pragma pack()

static const vm_size_t stack_size = 32*1024;

struct addr_bundle {
    mach_vm_address_t dlopen;
    mach_vm_address_t syscall;
};

struct symtab_bundle {
    mach_vm_address_t symaddr;
    uint32_t nsyms;
    mach_vm_address_t straddr;
    uint32_t strsize;
};


#define FAIL(x) do { kr = x; fprintf(stderr, "fail on line %d\n", __LINE__); goto bad; } while(0)
#define TRY(x) do { if(kr = x) { fprintf(stderr, "fail on line %d: %s\n", __LINE__, #x); goto bad; } } while(0)
#define address_cast(x) ((mach_vm_address_t) (uintptr_t) (x))
#define SWAP(x) (swap ? __builtin_bswap32(x) : (x))
#define SWAP64(x) (swap ? __builtin_bswap64(x) : (x))

static inline void handle_sym(const char *sym, uint32_t size, mach_vm_address_t value, struct addr_bundle *bundle) {
    switch(sym[1]) {
    case 'd':
        if(!strncmp(sym, "_dlopen", size)) bundle->dlopen = value;
        break;
    case 's':
        if(!strncmp(sym, "_syscall", size)) bundle->syscall = value;
        break;
    }
}

static kern_return_t find_symtab_addrs(uint32_t ncmds, mach_vm_size_t sizeofcmds, struct load_command *cmds, bool swap, size_t nlist_size, struct symtab_bundle *symtab) {
    struct load_command *lc;
    uint32_t symoff = 0, stroff;
    uint32_t cmdsleft;
    mach_vm_size_t accumulated_sizeofcmds = 0;

    lc = cmds;
    cmdsleft = ncmds;
    while(cmdsleft--) {
        if((accumulated_sizeofcmds += SWAP(lc->cmdsize)) > sizeofcmds) {
            // the mach file is invalid
            return KERN_INVALID_ARGUMENT;
        }
        if(SWAP(lc->cmd) == LC_SYMTAB) {
            struct symtab_command *sc = (void *) lc;
            symoff = SWAP(sc->symoff);
            symtab->nsyms = SWAP(sc->nsyms);
            stroff = SWAP(sc->stroff);
            symtab->strsize = SWAP(sc->strsize);
            if(symtab->strsize >= 1000000 || symtab->nsyms >= 1000000) {
                return KERN_INVALID_ARGUMENT;
            }
        }
        lc = (void *) ((char *) lc + SWAP(lc->cmdsize));
    }

    if(!symoff) {
        return KERN_INVALID_ARGUMENT;
    }

    lc = cmds;
    cmdsleft = ncmds;
    while(cmdsleft--) {
        if(SWAP(lc->cmd) == LC_SEGMENT) {
            struct segment_command *sc = (void *) lc;
            if(SWAP(sc->fileoff) < symoff && (SWAP(sc->fileoff) + SWAP(sc->filesize)) >= (symoff + symtab->nsyms * nlist_size)) {
                symtab->symaddr = sc->vmaddr + symoff - sc->fileoff; 
            }
            if(SWAP(sc->fileoff) < stroff && (SWAP(sc->fileoff) + SWAP(sc->filesize)) >= (stroff + symtab->strsize)) {
                symtab->straddr = sc->vmaddr + stroff - sc->fileoff; 
            }
        } else if(SWAP(lc->cmd) == LC_SEGMENT_64) {
            struct segment_command_64 *sc = (void *) lc;
            if(SWAP64(sc->fileoff) < symoff && (SWAP64(sc->fileoff) + SWAP64(sc->filesize)) >= (symoff + symtab->nsyms * nlist_size)) {
                symtab->symaddr = sc->vmaddr + symoff - sc->fileoff; 
            }
            if(SWAP64(sc->fileoff) < stroff && (SWAP64(sc->fileoff) + SWAP64(sc->filesize)) >= (stroff + symtab->strsize)) {
                symtab->straddr = sc->vmaddr + stroff - sc->fileoff; 
            }
        }
        lc = (void *) ((char *) lc + SWAP(lc->cmdsize));
    }

    if(!symtab->straddr || !symtab->symaddr) {
        return KERN_INVALID_ARGUMENT;
    }

    return 0;
}


static kern_return_t get_stuff(task_t task, cpu_type_t *cputype, struct addr_bundle *addrs) {
#ifdef __arm__
    *cputype = CPU_TYPE_ARM;
    addrs->dlopen = (mach_vmess_t) &dlopen;
    addrs->mach_thread_self = (mach_vmess_t) &mach_thread_self;
    addrs->thread_terminate = (mach_vmess_t) &thread_terminate;
    return 0;
#else
    kern_return_t kr = 0;
    task_dyld_info_data_t info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    union {
        struct dyld_all_image_infos data;
        struct dyld_all_image_infos_64 data64;
    } u;
    mach_vm_size_t data_size;


    TRY(task_info(task, TASK_DYLD_INFO, (task_info_t) &info, &count));
        
    data_size = sizeof(u);
    if(info.all_image_info_size < data_size) data_size = info.all_image_info_size;

    TRY(mach_vm_read_overwrite(task, info.all_image_info_addr, data_size, address_cast(&u), &data_size));

    if(u.data.version == 1) return KERN_NO_SPACE;

    // Try to guess whether the process is 64-bit,
    bool proc64 = info.all_image_info_addr > 0xffffffff;

    mach_vm_address_t dyldImageLoadAddress = proc64 ? u.data64.dyldImageLoadAddress : u.data.dyldImageLoadAddress;

    struct mach_header mach_hdr;
    TRY(mach_vm_read_overwrite(task, dyldImageLoadAddress, sizeof(mach_hdr), address_cast(&mach_hdr), &data_size));

    bool swap = mach_hdr.magic == MH_CIGAM || mach_hdr.magic == MH_CIGAM_64;
    bool mh64 = mach_hdr.magic == MH_MAGIC_64 || mach_hdr.magic == MH_CIGAM_64;

    *cputype = SWAP(mach_hdr.cputype);

    size_t nlist_size = mh64 ? sizeof(struct nlist_64) : sizeof(struct nlist);

    mach_vm_size_t sizeofcmds = SWAP(mach_hdr.sizeofcmds);
    struct load_command *cmds = malloc(sizeofcmds);

    TRY(mach_vm_read_overwrite(task, dyldImageLoadAddress + (mh64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header)), sizeofcmds, address_cast(cmds), &sizeofcmds));

    struct symtab_bundle symtab;
    TRY(find_symtab_addrs(mach_hdr.ncmds, sizeofcmds, cmds, swap, nlist_size, &symtab));
    
    char *strs = malloc(symtab.strsize);
    void *syms = malloc(symtab.nsyms * nlist_size);

    TRY(mach_vm_read_overwrite(task, symtab.straddr, symtab.strsize, address_cast(strs), &data_size));
    TRY(mach_vm_read_overwrite(task, symtab.symaddr, symtab.nsyms * nlist_size, address_cast(syms), &data_size));

    memset(addrs, 0, sizeof(*addrs));

    if(mh64) {
        const struct nlist_64 *nl = syms;
        while(symtab.nsyms--) {
            uint32_t strx = (uint32_t) SWAP(nl->n_un.n_strx);
            if(strx >= symtab.strsize) FAIL(KERN_FAILURE);
            handle_sym(strs + strx, symtab.strsize - strx, (mach_vm_address_t) SWAP64(nl->n_value), addrs);
            nl++;
        }
    } else {
        const struct nlist *nl = syms;
        while(symtab.nsyms--) {
            uint32_t strx = SWAP(nl->n_un.n_strx);
            if(strx >= symtab.strsize) FAIL(KERN_FAILURE);
            handle_sym(strs + strx, symtab.strsize - strx, (mach_vm_address_t) SWAP(nl->n_value), addrs);
            nl++;
        }
    }

    if(!addrs->dlopen || !addrs->syscall) FAIL(KERN_INVALID_ADDRESS);

    kr = 0;
bad:
    if(cmds) free(cmds);
    if(strs) free(strs);
    if(syms) free(syms);
    return kr;
#endif
}

kern_return_t inject(pid_t pid, const char *path) {
    kern_return_t kr = 0;

    char *path_real = realpath(path, 0);
    if(!path_real) return KERN_INVALID_ARGUMENT;
    
    task_t task = 0;
    TRY(task_for_pid(mach_task_self(), (int) pid, &task));

    cpu_type_t cputype;
    struct addr_bundle addrs;
    TRY(get_stuff(task, &cputype, &addrs));

    mach_vm_address_t stack_address = 0;
    TRY(mach_vm_allocate(task, &stack_address, stack_size, VM_FLAGS_ANYWHERE));

    mach_vm_address_t stack_end = stack_address + stack_size - 0x100;

    TRY(mach_vm_write(task, stack_address, address_cast(path_real), strlen(path_real) + 1));

    // the first one is the return address
    uint32_t args_32[] = {0, 360, 0xdeadbeef, 0xdeadbeef, 128*1024, 0, 0};
    uint64_t args_64[] = {0, 360, 0xdeadbeef, 0xdeadbeef, 128*1024, 0, 0};
    
    union {
        struct arm_thread_state arm;
        struct x86_thread_state32 x86;
        struct x86_thread_state64 x64;
        struct ppc_thread_state64 ppc;
        natural_t nat;
    } state;
    thread_state_flavor_t state_flavor;
	mach_msg_type_number_t state_count;

    memset(&state, 0, sizeof(state));

    switch(cputype) {
    case CPU_TYPE_ARM:
        memcpy(&state.arm.r[0], args_32 + 1, 4*4);
        TRY(mach_vm_write(task, stack_end, address_cast(args_32 + 5), 2*4));

        state.arm.sp = (uint32_t) stack_end;
        state.arm.pc = (uint32_t) addrs.syscall;

        state_flavor = ARM_THREAD_STATE;
        state_count = sizeof(state.arm) / sizeof(state.nat);
        break;
    case CPU_TYPE_X86:
        TRY(mach_vm_write(task, stack_end, address_cast(args_32), 7*4));

        state.x86.esp = state.x86.ebp = (uint32_t) stack_end;
        state.x86.eip = (uint32_t) addrs.syscall;

        state_flavor = x86_THREAD_STATE32;
        state_count = sizeof(state.x86) / sizeof(state.nat);
        break;
    case CPU_TYPE_X86_64:
        state.x64.rdi = args_64[1];
        state.x64.rsi = args_64[2];
        state.x64.rdx = args_64[3];
        state.x64.rcx = args_64[4];
        state.x64.r8  = args_64[5];
        state.x64.r9  = args_64[6];

        state.x64.rsp = state.x64.rbp = stack_end;
        state.x64.rip = addrs.syscall;

        state_flavor = x86_THREAD_STATE64;
        state_count = sizeof(state.x64) / sizeof(state.nat);
        break;
    case CPU_TYPE_POWERPC:
    case CPU_TYPE_POWERPC64:
        state.ppc.r[1] = stack_end;
        memcpy(&state.ppc.r[3], args_64 + 1, 6*8);
        state.ppc.srr0 = addrs.syscall;
        
        state_flavor = PPC_THREAD_STATE64;
        state_count = sizeof(state.ppc) / sizeof(state.nat);
        break;
    default:
        abort();
    }

    mach_port_t exc;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc);

    thread_act_t thread = 0;
    TRY(thread_create(task, &thread));
    TRY(mach_port_insert_right(mach_task_self(), exc, exc, MACH_MSG_TYPE_MAKE_SEND));
    
    exception_mask_t em[2];
    exception_handler_t eh[2];
    exception_behavior_t eb[2];
    thread_state_flavor_t ef[2];
    mach_msg_type_number_t em_count = 2;

    TRY(task_swap_exception_ports(task, EXC_MASK_BAD_ACCESS, exc, EXCEPTION_STATE_IDENTITY, state_flavor, em, &em_count, eh, eb, ef));
    if(em_count > 1) FAIL(KERN_FAILURE);

    TRY(thread_set_state(thread, state_flavor, &state.nat, state_count));

    TRY(thread_resume(thread));

    // We expect three exceptions: one from thread when it returns, one from the new thread when it calls the fake handler, and one from the new thread when it returns from dlopen.
    bool started_dlopen = false;
    while(1) {
        struct exception_message msg;
        TRY(mach_msg_overwrite(NULL, MACH_RCV_MSG, 0, sizeof(msg), exc, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, (void *) &msg, sizeof(msg)));
        printf("got a message\n");
        if(!(msg.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
           (msg.msgh_body.msgh_descriptor_count == 0) ||
           (msg.Head.msgh_size < offsetof(struct exception_message, old_state)) ||
           (msg.old_stateCnt != state_count) ||
           (msg.Head.msgh_size < offsetof(struct exception_message, old_state) + msg.old_stateCnt * sizeof(natural_t))) {
            FAIL(KERN_FAILURE);
        }
        memcpy(&state, msg.old_state, sizeof(state));

        if(msg.thread.name == thread) {
            TRY(thread_terminate(thread));
        } else {
            bool cond;

            switch(cputype) {
            case CPU_TYPE_ARM: cond = (state.arm.pc & ~1) == 0xdeadbeee; break;
                state.arm.pc = (uint32_t) addrs.dlopen;
            case CPU_TYPE_X86: cond = state.x86.eip == 0xdeadbeef; break;
            case CPU_TYPE_X86_64: cond = state.x64.rip == 0xdeadbeef; break;
            case CPU_TYPE_POWERPC:
            case CPU_TYPE_POWERPC64: cond = state.ppc.srr0 == 0xdeadbeef; break;
            }

            if(!cond) {
                FAIL(KERN_FAILURE);
            } else if(started_dlopen) {
                TRY(thread_terminate(msg.thread.name));
                break;
            } else {
                switch(cputype) {
                case CPU_TYPE_ARM:
                    state.arm.r[0] = (uint32_t) stack_address;
                    state.arm.r[1] = RTLD_LAZY;
                    break;
                case CPU_TYPE_X86:
                    {
                        uint32_t stack_stuff[3] = {0xdeadbeef, (uint32_t) stack_address, RTLD_LAZY};
                        TRY(mach_vm_write(task, state.x86.esp, address_cast(&stack_stuff), sizeof(stack_stuff)));
                    }
                    state.x86.eip = (uint32_t) addrs.dlopen;
                    break;
                case CPU_TYPE_X86_64:
                    {
                        uint64_t stack_stuff = 0xdeadbeef;
                        TRY(mach_vm_write(task, state.x64.rsp, address_cast(&stack_stuff), sizeof(stack_stuff)));
                    }
                    state.x64.rip = addrs.dlopen;
                    state.x64.rdi = stack_address;
                    state.x64.rsi = RTLD_LAZY;
                    break;
                case CPU_TYPE_POWERPC:
                case CPU_TYPE_POWERPC64:
                    cond = state.ppc.srr0 == 0xdeadbeef;
                    state.ppc.srr0 = addrs.dlopen;
                    state.ppc.r[3] = stack_address;
                    state.ppc.r[4] = RTLD_LAZY;
                    state.ppc.lr = 0xdeadbeef;
                    break;
                }

                struct exception_reply reply;
                memcpy(&reply.Head, &msg.Head, sizeof(mach_msg_header_t));
                reply.Head.msgh_bits &= ~MACH_MSGH_BITS_COMPLEX;
                reply.Head.msgh_size = offsetof(struct exception_reply, new_state) + state_count * sizeof(natural_t);
                reply.Head.msgh_id += 100;
                memcpy(&reply.NDR, &msg.NDR, sizeof(NDR_record_t));
                reply.RetCode = 0;
                reply.flavor = state_flavor;
                reply.new_stateCnt = state_count;
                memcpy(&reply.new_state, &state, sizeof(state));

                TRY(thread_set_state(msg.thread.name, state_flavor, &state.nat, state_count));
                TRY(mach_msg(&reply.Head, MACH_SEND_MSG, reply.Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
                started_dlopen = true;
            }
        }
    }

    TRY(task_set_exception_ports(task, em[0], eh[0], eb[0], ef[0]));

    mach_port_deallocate(mach_task_self(), thread);
    mach_port_deallocate(mach_task_self(), task);

    return 0;    

bad:
    if(stack_address) vm_deallocate(task, stack_address, stack_size);
    if(thread) {
        thread_terminate(thread);
        mach_port_deallocate(mach_task_self(), thread);
    }
    if(task) mach_port_deallocate(mach_task_self(), task);
    if(exc) mach_port_deallocate(mach_task_self(), exc);
    return kr;
}