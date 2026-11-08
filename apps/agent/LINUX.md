# Running tachyon-agent on Linux

The Linux agent (`tachyon-agent --backend perf`) samples with `perf_event_open`: a software cpu-clock event with
frame-pointer callchains, plus the `sched` tracepoints for thread states. There is no eBPF and no kernel module.
This page covers what the kernel has to allow, how to give the agent the right privileges, and how the SDK reaches
the agent. `tachyon-cli doctor` checks everything below and prints the fix for what it finds
(see [apps/cli/README.md](../cli/README.md)).

Quick start:

```sh
tachyon-cli doctor                                       # what is missing on this machine?
sudo setcap cap_perfmon,cap_sys_ptrace+ep tachyon-agent  # Linux 5.8 or newer
tachyon-agent --backend perf &
tachyon-cli capture --launch ./game --duration 30
```

## Privileges

### File capabilities (recommended)

```sh
sudo setcap cap_perfmon,cap_sys_ptrace+ep /path/to/tachyon-agent
getcap /path/to/tachyon-agent        # tachyon-agent cap_sys_ptrace,cap_perfmon=ep
```

* `CAP_PERFMON` (Linux 5.8+) allows system-wide sampling, kernel samples and the scheduler tracepoints regardless of
  `kernel.perf_event_paranoid`, and exempts the ring buffers from the `perf_event_mlock_kb` budget.
* `CAP_SYS_PTRACE` is needed because `perf_event_open` on a task of another user (attach mode, and a launch
  that changes uid) goes through the kernel's ptrace access check, which `CAP_PERFMON` does not cover.
* **Kernels older than 5.8 have no `CAP_PERFMON`.** Use `sudo setcap cap_sys_admin,cap_sys_ptrace+ep tachyon-agent`
  there (or run the agent as root). The agent accepts `CAP_SYS_ADMIN` in place of `CAP_PERFMON`.
* The `e` (effective) flag matters: `+p` alone gives the agent nothing until it raises the capability itself, which
  it does not do. The capability lives in an extended attribute, so it is lost whenever the file is replaced
  (rebuild, `cp`, package upgrade): run `setcap` again. The file system must support `security.*` xattrs and must
  not be mounted `nosuid`.
* A process started by the agent does not inherit the capabilities, but it does run as the agent's user. That is
  the reason to prefer `setcap` over `sudo tachyon-agent`: under `sudo` the profiled program runs as root, and an
  SDK in it connects to a root-owned socket (see "SDK transport" below).

The agent refuses to start the `perf` backend (exit code 3, `ENOPRIV`) unless it is root, holds `CAP_PERFMON` or
`CAP_SYS_ADMIN`, or `kernel.perf_event_paranoid` is 0 or lower.

### kernel.perf_event_paranoid

The sysctl limits what a process **without** `CAP_PERFMON`/`CAP_SYS_ADMIN` may do. Root and capable processes
ignore it.

| value | what an unprivileged process may do | Tachyon |
|---|---|---|
| `-1` | every event, no `perf_event_mlock_kb` limit | everything works: system captures, scheduler tracepoints |
| `0` | system-wide cpu events and kernel profiling, but no raw tracepoints | system captures work; scheduler events fall back to context-switch records (no wait reasons, no blocking stacks, no wakers) |
| `1` | its own processes, with kernel samples | no system capture (per-process only) |
| `2` (common default) | its own processes, user-space samples only | no system capture; per-process captures without kernel frames |
| `3` and above (Debian/Ubuntu kernels) | nothing | no capture without a capability |

Change it until the next reboot with `sudo sysctl kernel.perf_event_paranoid=-1`, permanently with a file in
`/etc/sysctl.d/`:

```
kernel.perf_event_paranoid = -1
```

Prefer the capability: the sysctl opens perf to every user of the machine.

### Sampling rate and throttling

* `kernel.perf_event_max_sample_rate` (often 100000, but lowered by the kernel on its own) must be at least the
  requested `--hz` (default 8000), otherwise `perf_event_open` fails with `EINVAL`. Raise it with
  `sudo sysctl kernel.perf_event_max_sample_rate=16000`.
* `kernel.perf_cpu_time_max_percent` (default 25) is the share of cpu time the kernel lets perf sampling use. When
  sampling exceeds it, the kernel lowers `perf_event_max_sample_rate` and throttles events, which the capture
  reports as a throttle warning and which leaves gaps in the samples. A value of 0 (or 100) disables the check.
* `kernel.perf_event_mlock_kb` (default 516) is the budget of locked memory per user for perf ring buffers when
  the process is neither privileged nor under paranoid `-1`. The agent maps about 1 MiB per cpu
  (`--perf-ring-pages`, default 256 pages), so an unprivileged system capture on 8 cpus needs about 8.2 MiB:
  `sudo sysctl kernel.perf_event_mlock_kb=8224`. `RLIMIT_MEMLOCK` also applies; `ulimit -l unlimited` lifts it.
* File descriptors: a system capture holds one descriptor per cpu for sampling and up to two more per cpu for the
  scheduler tracepoints. A per-process capture without `CAP_PERFMON` needs threads x cpus. Raise `ulimit -n` before
  starting the agent on machines with many cpus.

### tracefs and the scheduler tracepoints

Thread states, blocking stacks and wakers come from the `sched_switch` and `sched_waking` tracepoints. The agent
reads their ids from tracefs:

```sh
sudo mount -t tracefs nodev /sys/kernel/tracing      # if not mounted (older systems: /sys/kernel/debug/tracing)
ls /sys/kernel/tracing/events/sched/sched_switch/id   # must be readable by the agent's user
```

The directory is root-only by default. For a non-root agent:

```sh
sudo groupadd -f tracing && sudo usermod -aG tracing "$USER"        # log in again afterwards
sudo chgrp -R tracing /sys/kernel/tracing && sudo chmod -R g+rX /sys/kernel/tracing
```

Mount options (`mount -t tracefs -o gid=tracing,mode=0750 nodev /sys/kernel/tracing`) make this survive a remount;
an `/etc/fstab` line makes it survive a reboot. Opening the tracepoints also needs `CAP_PERFMON`, root or paranoid
`-1`. Without tracefs, or without that permission, the capture falls back to `PERF_RECORD_SWITCH`
records: you still see which thread ran on which cpu, but not why it stopped, and no wakers. The capture's
warnings say which mode was used.

### Containers

* `perf_event_open` is blocked by Docker's default seccomp profile. Run with
  `--security-opt seccomp=unconfined` (or a profile that allows `perf_event_open`).
* Add the capabilities: `--cap-add=PERFMON --cap-add=SYS_PTRACE` (Docker 20.10+ and a 5.8+ host kernel; older hosts:
  `--cap-add=SYS_ADMIN`). Kubernetes: `securityContext.capabilities.add: ["PERFMON", "SYS_PTRACE"]`.
* The sysctls are those of the host. They cannot be set inside an unprivileged container.
* For a system-wide capture the container needs the host's pid namespace (`--pid=host`) to see the processes; module
  paths of other containers' mount namespaces cannot be read, which shows as unresolved frames.
* Mount tracefs into the container if you need scheduler events: `-v /sys/kernel/tracing:/sys/kernel/tracing:ro`.
* A profiled program inside a container reaches an agent outside it by sharing the socket directory
  (`-v /run/user/1000:/run/user/1000`, same uid) or through `TACHYON_AGENT_PIPE`.

## SDK transport

The control socket is a Unix stream socket: `$TACHYON_AGENT_PIPE`, else `/run/user/<uid>/tachyon-agent.sock`, else
`/tmp/tachyon-agent.sock` when `/run/user/<uid>` does not exist. `--socket PATH` overrides it for the agent and the
CLI. The path must be shorter than 108 bytes.

* The socket file is created with mode `0600`, owned by the agent's user. Put it in a private directory (`0700`)
  when the machine is shared: `/tmp` is not private.
* **An agent running as root for another user** (the viewer's "Start with pkexec", `sudo tachyon-agent`) hands the
  control socket and every per-process SDK socket to that user with `chown` and leaves the modes at `0600`, so
  the unprivileged viewer, CLI and instrumented programs can connect and nobody else can. The user is, in this
  order: `--socket-owner UID` (the viewer passes its own uid), `$PKEXEC_UID`, `$SUDO_UID` (only honoured when
  the agent's effective uid is 0; uid 0 is never accepted). The group is the user's primary group from the
  account database. Run the agent with `--socket`/`TACHYON_AGENT_PIPE` pointing into a directory that user can
  enter (the default path of a root process is `/run/user/0/...`, which the user cannot). An agent started as
  root by other means (a service, `su`) has no invoking user: pass `--socket-owner`, otherwise the socket stays
  root's and the viewer reports "permission denied".
* An SDK client sends `inst_hello` with its pid on the control socket; the agent answers with a per-process
  socket `tachyon-inst-<pid>.sock` in the same directory (also `0600`), which the SDK then streams to.
  The agent checks the connecting process with `SO_PEERCRED`: the peer pid has to be the pid of the `inst_hello`,
  or the connection is dropped and counted as refused in the capture's warnings.
* `inst_hello` is answered only while a capture is starting or running, and only for processes that belong to it
  (the launched or attached process; any pid in a system capture; descendants with `--follow-children`).
* The SDK and the agent therefore have to run as the same user, or the agent has to hand its sockets to the
  SDK's user (`--socket-owner`, above). A program started under a third account cannot talk to this agent. With
  `sudo tachyon-agent` and a *launched* program, the program also runs as root; prefer `setcap` when the agent
  launches programs.
* Without a reachable agent the SDK writes a sidecar file `<exe>-<pid>.tachyon-inst` in the current directory (set
  `TACHYON_SIDECAR=off` to disable it, or a path to choose it). `tachyon-cli merge` adds it to a trace afterwards.
  A capture that shows no scopes while the program was instrumented usually means the SDK never reached the
  agent: look for the sidecar file, and check the socket path both sides use.

## Live view

A `start` with `"live":true` (`tachyon-cli live ...`, or the viewer's Live document) makes the perf backend decode
the records the drain thread reads as they arrive, in addition to writing the spool for the final trace. The
decoder (`libs/capture/linux_perf/live_source.h`) runs on its own thread behind a bounded queue (64 MiB of
sample and switch records, 256 MiB of FORK / COMM / MMAP / EXIT records): when it falls behind it drops live
sample and switch records, counts them (`dropped` in the live stream, `onDropped` in the sink) and never touches
the spool, so the final trace is the same with or without `live`. The decoder keeps its own process, thread and
address-space tables from the same records and the `/proc` rundown the converter uses; a module is reported the
first time its process shows up, with the same identity rules (GNU build-id, the ELF load base).

`--live-window-seconds N` (default 30) is how much the agent keeps for a client that subscribes late: the
keyframe it gets holds every table definition plus the last N seconds of batches. Batches are cut every 250 ms
or 256 KiB; a client that cannot keep up gets `live_gap` and resubscribes. Cost: one more memcpy of the ring
data per drain read plus the decode (measured: 2.2% of one cpu for the testbed's `spin` at 7.6 kHz of one
thread), and `window x rate x 20` bytes of retained batches (about 80 MB for 30 s of 16 threads at 8 kHz).

## Remote capture (TCP)

```sh
head -c 24 /dev/urandom | base64 > agent.token && chmod 600 agent.token     # 32 characters
tachyon-agent --backend perf --listen 0.0.0.0:7331 --token-file agent.token
```

`--listen HOST:PORT` adds a TCP listener next to the local socket (both serve the same agent). The host has to be
named: `127.0.0.1`, an IPv4 or `[IPv6]` literal, a host name, or `0.0.0.0` / `[::]` to listen on every interface.
The startup log prints the bound address (`listening on TCP 0.0.0.0:7331 (unencrypted, ...)`) and warns about
non-loopback binds. Without `--listen` there is no network listener.

* **Not encrypted.** The channel is plain TCP; the token is the only protection and a bearer secret that anyone
  who can read the traffic can replay. Use a trusted network, or tunnel it: `ssh -L 7331:localhost:7331 buildbox`
  and connect to `localhost:7331` (start the agent with `--listen 127.0.0.1:7331`). TLS is not in the dependency
  list for v1.
* **Token.** `--token-file PATH` is required with `--listen`. The agent refuses to start the TCP listener when the
  file is missing, empty, holds fewer than 32 characters in its first line, contains blanks, or (POSIX) is
  readable or writable by group or others. The first line a TCP client sends must be
  `{"cmd":"auth","token":"..."}` within 5 seconds; it is compared in constant time. A failure gets an `ENOPRIV`
  error response and the connection is closed. The peer address of every failure is logged, the token never.
  More than 5 failures from one address within a minute lock that address out for the rest of the minute. Until a
  client has authenticated it receives no events, and at most 16 unauthenticated connections are held at once.
  Local socket / pipe clients never authenticate.
* **Remote clients are restricted.** `start` with mode launch is refused with `ENOPRIV` unless the agent was
  started with `--allow-remote-launch` (it lets anyone holding the token run programs as the agent's user).
  Attach and system captures are allowed. A remote client cannot choose where traces go: `out_path` is reduced to
  a file name inside `--remote-out-dir` (default: the agent's working directory), existing names get a `-N`
  suffix. The agent refuses `--listen` when that directory is writable by group or others without the sticky
  bit, or owned by another user (a local user could swap a trace for a symlink): use a directory you own.
  `inst_hello` is accepted from local clients only.
* **`fetch`.** `{"cmd":"fetch","path","offset","length"}` streams a trace in chunks of at most 512 KiB (base64 in
  the JSON line), with the SHA-256 of the whole file on the final chunk. Only files the agent produced in this
  process lifetime are served (capture outputs and flushes). The requested path is canonicalised (`..` and every
  symlink resolved) and must equal a produced file, so traversal and symlinks out of the output directory get
  `ENOTFOUND`, the same answer as for a missing file. `tachyon-cli fetch` and the viewer resume from
  `<name>.part` and verify the checksum.
* **`processes`** lists the agent machine's processes (pid, name, user, command line capped at 256 characters) for
  the viewer's Attach dialog.
* **Symbols stay on the viewer's side.** Modules are identified by GNU build-id (PDB GUID + age on Windows), not by
  path, so the remote paths in the trace do not matter. Put the debug files where the viewer looks:
  a `.build-id/xx/rest.debug` tree in a symbol search path or debug directory, `<image>.debug` in a search path, a
  symbol server (debuginfod). A `.gnu_debuglink` can only be followed when the image itself is on the viewer's
  machine (the link name lives in the image), so a debug file known only by its debuglink name is not found for a
  remote trace.
* **Windows.** The same options work with the ETW backend (`--listen`, `--token-file`); the token file's
  permissions are not checked there, so keep it in a profile directory. Open the port in the firewall for the
  workstation only.

## What `tachyon-cli doctor` reports

| check id | what it looks at | FAIL when |
|---|---|---|
| `kernel_version` | `/proc/sys/kernel/osrelease` | older than 4.3 (WARN before 5.8: no `CAP_PERFMON`) |
| `process_capabilities` | `CapEff` of the doctor process: `CAP_PERFMON`, `CAP_SYS_ADMIN`, `CAP_SYS_PTRACE` | never (WARN without a perf capability, unless paranoid is -1) |
| `agent_capabilities` | the `security.capability` xattr and the setuid bit of `tachyon-agent` | never (WARN: not found, no caps, `+p` without `e`, no `CAP_SYS_PTRACE`, no xattr support) |
| `perf_event_paranoid` | the sysctl, and the table above | 3 or higher with neither process nor agent privileged |
| `max_sample_rate` | `perf_event_max_sample_rate` against `--hz` | the limit is below `--hz` |
| `cpu_time_max_percent` | `perf_cpu_time_max_percent` | never (WARN below 10) |
| `perf_event_mlock_kb` | the budget against 1 MiB per cpu | never (WARN, only without privileges) |
| `tracefs` | readable `sched_switch/id` and `sched_waking/id` (or `sched_wakeup`) | never (WARN: fallback to context-switch records) |
| `perf_open_system` | a trial cpu-clock event with callchains on cpu 0 | it fails together with the per-process trial |
| `perf_open_process` | the same event for the calling process | it fails together with the system-wide trial |
| `rlimit_nofile` | `ulimit -n` against 3 descriptors per cpu | below one per cpu plus 32 |
| `rlimit_memlock` | `ulimit -l` against the ring buffers (unprivileged only) | never |
| `ptrace_scope` | `kernel.yama.ptrace_scope` (attach mode) | never (WARN at 2 and above without `CAP_SYS_PTRACE`) |
| `socket_dir` | the agent socket's directory exists and is writable; path length | not writable, or path too long |
| `symbols` | `$DEBUGINFOD_URLS`, `/usr/lib/debug/.build-id`, `~/.debug/.build-id` | never (WARN: system libraries will lack names) |

The doctor tests with the privileges of its own process. If the agent has file capabilities and the doctor does
not, the trial failures are warnings (run `sudo tachyon-cli doctor` to test as root). Exit code: 0 when nothing
failed, 1 when any check printed FAIL.
