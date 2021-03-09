/*
 * Workflow of different components (AFL, OURTOOL, fork server, and client):
 *
 *   +--------- pre-handshake (shm) -----------+
 *   |               +-- pre-handshake (shm) --+
 *   |               |                         |
 * +-+-+         +---+---+              +-----+-----+                +------+
 * |AFL|         |OURTOOL|              |fork server|                |client|
 * +-+-+         +---+---+              +-----+-----+                +------+
 *   |               |                         |
 *   |               |   [trigger execution]   |   [   new client  &  ]
 *   +--------------{|}----------------------->|   [handshake (socket)]
 *   |               |                         +------------------------>|
 *   |               |                         |                         |
 *   |               |                         |                         |
 *   |               |                         |     [status (wait4)]    x crash
 *   |               |  [status (comm socket)] |<----------------------+-+
 *   |               |<------------------------+                       |
 *   |               |     [*CRPS* (shm)]      |                       |
 *   |               |<-----------------------{|}----------------------+
 *   |               |                         |
 *   |     validate  | [trigger (comm socket)] |
 *   |     crashsite ~ [ patch commands (shm)] |
 *   |     (if fake) +------------------------>|
 *   |               |                         ~ patch self and re-mmap
 *   |               |                         |
 *   |               |                         |   [   new client  &  ]
 *   |               |                         |   [handshake (socket)]
 *   |               |                         +------------------------>|
 *   |               |                         |                         |
 *   |               |                         |                         |
 *   |               |                         |     [status (wait4)]    x crash
 *   |               |  [status (comm socket)] |<----------------------+-+
 *   |               |<------------------------+                       |
 *   |               |     [*CRPS* (shm)]      |                       |
 *   |               |<-----------------------{|}----------------------+
 *   |               |                         |
 *   |     validate  | [trigger (comm socket)] |
 *   |     crashsite ~ [ patch commands (shm)] |
 *   |     (if real) +------------------------>|
 *   |               |                         |
 *   |               |    [status (socket)]    |
 *   |<-------------{|}------------------------+
 *   |               |                         |
 *   |               |                         |
 *   |               | [trigger new execution] |   [   new client  &  ]
 *   +--------------{|}----------------------->|   [handshake (socket)]
 *   |               |                         +------------------------>|
 *   |               |                         |                         |
 *   |               |                         |     [status (wait4)]    | exit
 *   |               |    [status (socket)]    |<------------------------+
 *   |<-------------{|}------------------------+
 *
 *
 *  *CRPS*: crash points
 *
 */

/*
 * Different situations:
 *
 * +------------------------+------------------+-------------------------------+
 * | Daemon mode / Run mode |   AFL attached   |           Action              |
 * +========================+==================+===============================+
 * |                        |        No        |        Perform dry run        |
 * |        Run mode        +------------------+-------------------------------+
 * |                        |        Yes       |           Invalid             |
 * +------------------------+------------------+-------------------------------+
 * |                        |        No        | Ignore AFL-related operations |
 * |       Daemon mode      +------------------+-------------------------------+
 * |                        |        Yes       |      Follow above workflow    |
 * +------------------------+------------------+-------------------------------+
 */

#include "fork_server.h"

#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "asm_utils.c"

#ifdef DEBUG
extern const char no_daemon_str[];
extern const char getenv_err_str[];
extern const char afl_shmat_err_str[];
extern const char crs_shmat_err_str[];
extern const char hello_err_str[];
extern const char read_err_str[];
extern const char fork_err_str[];
extern const char wait4_err_str[];
extern const char mumap_err_str[];
extern const char mprotect_err_str[];
extern const char pipe_err_str[];
extern const char dup2_err_str[];
extern const char env_setting_err_str[];
extern const char socket_err_str[];
extern const char data_pipe_err_str[];
extern const char msync_err_str[];
extern const char cmd_err_str[];
extern const char write_err_str[];
extern const char pipe_filename_err_str[];
extern const char afl_attached_str[];
extern const char status_str[];
extern const char setpgid_err_str[];
extern const char patch_cmd_err_str[];
#endif

extern const char magic_string[];
extern const char afl_shm_env[];

#define NO_SHM_ID -233

asm(".globl _entry\n"
    ".type _entry,@function\n"
    "_entry:\n"

    // (1) push all registers
    "\tpushq %r15;\n"
    "\tpushq %r14;\n"
    "\tpushq %r13;\n"
    "\tpushq %r12;\n"
    "\tpushq %r11;\n"
    "\tpushq %r10;\n"
    "\tpushq %r9;\n"
    "\tpushq %r8;\n"
    "\tpushq %rcx;\n"
    "\tpushq %rdx;\n"
    "\tpushq %rsi;\n"
    "\tpushq %rdi;\n"

    // (2) make rsp 16-bytes alignment
    "\tmovq %rsp, %rbp;\n"
    "\torq $8, %rsp;\n"
    "\tpushq %rbp;\n"

    // (3) get envp into %rdi
    "\tmovq %rdx, %rdi;\n"

    // (4) call fork_server_start()
    "\tcallq fork_server_start;\n"

    // (5) restore context
    "\tpopq %rsp;\n"
    "\tpopq %rdi;\n"
    "\tpopq %rsi;\n"
    "\tpopq %rdx;\n"
    "\tpopq %rcx;\n"
    "\tpopq %r8;\n"
    "\tpopq %r9;\n"
    "\tpopq %r10;\n"
    "\tpopq %r11;\n"
    "\tpopq %r12;\n"
    "\tpopq %r13;\n"
    "\tpopq %r14;\n"
    "\tpopq %r15;\n"

    // (6) jump to following code
    "\tjmp __etext;\n"

#ifdef DEBUG
    // no_daemon_str
    ASM_STRING(no_daemon_str, "fork server: no daemon found, switch to dry run")
    // getenv_err_str
    ASM_STRING(getenv_err_str, "fork server: environments not found")
    // afl_shmat_err_str
    ASM_STRING(afl_shmat_err_str, "fork server: shmat error (AFL)")
    // crs_shmat_err_str
    ASM_STRING(crs_shmat_err_str, "fork server: shmat error (CRS)")
    // hello_err_str
    ASM_STRING(hello_err_str, "fork server: hello error")
    // write_err_str
    ASM_STRING(write_err_str, "fork server: write error")
    // read_err_str
    ASM_STRING(read_err_str, "fork server: read error")
    // fork_err_str
    ASM_STRING(fork_err_str, "fork server: fork error")
    // wait4_err_str
    ASM_STRING(wait4_err_str, "fork server: wait4 error")
    // mumap_err_str
    ASM_STRING(mumap_err_str, "fork server: mumap error")
    // mprotect_err_str
    ASM_STRING(mprotect_err_str, "fork server: mprotect error")
    // pipe_err_str
    ASM_STRING(pipe_err_str, "fork server: pipe error")
    // socket_err_str
    ASM_STRING(socket_err_str, "fork server: socket error")
    // data_pipe_err_str
    ASM_STRING(data_pipe_err_str, "fork server: data pipe connection error")
    // msync_err_str
    ASM_STRING(msync_err_str, "fork server: msync error")
    // dup2_err_str
    ASM_STRING(dup2_err_str, "fork server: dup2 error")
    // cmd_err_str
    ASM_STRING(cmd_err_str, "fork server: invalid patch command type")
    // pipe_filename_err_str
    ASM_STRING(pipe_filename_err_str, "fork server: pipe filename too long")
    // env_setting_err_str
    ASM_STRING(env_setting_err_str,
               "fork server: fuzzing without daemon running")
    // afl_attached_str
    ASM_STRING(afl_attached_str, "fork server: AFL detected")
    // status_str
    ASM_STRING(status_str, "fork server: client status: ")
    // setpgid_err_str
    ASM_STRING(setpgid_err_str, "fork server: setpgid error")
    // patch_cmd_err_str
    ASM_STRING(patch_cmd_err_str, "fork server: too many patch commands")
#endif

    // Magic String to indicate instrumented
    ASM_STRING(magic_string, MAGIC_STRING)
    // AFL's shm environment variable
    ASM_STRING(afl_shm_env, AFL_SHM_ENV));

/*
 * Atoi without any safe check
 */
static inline int fork_server_atoi(char *s) {
    int val = 0;
    bool is_neg = false;

    if (*s == '-') {
        s++;
        is_neg = true;
    }

    while (*s)
        val = val * 10 + (*(s++) - '0');

    if (is_neg) {
        val = -val;
    }

    return val;
}

/*
 * Get shm_id from environment.
 */
static inline int fork_server_get_shm_id(char **envp) {
    char *s;
    while ((s = *(envp++))) {
        // hand-written strcmp with "__AFL_SHM_ID="
        if (*(unsigned long *)s != 0x48535f4c46415f5f) {
            continue;
        }
        if (*(unsigned int *)(s + 8) != 0x44495f4d) {
            continue;
        }
        if (*(s + 12) != '=') {
            continue;
        }

        return fork_server_atoi(s + 13);
    }

    utils_puts(getenv_err_str, true);
    return NO_SHM_ID;
}

/*
 * Patch guided by CRS patch commands
 */
static inline void fork_server_patch(int n, bool *shadow_need_sync) {
    // update n
    if (n > CRS_MAP_MAX_CMD_N) {
        utils_error(patch_cmd_err_str, true);
    }

    CRSCmd *cmd = (CRSCmd *)(CRS_MAP_ADDR);
    for (int i = 0; i < n; i++, cmd++) {
        if (cmd->type == CRS_CMD_NONE) {
            // place holder
            continue;
        } else if (cmd->type == CRS_CMD_REMMAP) {
            // munmap current shadow file
            if (sys_munmap(
                    (uint64_t)(RW_PAGE_INFO(program_base)) + SHADOW_CODE_ADDR,
                    RW_PAGE_INFO(shadow_size))) {
                utils_error(mumap_err_str, true);
            }
            // remmap it
            RW_PAGE_INFO(shadow_size) = utils_mmap_external_file(
                RW_PAGE_INFO(shadow_path),
                (uint64_t)(RW_PAGE_INFO(program_base)) + SHADOW_CODE_ADDR,
                PROT_READ | PROT_EXEC);
            // we do not need to sync the file right now
            *shadow_need_sync = false;
        } else if (cmd->type == CRS_CMD_REWRITE) {
            // patch one by one
            for (int j = 0; j < cmd->size; j++) {
                *((char *)(RW_PAGE_INFO(program_base) + cmd->addr + j)) =
                    cmd->buf[j];
            }
        } else if (cmd->type == CRS_CMD_MPROTECT) {
            // change page permission
            if (sys_mprotect(cmd->addr + (addr_t)RW_PAGE_INFO(program_base),
                             cmd->size, cmd->data)) {
                utils_error(mprotect_err_str, true);
            }
            continue;
        } else {
            utils_error(cmd_err_str, true);
        }
    }
}

/*
 * Connect to the pipeline
 */
static inline int fork_server_connect_pipe() {
    // step (1). create sock_fd
    int sock_fd = sys_socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        utils_error(socket_err_str, true);
    }

    //  step (2). construct sockaddr
    struct sockaddr_un server = {0};
    server.sun_family = AF_UNIX;
#ifdef DEBUG
    size_t n_ = utils_strcpy(server.sun_path, RW_PAGE_INFO(pipe_path));
    if (n_ >= sizeof(server.sun_path)) {
        utils_error(pipe_filename_err_str, true);
    }
#else
    utils_strcpy(server.sun_path, RW_PAGE_INFO(pipe_path));
#endif

    //  step (3). connect to daemon
    if (sys_connect(sock_fd, (struct sockaddr *)&server,
                    sizeof(struct sockaddr_un)) < 0) {
        // daemon is not setup, direct return (dry_run)
        sys_close(sock_fd);
        return -1;
    }

    return sock_fd;
}

/*
 * Start fork server and do random patch.
 */
NO_INLINE void fork_server_start(char **envp) {
    /*
     * step (1). setup comm connection
     */
    // step (1.1). connect socket for comm_fd
    int comm_fd = fork_server_connect_pipe();
    if (comm_fd < 0) {
        // make sure AFL is not attached
        if (fork_server_get_shm_id(envp) != NO_SHM_ID) {
            utils_error(env_setting_err_str, true);
        }
        utils_puts(no_daemon_str, true);
        RW_PAGE_INFO(daemon_attached) = false;
        return;
    } else {
        RW_PAGE_INFO(daemon_attached) = true;
    }

    // step (1.2). dup2 comm_fd to CRS_COMM_FD
    {
        if (sys_dup2(comm_fd, CRS_COMM_FD) < 0) {
            utils_error(dup2_err_str, true);
        }
        sys_close(comm_fd);
    }

    /*
     * step (2). check whether AFL is attached
     */
    int afl_shm_id = fork_server_get_shm_id(envp);
    bool afl_attached = (afl_shm_id != NO_SHM_ID);
    if (afl_attached) {
        utils_puts(afl_attached_str, true);
    }

    /*
     * step (3). read crs_shm_id from daemon and respond afl_attached (comm
     * shakehand)
     */
    // XXX: CRS may be uncessary once we use shared memory for .text section
    int crs_shm_id = 0;
    {
        if (sys_read(CRS_COMM_FD, (char *)&crs_shm_id, 4) != 4) {
            utils_error(hello_err_str, true);
        }

        int __tmp_data = afl_attached;
        if (sys_write(CRS_COMM_FD, (char *)&__tmp_data, 4) != 4) {
            utils_error(hello_err_str, true);
        }
    }

    /*
     * step (4). mmap CRS_SHARED_MEMORY
     */
    if ((size_t)sys_shmat(crs_shm_id, (const void *)CRS_MAP_ADDR, SHM_RND) !=
        CRS_MAP_ADDR) {
        utils_error(crs_shmat_err_str, true);
    }

    /*
     * step (5) [if: AFL_ATTACHED].
     *      munmap the fake AFL_SHARED_MEMORY and mmap the real one
     */
    if (afl_attached) {
        if (sys_munmap(AFL_MAP_ADDR, AFL_MAP_SIZE) != 0) {
            utils_error(mumap_err_str, true);
        }
        if ((size_t)sys_shmat(afl_shm_id, (const void *)AFL_MAP_ADDR,
                              SHM_RND) != AFL_MAP_ADDR) {
            utils_error(afl_shmat_err_str, true);
        }
    }

    /*
     * step (6). [if: AFL_ATTACHED]
     *      send 4-byte "hello" message to AFL
     */
    {
        int __tmp_data = 0x19961219;
        if (afl_attached) {
            if (sys_write(AFL_FORKSRV_FD + 1, (char *)&__tmp_data, 4) != 4) {
                utils_error(hello_err_str, true);
            }
        }
    }

    /*
     * step (7). main while-loop
     */
    bool crs_loop = false;
    while (true) {
        // step (7.1). [if: AFL_ATTACHED && !CRS_LOOP]
        //      wait AFL's signal
        if (afl_attached && !crs_loop) {
            int __tmp_data;
            if (sys_read(AFL_FORKSRV_FD, (char *)&__tmp_data, 4) != 4) {
                utils_error(read_err_str, true);
            }
        }

        // step (7.2). do fork
        pid_t tid = 0;
        pid_t client_pid =
            sys_clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD, 0,
                      NULL, &tid, NULL);
        if (client_pid < 0) {
            utils_error(fork_err_str, true);
        }

        if (client_pid == 0) {
            /*
             * child process
             */

            /*
             * XXX: To handle multi-thread/-process programs, a safe approach is
             * to change client's process group, and every time a potential
             * patch crash happens, the signal hander kills all processes in the
             * client's process group. The following code can be used to
             * implement this approach:
             *
             *   ------
             *      // set pgid, to avoid kill fork_server when sending signal
             *      if (sys_setpgid(0, 0)) {
             *          utils_error(setpgid_err_str, true);
             *      }
             *   ------
             *
             * However, the disadvantage of this approach is that, every time
             * the fork server creates a new client, the *setpgid* syscall will
             * bring additional overhead.
             *
             * Alternatively, we can use following code in the signal handler to
             * kill client and the crashed process:
             *
             *   ------
             *      sys_kill(client_pid, SIGUSR1);
             *      sys_kill(sys_getpid(), SIGUSR1);
             *   ------
             *
             * Instead of killing the whole process group like following
             *
             *   ------
             *      sys_kill(0, SIGUSR1);
             *   ------
             *
             * It is helpful when facing multi-thread/-process programs.
             * Additionally, it is also good to know that a child process can
             * send signal to its parent process (as if they share the same user
             * ID or effective user ID). But it may also leave some other
             * processes zombie (e.g., the parent process creates two child
             * processes).
             *
             * However, a good obversation is that, vanilla AFL can also have
             * such problem. Imagine that a multi-process program has a crashed
             * parent process, AFL will not take care of the client processes
             * anymore and leave them zombie.
             *
             * Hence, we choose the latter approach to reduce overhead.
             */
            RW_PAGE_INFO(client_pid) = tid;

            // TODO: correctly handle TIMEOUT.

            // update pid and tid in TLS, so that when the child process sends
            // signal to itself, it will not mis-send to its parent.
            //
            // check glibc source code for more information:
            //
            // https://code.woboq.org/userspace/glibc/sysdeps/nptl/fork.c.html#76
            // for how glibc implements fork() as a wrapper of syscall clone;
            //
            // https://code.woboq.org/userspace/glibc/nptl/descr.h.html#pthread
            // for the memory layout of struct pthread in glibc.
            register unsigned int tid_ asm("eax") = (unsigned int)tid;
            asm(".intel_syntax noprefix\n"
                "  mov DWORD PTR fs:0x2d0, eax;\n"
                "  mov DWORD PTR fs:0x2d4, eax;\n"
                :
                : "r"(tid_)
                :);

            // close uncessary file descriptors
            sys_close(AFL_FORKSRV_FD);
            sys_close(AFL_FORKSRV_FD + 1);
            sys_close(CRS_COMM_FD);

            RW_PAGE_INFO(afl_prev_id) = 0;
            break;
        }

        // step (7.3). [if: AFL_ATTACHED && !CRS_LOOP]
        //      tell AFL that the client is started
        if (afl_attached && !crs_loop) {
            sys_write(AFL_FORKSRV_FD + 1, (char *)&client_pid, 4);
        }

        // step (7.4). wait till the client stop
        int client_status = 0;
        if (sys_wait4(client_pid, &client_status, 0, NULL) < 0) {
            utils_error(wait4_err_str, true);
        }
#ifdef DEBUG
        utils_puts(status_str, false);
        utils_output_number(client_status);
#endif

        // step (7.5). check the client's status
        if (IS_SUSPECT_STATUS(client_status)) {
            // step (7.5.1). notify the daemon and wait response
            //      + sending out the status
            //      + receiving the number of commands or -1
            int cmd_n = -1;
            {
                sys_write(CRS_COMM_FD, (char *)&client_status, 4);
                sys_read(CRS_COMM_FD, (char *)&cmd_n, 4);
            }

            // step (7.5.2). if we need patch, do patch and notify the daemon
            if (cmd_n >= 0) {
                bool shadow_need_sync = true;

                // patch all cmds
                while (true) {
                    // do patch
                    fork_server_patch(cmd_n, &shadow_need_sync);
                    // terminate if needed
                    if (cmd_n < CRS_MAP_MAX_CMD_N) {
                        break;
                    }
                    // send back how many cmds are actually patched
                    sys_write(CRS_COMM_FD, (char *)&cmd_n, 4);
                    sys_read(CRS_COMM_FD, (char *)&cmd_n, 4);
                }

                // fsync shadow code and lookup table
                if (sys_msync(LOOKUP_TABLE_ADDR, RW_PAGE_INFO(lookup_tab_size),
                              MS_SYNC)) {
                    utils_error(msync_err_str, true);
                }
                if (shadow_need_sync) {
                    if (sys_msync((uint64_t)(RW_PAGE_INFO(program_base)) +
                                      SHADOW_CODE_ADDR,
                                  RW_PAGE_INFO(shadow_size), MS_SYNC)) {
                        utils_error(msync_err_str, true);
                    }
                }

                // we are going into the CRS loop which is out of AFL's control
                crs_loop = true;

                // clear shared memory
                {
                    register uintptr_t dst asm("rdi") = (uintptr_t)AFL_MAP_ADDR;
                    register uintptr_t n asm("rcx") = (uintptr_t)AFL_MAP_SIZE;
#ifdef AVX512
                    // (AVX512F version)
                    asm volatile(
                        ".intel_syntax noprefix\n"
                        "  xor rax, rax;\n"
                        "  vpbroadcastd zmm16, eax;\n"
                        "  lea rax, [rdi + rcx];\n"
                        "  sub rdi, rax;\n"
                        "loop:\n"
                        "  vmovdqa64 [rax + rdi], zmm16;\n"
                        "  add rdi, 0x40;\n"
                        "  jnz loop;\n"
                        :
                        : "r"(dst), "r"(n)
                        : "rax", "zmm16");
#else
                    // (SSE version)
                    asm volatile(
                        ".intel_syntax noprefix\n"
                        "  xorps xmm0, xmm0;\n"
                        "  lea rax, [rdi + rcx];\n"
                        "  sub rdi, rax;\n"
                        "loop:\n"
                        "  movdqa [rax + rdi], xmm0;\n"
                        "  add rdi, 0x10;\n"
                        "  jnz loop;\n"
                        :
                        : "r"(dst), "r"(n)
                        : "rax", "xmm0");
#endif
                }

                // go into CRS loop
                continue;
            }

            // If the program has reached this part, it indicates a real
            // crash has occured. Here, we need to reset client_status as
            // SIGSEGV or SIGILL, here we choose SIGSEGV
            client_status = 139;
        }

        // step (7.6). handle any other situation which is not caused by
        // patching
        //      [if: AFL_ATTCHED]: notify AFL and loop
        //      [if: !AFL_ATTACHED]: exit as normal or kill self with the same
        //      signal
        crs_loop = false;
        if (afl_attached) {
            sys_write(AFL_FORKSRV_FD + 1, (char *)&client_status, 4);
        } else {
            // notify the daemon is exited normally
            sys_write(CRS_COMM_FD, (char *)&client_status, 4);
            if (WIFEXITED(client_status)) {
                sys_exit(WEXITSTATUS(client_status));
            } else if (WIFSIGNALED(client_status)) {
                // XXX: if the daemon already identified this crash, it will
                // stop automatically
                sys_kill(0, WTERMSIG(client_status));
            } else {
                sys_kill(0, WSTOPSIG(client_status));
            }
        }
    }

    return;
}
