# ps_mem-c

C port of [pixelb/ps_mem](https://github.com/pixelb/ps_mem) with `smaps_rollup` optimization.

## Why

The original C binary reads `/proc/<pid>/smaps` per process — thousands of lines per process, every run. Linux 4.4+ exposes `/proc/<pid>/smaps_rollup`: same aggregated data, one read. This port uses rollup when available.

## Performance

Benchmarked on RHEL 9, x86_64, ~300 processes:

| Implementation | Avg wall time | Speedup |
|---|---|---|
| Original C (full smaps) | ~1.05s | 1× baseline |
| Python v3.14 (smaps_rollup) | ~0.61s | 1.7× |
| **This build (smaps_rollup)** | **~0.47s** | **2.2×** |

Runtime is dominated by kernel `/proc` I/O (`sys` time). The rollup optimization cuts I/O proportionally to process count.

## Build

```sh
gcc -O2 -o ps_mem ps_mem.c
sudo cp ps_mem /usr/bin/ps_mem
```

Requires: Linux 3.2+ (glibc 2.2.5+). `smaps_rollup` fallback to full `smaps` on older kernels.

## Usage

```
Usage: ps_mem [OPTION]...
Show program core memory usage

  -h, -help                   Show this help
  -p <pid>[,pid2,...pidN]     Only show memory usage PIDs in the specified list
  -s, --split-args            Show and separate by, all command line arguments
  -t, --total                 Show only the total value
  -w <N>                      Measure and show process memory every N seconds
```

Requires root (reads `/proc/<pid>/smaps_rollup`).

## Output Examples

### Default — per-program RAM usage, sorted ascending

```
$ sudo ps_mem

 Private  +   Shared  =  RAM used	Program

  7.0 MiB + 475.5 KiB =   7.4 MiB	tracker-miner-fs-3
  5.2 MiB +   2.4 MiB =   7.6 MiB	gjs-console (2)
  7.3 MiB +   1.2 MiB =   8.5 MiB	bash (5)
  8.3 MiB + 354.5 KiB =   8.7 MiB	Xwayland
 11.3 MiB + 908.5 KiB =  12.2 MiB	firewalld
 12.5 MiB + 253.5 KiB =  12.7 MiB	gnome-software
 12.4 MiB + 470.0 KiB =  12.9 MiB	anydesk (2)
 11.4 MiB +   2.9 MiB =  14.2 MiB	gnome-terminal-server
 35.4 MiB +   7.5 KiB =  35.4 MiB	containerd
 44.7 MiB +   2.6 MiB =  47.2 MiB	dnf
139.5 MiB +   5.2 MiB = 144.7 MiB	gnome-shell
128.7 MiB +  22.8 MiB = 151.5 MiB	node-MainThread (6)
176.4 MiB +  16.5 KiB = 176.4 MiB	dockerd
206.0 MiB + 128.2 MiB = 334.2 MiB	chrome (8)
304.4 MiB +  47.2 MiB = 351.7 MiB	slack (8)
564.9 MiB +  26.0 KiB = 565.0 MiB	claude (2)
  1.0 GiB + 141.5 MiB =   1.1 GiB	brave (32)
---------------------------------
                          3.1 GiB
=================================
```

`(N)` = N processes grouped under same program name.  
Footer total shown only when PSS data available (kernel 2.6.23+).

### `-t` — total RAM used only (bytes, for scripting)

```
$ sudo ps_mem -t
2530798080
```

### `-p` — specific PIDs only

```
$ sudo ps_mem -p 1
 Private  +   Shared  =  RAM used	Program

  2.7 MiB +   1.4 MiB =   4.1 MiB	systemd
---------------------------------
                          4.1 MiB
=================================
```

### `-s` — split by full command line (no grouping)

```
$ sudo ps_mem -s
 Private  +   Shared  =  RAM used	Program

  4.0 KiB +  10.5 KiB =  14.5 KiB	fusermount -o rw,nosuid,nodev,...
  4.0 KiB +  11.5 KiB =  15.5 KiB	/opt/brave.com/brave/chrome_crashpad_handler ...
  ...
```

### `-w N` — watch mode, refresh every N seconds

```
$ sudo ps_mem -w 5
```

## How It Works

For each `/proc/<pid>`:
1. Reads `smaps_rollup` (or `smaps` fallback) for Private/Shared/PSS memory
2. Resolves program name via `exe` symlink + `status`
3. Groups processes by program name, aggregates private RAM
4. PSS (Proportional Set Size) used for shared RAM when available — avoids double-counting shared libraries
5. CLONE_VM detection: processes with identical smaps fingerprint counted once

## Columns

| Column | Meaning |
|---|---|
| Private | RAM exclusively used by this program |
| Shared | Proportional share of shared/mapped memory (PSS) |
| RAM used | Private + Shared |

## Files

| File | Description |
|---|---|
| `ps_mem.c` | C source |
| `ps_mem.py` | Upstream Python v3.14 (reference) |

## License

LGPLv2 — same as upstream [pixelb/ps_mem](https://github.com/pixelb/ps_mem).
