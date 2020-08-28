/* PANDABEGINCOMMENT
 *
 *  Authors:
 *  Tiemoko Ballo           N/A
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 * See the COPYING file in the top-level directory.
 *
PANDAENDCOMMENT */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include <set>
#include <map>
#include <string>
#include <vector>

#include "panda/plugin.h"
#include "panda/common.h"

#include "syscalls2/syscalls_ext_typedefs.h"
#include "syscalls2/syscalls2_info.h"
#include "syscalls2/syscalls2_ext.h"
#include "osi/osi_types.h"
#include "osi/osi_ext.h"
#include "osi_linux/osi_linux_ext.h"
#include "callstack_instr/callstack_instr.h"
#include "callstack_instr/callstack_instr_ext.h"

#include "signal_int_fns.h"

#include <capstone/capstone.h>
#if defined(TARGET_I386)
#include <capstone/x86.h>
#elif defined(TARGET_ARM)
#include <capstone/arm.h>
#elif defined(TARGET_PPC)
#include <capstone/ppc.h>
#elif defined(TARGET_MIPS)
#include <capstone/mips.h>
#endif


// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
    bool init_plugin(void *);
    void uninit_plugin(void *);
}

// If a future API is to retrive events thus far (using CFFI and not PANDALOG)
//#define IN_MEM_BUF

// Verbose print to console
//#define DEBUG

// Structs -------------------------------------------------------------------------------------------------------------

// Captured signal event
typedef struct sig_event_t {
    int32_t sig;
    bool suppressed;
    std::string src_name;
    std::string dst_name;
    target_pid_t src_pid;
    target_pid_t dst_pid;
} sig_event_t;

// Globals -------------------------------------------------------------------------------------------------------------

Panda__SignalEvent pse;  // Global faster than malloc, size is fixed
std::set<int32_t> hyper_blocked_sigs;
std::map<std::string, std::set<int32_t>> hyper_blocked_sigs_by_proc;
target_ulong do_signal_kaddr = 0;
target_ulong get_signal_kaddr = 0;

#ifdef IN_MEM_BUF
    std::vector<sig_event_t> hyper_sig_events;
#endif

// Python CFFI API -----------------------------------------------------------------------------------------------------

// Block a signal for all processes
void block_sig(int32_t sig) {
    hyper_blocked_sigs.insert(sig);
    printf("signal: suppressing %d universally.\n", sig);
}

// Block a signal only for a named process
void block_sig_by_proc(int32_t sig, char* proc_name) {

    std::string name(proc_name);
    auto named_block = hyper_blocked_sigs_by_proc.find(name);

    if (named_block == hyper_blocked_sigs_by_proc.end()) {
        std::set<int32_t> new_sig_set{sig};
        hyper_blocked_sigs_by_proc.insert(std::make_pair(proc_name, new_sig_set));
    } else {
        named_block->second.insert(sig);
    }

    printf("signal: suppressing %d only for process \'%s\'.\n", sig, proc_name);
}

// Address of Linux kernel's get_signal() function (for kernel-to-process interception)
// https://elixir.bootlin.com/linux/v5.8/source/kernel/signal.c#L2526
void register_kernel_get_signal_addr(target_ulong addr) {
    get_signal_kaddr = addr;
}

// Address of Linux kernel's do_signal() function (for kernel-to-process interception)
// https://elixir.bootlin.com/linux/v5.8/source/arch/arm/kernel/signal.c#L578
void register_kernel_do_signal_addr(target_ulong addr) {
    do_signal_kaddr = addr;
}

// Core ----------------------------------------------------------------------------------------------------------------

// Incremental log update
void flush_to_plog(sig_event_t* se_ptr) {

    if (!pandalog) { return; }    // Pre-condition

    // Load event
    pse = PANDA__SIGNAL_EVENT__INIT;
    pse.sig = se_ptr->sig;
    pse.suppressed = se_ptr->suppressed;
    pse.src_name = (char*)se_ptr->src_name.c_str();
    pse.dst_name = (char*)se_ptr->dst_name.c_str();
    pse.src_pid = se_ptr->src_pid;
    pse.dst_pid = se_ptr->dst_pid;

    // Flush event
    Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
    ple.signal_event = &pse;
    pandalog_write_entry(&ple);
}

// Per Luke C., we'll suppress by swapping to SIGWINCH instead of re-directing control flow from the hypervisor
// Andrew F. also suggested setting an illegal sig num and suppressing the error on syscall return - noting in case we decide to switch to that impl
bool suppress_curr_sig(CPUState* cpu) {

    target_ulong sigwinch_num = 28;

    #if defined(TARGET_I386) and !defined(TARGET_X86_64)
        // int 0x80 -> ecx
        // https://en.wikibooks.org/wiki/X86_Assembly/Interfacing_with_Linux#int_0x80
        #define SIG_ARG_REG &(((CPUArchState*)cpu->env_ptr)->regs[1])
    #elif defined(TARGET_X86_64)
        // syscall -> rsi
        // https://en.wikibooks.org/wiki/X86_Assembly/Interfacing_with_Linux#syscall
        #define SIG_ARG_REG &(((CPUArchState*)cpu->env_ptr)->regs[6])
    #elif defined(TARGET_ARM) and !defined(TARGET_AARCH64)
        // swi -> r1
        // https://jumpnowtek.com/shellcode/linux-arm-shellcode-part1.html
        #define SIG_ARG_REG &(((CPUArchState*)cpu->env_ptr)->regs[1])
    #elif defined(TARGET_AARCH64)
        // swi -> x1
        // https://jumpnowtek.com/shellcode/linux-arm-shellcode-part1.html
        #define SIG_ARG_REG &(((CPUArchState*)cpu->env_ptr)->xregs[1])
    #elif defined(TARGET_MIPS)
        // syscall -> a1
        // https://www.linux-mips.org/wiki/Syscall
        #define SIG_ARG_REG &(((CPUArchState*)cpu->env_ptr)->active_tc.gpr[5])
    #else
        // NOP for unsupported architectures
        #define SIG_ARG_REG &sigwinch_num
    #endif

    *SIG_ARG_REG = sigwinch_num;
    return true;
}

// Process-to-kernel-to-process logging/suppression
void sig_mitm(CPUState* cpu, target_ulong pc, int32_t pid, int32_t sig) {

    bool suppressed = false;

    // pid -> signal destination process name
    std::string dst_proc_name("UNKOWN_DST_PROC");
    GArray *proc_list = get_processes(cpu);
    if (proc_list != NULL) {
        for (int i = 0; i < proc_list->len; i++) {
            OsiProc *proc = &g_array_index(proc_list, OsiProc, i);
            if (proc->pid == pid) {
                dst_proc_name = proc->name;
                break;
            }
        }
    }

    // Optional supression
    if (hyper_blocked_sigs.find(sig) != hyper_blocked_sigs.end()) {
        suppressed = suppress_curr_sig(cpu);
    } else {
        auto named_block = hyper_blocked_sigs_by_proc.find(dst_proc_name);
        if (named_block != hyper_blocked_sigs_by_proc.end()) {
            if (named_block->second.find(sig) != named_block->second.end()) {
                suppressed = suppress_curr_sig(cpu);
            }
        }
    }

    // Logging
    OsiProc* curr_proc = get_current_process(cpu);
    sig_event_t sig_event = {
        sig,
        suppressed,
        curr_proc->name,
        dst_proc_name,
        curr_proc->pid,
        pid,
    };
    flush_to_plog(&sig_event);

    #if defined(DEBUG)
        printf("[DEBUG] signal (%d): %s (%d) -> %s (%d). %s.\n",
            sig_event.sig,
            sig_event.src_name.c_str(),
            sig_event.src_pid,
            sig_event.dst_name.c_str(),
            sig_event.dst_pid,
            (sig_event.suppressed ? "Suppressed" : "Passed through")
        );
    #endif

    #ifdef IN_MEM_BUF
        hyper_sig_events.push_back(sig_event);
    #endif
}

// Kernel-to-process logging/suppression
// For capstone APIs, see: https://github.com/aquynh/capstone/blob/master/include/capstone/capstone.h
void kernel_call_handler(CPUState *cpu, target_ulong func) {

    // Max call stack entries to retrieve
    #define CALLER_MAX_CNT 4

    // Max instruction encoding length
    #if defined(TARGET_I386)
        #define MAX_INSTR_LEN 15
    #else
        #define MAX_INSTR_LEN 4
    #endif

    target_ulong curr_pc = cpu->panda_guest_pc;
    bool do_signal_callee = false;
    bool is_call = false;

    target_ulong caller_addrs[CALLER_MAX_CNT];
    uint8_t instr_buf[MAX_INSTR_LEN];
    int caller_cnt;
    int res;
    csh handle;
	cs_insn *insn;
    cs_detail *detail;
	size_t count;

    // Short-circuit if addrs not initialized or not in kernel
    if ((!do_signal_kaddr) || (!get_signal_kaddr) || !panda_in_kernel(cpu)) {
        return;
    }

    // Verify that we're in do_signal's callframe
    caller_cnt = get_callers(caller_addrs, CALLER_MAX_CNT, cpu);
    for (int i = 0; i < caller_cnt; i++) {
        if (caller_addrs[i] == do_signal_kaddr) {
            do_signal_callee = true;
        }
    }

    if (!do_signal_callee) {
        return;
    }

    // Read out call instruction bytes
    res = panda_virtual_memory_read(cpu, curr_pc, instr_buf, MAX_INSTR_LEN);
    if (res < 0) {
        fprintf(stderr, "[ERROR] signal: Failed to read guest memory from PC 0x" TARGET_PTR_FMT "!\n", curr_pc);
        return;
    }

    // Init Capstone
    #if defined(TARGET_I386) and !defined(TARGET_X86_64)
        if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK) {
            fprintf(stderr, "[ERROR] signal: Failed to init Capstone for x86!\n");
		    return;
        }
    #elif defined(TARGET_X86_64)
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
            fprintf(stderr, "[ERROR] signal: Failed to init Capstone for x64!\n");
		    return;
        }
    #elif defined(TARGET_ARM) and !defined(TARGET_AARCH64)
        if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &handle) != CS_ERR_OK) {
            fprintf(stderr, "[ERROR] signal: Failed to init Capstone for ARM!\n");
		    return;
        }
    #elif defined(TARGET_AARCH64)
        if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK) {
            fprintf(stderr, "[ERROR] signal: Failed to init Capstone for AARCH64!\n");
		    return;
        }
    #elif defined(TARGET_MIPS)
        if (cs_open(CS_ARCH_MIPS, CS_MODE_MIPS32, &handle) != CS_ERR_OK) {
            fprintf(stderr, "[ERROR] signal: Failed to init Capstone for MIPS!\n");
		    return;
        }
    #else
        fprintf(stderr, "[ERROR] signal: Unsuppported architecture for kernel hooking!\n");
        return;
    #endif

    // Request detailed disassembly
    if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
        fprintf(stderr, "[ERROR] signal: Failed to set Capstone options!\n");
        return;
    }

    // Disassemble call instruction
    count = cs_disasm(handle, instr_buf, MAX_INSTR_LEN, curr_pc, 1, &insn);
    if (count > 0) {
        assert(count == 1);

        // Arch neutral verify of call instr
        detail = insn[0].detail;
        if (detail->groups_count > 0) {
            for (int i = 0; i < detail->groups_count; i++) {
                if (detail->groups[i] == CS_GRP_CALL) {
                    is_call = true;
                }
            }
        }
        assert(is_call);

        // TODO: get call target

        // TODO: check if call target is get_signal address

        // TODO: if so, pull out signal number from ksignal struct
        // https://elixir.bootlin.com/linux/v5.8/source/include/linux/signal_types.h#L65

        cs_free(insn, count);
    }
    cs_close(&handle);

    // TODO: detail for call target, check against get_signal, retreive struct
}

// Setup/Teardown ------------------------------------------------------------------------------------------------------

bool init_plugin(void *_self) {

    if (!pandalog) {
        fprintf(stderr, "[ERROR] signal: Set with -pandalog [filename]\n");
        return  false;
    }

    // Setup dependencies
    panda_enable_precise_pc();
    panda_require("syscalls2");
    assert(init_syscalls2_api());
    panda_require("osi");
    assert(init_osi_api());
    panda_require("osi_linux");
    assert(init_osi_linux_api());
    panda_require("callstack_instr");
    assert(init_callstack_instr_api());

    // Setup signature
    switch (panda_os_familyno) {

        case OS_LINUX: {
            #if ((defined(TARGET_I386) && !defined(TARGET_X86_64)) || (defined(TARGET_ARM) && !defined(TARGET_AARCH64)) || defined(TARGET_MIPS))
                printf("signal: setting up 32-bit Linux.\n");
                PPP_REG_CB("syscalls2", on_sys_kill_enter, sig_mitm);
                PPP_REG_CB("callstack_instr", on_call, kernel_call_handler);
            #elif (defined(TARGET_X86_64) || defined(TARGET_AARCH64))
                printf("signal: setting up 64-bit Linux.\n");
                PPP_REG_CB("syscalls2", on_sys_kill_enter, sig_mitm);
                PPP_REG_CB("callstack_instr", on_call, kernel_call_handler);
            #else
                fprintf(stderr, "[ERROR] signal: Unsuppported architecture for Linux!\n");
                return false;
            #endif
            return true;
        } break;

        /*
        // TODO: need OSI and kernel function hooking logic to support FreeBSD
        case OS_FREEBSD: {
            #if defined(TARGET_X86_64)
                printf("signal: setting up 64-bit FreeBSD.\n");
                PPP_REG_CB("syscalls2", on_kill_enter, sig_mitm);
                PPP_REG_CB("callstack_instr", on_call, kernel_call_handler);
            #else
                fprintf(stderr, "[ERROR] signal: Unsuppported architecture for FreeBSD!\n");
                return false;
            #endif
            return true;
        }
        */

        default: {
            fprintf(stderr, "[ERROR] signal: Unsuppported operating system!\n");
            return false;
        }
    }
}

void uninit_plugin(void *_self) {
    // N/A
}