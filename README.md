#Linux Kernel Recompilation on Debian 6 with OpenVZ+utrace Patch

This document details the procedure for enabling utrace and OpenVZ from within the kernel. It uses two patch-sets, OpenVZ and utrace, to recompile the kernel.

###Install build dependencies
apt-get install libncurses5-dev gcc make git exuberant-ctags libc6-dev fakeroot kernel-package build-essential

###Download linux-kernel from kernel.org

	# git clone git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
	# chmod -R a-s linux-stable 
	These permissions are required whenever compiling from git
Create a branch from `v2.6.32` tag.

	# git checkout -b v2.6.32-openvz-2.6.32-afanasyev.1-utrace-fche-2.6.32 v2.6.32
	Switched to a new branch 'v2.6.32.28-openvz-2.6.32-afanasyev.1-utrace-fche-2.6.32'
	
###Download, apply OpenVZ patch and commit
			
	# wget http://download.openvz.org/kernel/branches/2.6.32/2.6.32-afanasyev.1/patches/patch-afanasyev.1-combined.gz
Apply the patch

	# gzip -dc patch-afanasyev.1-combined.gz | patch -p1		
Commit

	# git add -A
	# git commit -a -m "openvz patch applied to v2.6.32"
	# git log -1 --pretty=oneline
	ec66872ce417a13f617479b98624c963aa561639 openvz patch applied to v2.6.32
	
###Download and apply utrace patches. Resolve merge conflicts manually.

Download

	wget http://web.elastic.org/~fche/frob-utrace/2.6.32/tracehook.patch
	wget http://web.elastic.org/~fche/frob-utrace/2.6.32/utrace.patch
	wget http://web.elastic.org/~fche/frob-utrace/2.6.32/utrace-ptrace.patch
	
Patch and resolve merge-conflicts

	# patch -p1 < ../tracehook.patch
	patching file arch/powerpc/include/asm/ptrace.h
	patching file arch/powerpc/kernel/traps.c
	patching file arch/s390/kernel/traps.c
	patching file arch/x86/include/asm/ptrace.h
	patching file arch/x86/kernel/ptrace.c
	patching file include/linux/ptrace.h
	patching file include/linux/sched.h
	Hunk #1 succeeded at 2169 (offset 109 lines).
	patching file include/linux/tracehook.h
	patching file kernel/ptrace.c
	Hunk #1 succeeded at 282 (offset 11 lines).
	patching file kernel/signal.c
	Hunk #1 succeeded at 1509 (offset 48 lines).
	Hunk #2 succeeded at 1781 (offset 50 lines).
	Hunk #3 succeeded at 1855 (offset 48 lines).
	Hunk #4 succeeded at 1866 (offset 48 lines).
	
	# patch -p1 < ../utrace.patch
	patching file Documentation/DocBook/Makefile
	patching file Documentation/DocBook/utrace.tmpl
	patching file fs/proc/array.c
	Hunk #1 succeeded at 82 with fuzz 1.
	Hunk #2 succeeded at 205 (offset 15 lines).
	patching file include/linux/sched.h
	Hunk #1 succeeded at 1428 (offset 35 lines).
	patching file include/linux/tracehook.h
	patching file include/linux/utrace.h
	patching file init/Kconfig
	patching file kernel/Makefile
	Hunk #1 succeeded at 76 (offset 8 lines).
	patching file kernel/fork.c
	Hunk #1 succeeded at 160 (offset 8 lines).
	Hunk #2 succeeded at 1059 (offset 40 lines).
	patching file kernel/ptrace.c
	Hunk #2 succeeded at 173 (offset 8 lines).
	Hunk #3 succeeded at 204 (offset 8 lines).
	Hunk #4 succeeded at 246 (offset 11 lines).
	patching file kernel/utrace.c

	# patch -p1 < ../utrace-ptrace.patch
	patching file include/linux/ptrace.h
	patching file kernel/Makefile
	Hunk #1 succeeded at 77 (offset 8 lines).
	patching file kernel/ptrace-utrace.c
	patching file kernel/ptrace.c
	Hunk #3 FAILED at 379.
	Hunk #4 succeeded at 482 (offset 8 lines).
	Hunk #5 succeeded at 522 (offset 11 lines).
	Hunk #6 succeeded at 541 (offset 11 lines).
	Hunk #7 FAILED at 583.
	Hunk #8 FAILED at 816.
	Hunk #9 succeeded at 970 (offset 16 lines).
	3 out of 9 hunks FAILED -- saving rejects to file kernel/ptrace.c.rej
	patching file kernel/utrace.c
	
Resolve manually (using `kernel/ptrace.c.rej` file) and commit.
	
	# vim kernel/ptrace.c
	# git add -A
	# git commit -a -m "utrace patch applied after applying openvz patch from previous commit"
	# git log -2 --pretty=oneline
	34eced89a3afc2a21485e0e6fddde04aa0029bc4 utrace patch applied after 	applying openvz patch from previous commit
	ec66872ce417a13f617479b98624c963aa561639 openvz patch applied to v2.6.32
	
###Configure and compile

	cp /boot/config-`uname -r` .config
	make oldconfig 
	make menuconfig

Configurations	

1. turn on "kernel hacking-->Compile the kernel with debug info".

2. turn on relayfs support:  
General setup-->Kernel->user space relay support (formerly relayfs)

2. turn on kprobe:  
General setup-->Kprobes

3. Disable checkpointing  
OpenVZ  --->Checkpointing & restoring Virtual Environments

Compile

	#make-kpkg --initrd --append-to-version -openvz-utrace --revision=1 kernel_image kernel_headers kernel_debug
	
Install and reboot into new kernel

	# cd ..
	# dpkg -i linux-image-2.6.32.10-openvz-utrace_1_amd64.deb 
	# dpkg -i linux-headers-2.6.32.10-openvz-utrace_1_amd64.deb 
	# dpkg -i linux-image-2.6.32.10-openvz-utrace-dbg_1_amd64.deb 
	
Install systemtap from source

1. git clone git://sourceware.org/git/systemtap.git
2. apt-get install elfutils	
3. apt-get build-dep systemtap
4. ./configure
5. make
6. sudo make install
7. Check by running as root: `stap -vvv -e 'probe begin { println("hello world") exit () }'`

###Steps to reproduce

1. In /usr/src/ run: `git clone https://github.com/faarshad/linux-stable.git`  
   `chmod -R a-s linux-stable `
2. `git checkout v2.6.32-openvz-2.6.32-afanasyev.1-utrace-fche-2.6.32`
3. `cp config .config`
4. `make-kpkg --initrd --append-to-version -openvz-utrace --revision=1 kernel_image kernel_headers kernel_debug`
5. 	`dpkg -i ../linux-image-2.6.32.28-openvz-utrace_1_amd64.deb`  
	`dpkg -i ../linux-headers-2.6.32.28-openvz-utrace_1_amd64.deb`  
   	`dpkg -i ../linux-image-2.6.32.28-openvz-utrace-dbg_1_amd64.deb` 
6. Reboot in the new kernel.   	
7. Install systemtap from source.
