/*
 * init.c
 * 
 * This is the install type init 
 *
 * Erik Troan (ewt@redhat.com)
 *
 * Copyright 1996 - 2002 Red Hat Software 
 *
 * This software may be freely redistributed under the terms of the GNU
 * public license.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#if USE_MINILIBC
#include "minilibc.h"
#ifndef SOCK_STREAM
# define SOCK_STREAM 1
#endif 
#else
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <termios.h>

/* Need to tell loop.h what the actual dev_t type is. */
#undef dev_t
#if defined(__alpha) || (defined(__sparc__) && defined(__arch64__))
#define dev_t unsigned int
#else
#define dev_t unsigned short
#endif
#include <linux/loop.h>
#undef dev_t
#define dev_t dev_t

#define syslog klogctl
#endif

struct unmountInfo {
    char * name;
    int mounted;
    int loopDevice;
    enum { FS, LOOP } what;
} ;

#include <linux/cdrom.h>

#define KICK_FLOPPY     1
#define KICK_BOOTP	2

#define MS_REMOUNT      32

#define ENV_PATH 		0
#define ENV_LD_LIBRARY_PATH 	1
#define ENV_HOME		2
#define ENV_TERM		3
#define ENV_DEBUG		4

char * env[] = {
    "PATH=/usr/bin:/bin:/sbin:/usr/sbin:/mnt/sysimage/bin:"
    "/mnt/sysimage/usr/bin:/mnt/sysimage/usr/sbin:/mnt/sysimage/sbin",
    /* we set a nicer ld library path specifically for bash -- a full
       one makes anaconda unhappy */
    "LD_LIBRARY_PATH=/lib:/usr/lib:/usr/X11R6/lib",
    "HOME=/",
    "TERM=linux",
    "DEBUG=",
    "TERMINFO=/etc/linux-terminfo",
    "PYTHONPATH=/tmp/updates",
    NULL
};


/* 
 * this needs to handle the following cases:
 *
 *	1) run from a CD root filesystem
 *	2) run from a read only nfs rooted filesystem
 *      3) run from a floppy
 *	4) run from a floppy that's been loaded into a ramdisk 
 *
 */

int testing=0;

void printstr(char * string) {
    write(1, string, strlen(string));
}

void fatal_error(int usePerror) {
/* FIXME */
#if 0
    if (usePerror) 
	perror("failed:");
    else
#endif
	printf("failed.\n");

    printf("\nI can't recover from this.\n");
    if (testing) exit(0);
#if !defined(__s390__) && !defined(__s390x__)
    while (1) ;
#endif
}

int doMke2fs(char * device, char * size) {
    char * args[] = { "/usr/sbin/mke2fs", NULL, NULL, NULL };
    int pid, status;

    args[1] = device;
    args[2] = size;

    if (!(pid = fork())) {
	/* child */
	execve("/usr/sbin/mke2fs", args, env);
	fatal_error(1);
    }

    wait4(-1, &status, 0, NULL);
    
    return 0;
}

int hasNetConfiged(void) {
    int rc;
    int s;
    struct ifconf configs;
    struct ifreq devs[10];

    #ifdef __i386__
	return 0;
    #endif

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
	/* FIXME was perror*/
	printf("error creating socket: %d\n", errno);
	return 0;
    } else {
	/* this is just good enough to tell us if we have anything 
	   configured */
	configs.ifc_len = sizeof(devs);
	configs.ifc_buf = (void *) devs;

	rc = ioctl(s, SIOCGIFCONF, &configs);
	if (rc < 0) {
	    /* FIXME was perror*/
	    printstr("SIOCGIFCONF");
	    return 0;
	}
	if (configs.ifc_len == 0) {
	    return 0;
	}

	return 1;
    }

    return 0;
}

void doklog(char * fn) {
    fd_set readset, unixs;
    int in, out, i;
    int log;
    int s;
    int sock = -1;
    struct sockaddr_un sockaddr;
    char buf[1024];
    int readfd;

    in = open("/proc/kmsg", O_RDONLY,0);
    if (in < 0) {
	/* FIXME: was perror */
	printstr("open /proc/kmsg");
	return;
    }

    out = open(fn, O_WRONLY, 0);
    if (out < 0) 
	printf("couldn't open %s for syslog -- still using /tmp/syslog\n", fn);

    log = open("/tmp/syslog", O_WRONLY | O_CREAT, 0644);
    if (log < 0) {
	/* FIXME: was perror */
	printstr("error opening /tmp/syslog");
	sleep(5);
	
	close(in);
	return;
    }

    /* if we get this far, we should be in good shape */

    if (fork()) {
	/* parent */
	close(in);
	close(out);
	close(log);
	return;
    }
#ifdef INCLUDE_RESCUE
    if(ioctl(0, TIOCNOTTY, (char *)0)) {
	printstr("ioctl(0, TIOCNOTTY, NULL) failed\n");
    }
#endif /* INCLUDE_RESCUE */
    close(0); 
    close(1);
    close(2);

    dup2(1, log);

#if defined(USE_LOGDEV)
    /* now open the syslog socket */
    sockaddr.sun_family = AF_UNIX;
    strcpy(sockaddr.sun_path, "/dev/log");
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
	printf("error creating socket: %d\n", errno);
	sleep(5);
    }
    printstr("got socket\n");
    if (bind(sock, (struct sockaddr *) &sockaddr, sizeof(sockaddr.sun_family) + 
			strlen(sockaddr.sun_path))) {
	printf("bind error: %d\n", errno);
	sleep(5);
    }
    printstr("bound socket\n");
    chmod("/dev/log", 0666);
    if (listen(sock, 5)) {
	printf("listen error: %d\n", errno);
	sleep(5);
    }
#endif

    syslog(8, NULL, 1);

    FD_ZERO(&unixs);
    while (1) {
	memcpy(&readset, &unixs, sizeof(unixs));

	if (sock >= 0) FD_SET(sock, &readset);
	FD_SET(in, &readset);

	i = select(20, &readset, NULL, NULL, NULL);
	if (i <= 0) continue;

	if (FD_ISSET(in, &readset)) {
	    i = read(in, buf, sizeof(buf));
	    if (i > 0) {
		if (out >= 0) write(out, buf, i);
		write(log, buf, i);
	    }
	} 

	for (readfd = 0; readfd < 20; ++readfd) {
	    if (FD_ISSET(readfd, &readset) && FD_ISSET(readfd, &unixs)) {
		i = read(readfd, buf, sizeof(buf));
		if (i > 0) {
		    if (out >= 0) {
			write(out, buf, i);
			write(out, "\n", 1);
		    }

		    write(log, buf, i);
		    write(log, "\n", 1);
		} else if (i == 0) {
		    /* socket closed */
		    close(readfd);
		    FD_CLR(readfd, &unixs);
		}
	    }
	}

	if (sock >= 0 && FD_ISSET(sock, &readset)) {
	    s = sizeof(sockaddr);
	    readfd = accept(sock, (struct sockaddr *) &sockaddr, &s);
	    if (readfd < 0) {
		if (out >= 0) write(out, "error in accept\n", 16);
		write(log, "error in accept\n", 16);
		close(sock);
		sock = -1;
	    } else {
		FD_SET(readfd, &unixs);
	    }
	}
    }    
}

int setupTerminal(int fd) {
    struct winsize winsize;

    if (ioctl(fd, TIOCGWINSZ, &winsize)) {
	printf("failed to get winsize");
	fatal_error(1);
    }

    winsize.ws_row = 24;
    winsize.ws_col = 80;

    if (ioctl(fd, TIOCSWINSZ, &winsize)) {
	printf("failed to set winsize");
	fatal_error(1);
    }

    env[ENV_TERM] = "TERM=vt100";

    return 0;
}

void undoLoop(struct unmountInfo * fs, int numFs, int this);

void undoMount(struct unmountInfo * fs, int numFs, int this) {
    int len = strlen(fs[this].name);
    int i;

    if (!fs[this].mounted) return;
    fs[this].mounted = 0;

    /* unmount everything underneath this */
    for (i = 0; i < numFs; i++) {
	if (fs[i].name[len] == '/' && 
		!strncmp(fs[this].name, fs[i].name, len)) {
	    if (fs[i].what == LOOP)
		undoLoop(fs, numFs, i);
	    else
		undoMount(fs, numFs, i);
	}
    }

    printf("\t%s", fs[this].name);
    /* don't need to unmount /tmp.  it is busy anyway. */
    if (!testing) {
	if (umount(fs[this].name) < 0) {
	    printf(" umount failed (%d)", errno);
	} else {
	    printf(" done");
	}
    }
    printf("\n");
}

void undoLoop(struct unmountInfo * fs, int numFs, int this) {
    int i;
    int fd;

    if (!fs[this].mounted) return;
    fs[this].mounted = 0;

    /* find the device mount */
    for (i = 0; i < numFs; i++) {
	if (fs[i].what == FS && (fs[i].loopDevice == fs[this].loopDevice))
	    break;
    }

    if (i < numFs) {
	/* the device is mounted, unmount it (and recursively, anything
	 * underneath) */
	undoMount(fs, numFs, i);
    }

    unlink("/tmp/loop");
    mknod("/tmp/loop", 0600 | S_IFBLK, (7 << 8) | fs[this].loopDevice);
    printf("\tdisabling /dev/loop%d", fs[this].loopDevice);
    if ((fd = open("/tmp/loop", O_RDONLY, 0)) < 0) {
	printf(" failed to open device: %d", errno);
    } else {
	if (!testing && ioctl(fd, LOOP_CLR_FD, 0))
	    printf(" LOOP_CLR_FD failed: %d", errno);
	close(fd);
    }

    printf("\n");
}

void unmountFilesystems(void) {
    int fd, size;
    char buf[65535];			/* this should be big enough */
    char * chptr, * start;
    struct unmountInfo filesystems[500];
    int numFilesystems = 0;
    int i;
    struct loop_info li;
    char * device;
    struct stat sb;

    fd = open("/proc/mounts", O_RDONLY, 0);
    if (fd < 1) {
	/* FIXME: was perror */
	printstr("failed to open /proc/mounts");
	sleep(2);
	return;
    }

    size = read(fd, buf, sizeof(buf) - 1);
    buf[size] = '\0';

    close(fd);

    chptr = buf;
    while (*chptr) {
	device = chptr;
	while (*chptr != ' ') chptr++;
	*chptr++ = '\0';
	start = chptr;
	while (*chptr != ' ') chptr++;
	*chptr++ = '\0';

	if (strcmp(start, "/") && strcmp(start, "/tmp")) {
	    filesystems[numFilesystems].name = alloca(strlen(start) + 1);
	    strcpy(filesystems[numFilesystems].name, start);
	    filesystems[numFilesystems].what = FS;
	    filesystems[numFilesystems].mounted = 1;

	    stat(start, &sb);
	    if ((sb.st_dev >> 8) == 7) {
		filesystems[numFilesystems].loopDevice = sb.st_dev & 0xf;
	    } else {
		filesystems[numFilesystems].loopDevice = -1;
	    }

	    numFilesystems++;
	}

	while (*chptr != '\n') chptr++;
	chptr++;
    }

    for (i = 0; i < 7; i++) {
	unlink("/tmp/loop");
	mknod("/tmp/loop", 0600 | S_IFBLK, (7 << 8) | i);
	if ((fd = open("/tmp/loop", O_RDONLY, 0)) >= 0) {
	    if (!ioctl(fd, LOOP_GET_STATUS, &li) && li.lo_name[0]) {
		filesystems[numFilesystems].name = alloca(strlen(li.lo_name) 
								+ 1);
		strcpy(filesystems[numFilesystems].name, li.lo_name);
		filesystems[numFilesystems].what = LOOP;
		filesystems[numFilesystems].mounted = 1;
		filesystems[numFilesystems].loopDevice = i;
		numFilesystems++;
	    }

	    close(fd);
	}
    }

    for (i = 0; i < numFilesystems; i++) {
	if (filesystems[i].what == LOOP) {
	    undoLoop(filesystems, numFilesystems, i);
	}
    }

    for (i = 0; i < numFilesystems; i++) {
	if (filesystems[i].mounted) {
	    undoMount(filesystems, numFilesystems, i);
	}
    }
}

void disableSwap(void) {
    int fd;
    char buf[4096];
    int i;
    char * start;
    char * chptr;

    if ((fd = open("/proc/swaps", O_RDONLY, 0)) < 0) return;

    i = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (i < 0) return;
    buf[i] = '\0';

    start = buf;
    while (*start) {
	while (*start != '\n' && *start) start++;
	if (!*start) return;

	start++;
	if (*start != '/') return;
	chptr = start;
	while (*chptr && *chptr != ' ') chptr++;
	if (!(*chptr)) return;
	*chptr = '\0';
	printf("\t%s", start);
	if (swapoff(start)) 
	    printf(" failed (%d)", errno);
	printf("\n");

	start = chptr + 1;
    }
}

void ejectCdrom(void) {
  int ejectfd;
  struct stat sb;

  stat("/tmp/cdrom", &sb);

  if ((sb.st_mode & S_IFBLK) == S_IFBLK) {
    printf("ejecting /tmp/cdrom...");
    if ((ejectfd = open("/tmp/cdrom", O_RDONLY | O_NONBLOCK, 0)) >= 0) {
      if (ioctl(ejectfd, CDROMEJECT, 0))
	printf("eject failed %d ", errno);
      close(ejectfd);
    } else {
      printf("eject failed %d ", errno);
    }
    printf("\n");
  }
}

int mystrstr(char *str1, char *str2) {
    char *p;
    int rc=0;

    for (p=str1; *p; p++) {
	if (*p == *str2) {
	    char *s, *t;

	    rc = 1;
	    for (s=p, t=str2; *s && *t; s++, t++)
		if (*s != *t) {
		    rc = 0;
		    p++;
		}

	    if (rc)
		return rc;
	} 
    }
    return rc;
}



int main(int argc, char **argv) {
    pid_t installpid, childpid;
    int waitStatus;
    int fd;
    int nfsRoot = 0;
    int roRoot = 0;
    int cdRoot = 0;
    int doReboot = 0;
    int doShutdown =0;
    int isSerial = 0;
    int noKill = 0;
#ifdef __alpha__
    char * kernel;
#endif
    char * argvc[15];
    char ** argvp = argvc;
    char twelve = 12;
    int i;
    char buf[500];
    int len;


#if !defined(__s390__) && !defined(__s390x__)
    testing = (getppid() != 0) && (getppid() != 1);
#endif

    if (!testing) {
	/* turn off screen blanking */
	printstr("\033[9;0]");
	printstr("\033[8]");
    } else {
	printstr("(running in test mode).\n");
    }

#if 0
    printf("unmounting filesystems...\n"); 
    unmountFilesystems();
    exit(0);
#endif

    umask(022);

    printstr("Greetings.\n");

    printf("Red Hat install init version %s starting\n", VERSION);

    printf("mounting /proc filesystem... "); 
    if (!testing) {
	if (mount("/proc", "/proc", "proc", 0, NULL))
	    fatal_error(1);
    }
    printf("done\n");

    printf("mounting /dev/pts (unix98 pty) filesystem... "); 
    if (!testing) {
	if (mount("/dev/pts", "/dev/pts", "devpts", 0, NULL))
	    fatal_error(1);
    }
    printf("done\n");

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    /* these args are only for testing from commandline */
    for (i = 1; i < argc; i++)
	if (!strcmp (argv[i], "serial")) {
	    isSerial = 1;
	    break;
	}

    /* look through /proc/cmdline for special options */
    if ((fd = open("/proc/cmdline", O_RDONLY,0)) > 0) {
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len > 0 && mystrstr(buf, "nokill"))
	    noKill = 1;
    }

#if !defined(__s390__) && !defined(__s390x__)
    if (ioctl (0, TIOCLINUX, &twelve) < 0)
	isSerial = 2;
    
    if (isSerial) {
	char *device = "/dev/ttyS0";

	printf("Red Hat install init version %s using a serial console\n", 
		VERSION);

	printf("remember, cereal is an important part of a nutritionally "
	       "balanced breakfast.\n\n");

	if (isSerial == 2)
	    device = "/dev/console";
	fd = open(device, O_RDWR, 0);
	if (fd < 0)
	    device = "/dev/tts/0";

	if (fd < 0) {
	    printf("failed to open %s\n", device);
	    fatal_error(1);
	}

	setupTerminal(fd);
    } else {
	fd = open("/dev/tty1", O_RDWR, 0);
	if (fd < 0)
	    fd = open("/dev/vc/1", O_RDWR, 0);

	if (fd < 0) {
	    printf("failed to open /dev/tty1 and /dev/vc/1");
	    fatal_error(1);
	}
    }

    if (testing)
	exit(0);

    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
#endif

    setsid();
    if (ioctl(0, TIOCSCTTY, NULL)) {
	printf("could not set new controlling tty\n");
    }

    if (!testing) {
	sethostname("localhost.localdomain", 21);
	/* the default domainname (as of 2.0.35) is "(none)", which confuses 
	   glibc */
	setdomainname("", 0);
    }

    printf("checking for NFS root filesystem...");
    if (hasNetConfiged()) {
	printf("yes\n");
	roRoot = nfsRoot = 1;
    } else {
	printf("no\n");
    }

    if (!nfsRoot) {
	printf("trying to remount root filesystem read write... ");
	if (mount("/", "/", "ext2", MS_REMOUNT | MS_MGC_VAL, NULL)) {
	    printf("failed (but that's okay)\n");
	
	    roRoot = 1;
	} else {
	    printf("done\n");

	    /* 2.0.18 (at least) lets us remount a CD r/w!! */
	    printf("checking for writeable /tmp... ");
	    fd = open("/tmp/tmp", O_WRONLY | O_CREAT, 0644);
	    if (fd < 0) {
		printf("no (probably a CD rooted install)\n");
		roRoot = 1;
	    } else {
		close(fd);
		unlink("/tmp/tmp");
		printf("yes\n");
	    }
	}
    }

#if !defined(__s390__) && !defined(__s390x__)
#define RAMDISK_DEVICE "/dev/ram"
#else
#define RAMDISK_DEVICE "/dev/ram2"
#endif

    if (!testing && roRoot) {
	printf("creating 300k of ramdisk space... ");
	if (doMke2fs(RAMDISK_DEVICE, "300"))
	    fatal_error(0);

	printf("done\n");
	
	printf("mounting /tmp from ramdisk... ");
	if (mount(RAMDISK_DEVICE, "/tmp", "ext2", 0, NULL))
	    fatal_error(1);

	printf("done\n");

	if (!nfsRoot) cdRoot = 1;
    }

    /* Now we have some /tmp space set up, and /etc and /dev point to
       it. We should be in pretty good shape. */

    if (!testing) 
	doklog("/dev/tty4");

    /* Go into normal init mode - keep going, and then do a orderly shutdown
       when:

	1) /bin/install exits
	2) we receive a SIGHUP 
    */

    printf("running install...\n"); 

    setsid();

#ifndef INCLUDE_RESCUE
    if (!(installpid = fork())) {
	/* child */
#endif /* INCLUDE_RESCUE */	    
	*argvp++ = "/sbin/loader";
	*argvp++ = NULL;

	printf("running %s\n", argvc[0]);
	execve(argvc[0], argvc, env);
	printf("execve failed!\n");
#ifndef INCLUDE_RESCUE
	exit(0);
    }
#endif /* INCLUDE_RESCUE */

#ifdef INCLUDE_RESCUE    
    /* just close the file descriptors
    * don't even think about to call
    * ioctl(0, TIOCNOTTY, (char *)0)
    * you will kill the main linuxrc and provocate a kernel panic */
    close(0);
    close(1);
    close(2);
#endif /* INCLUDE_RESCUE */

    while (!doShutdown) {
	childpid = wait4(-1, &waitStatus, 0, NULL);

	if (childpid == installpid) 
	    doShutdown = 1;
    }

    if (!WIFEXITED(waitStatus) || WEXITSTATUS(waitStatus)) {
	printf("install exited abnormally ");
	if (WIFSIGNALED(waitStatus)) {
	    printf("-- received signal %d", WTERMSIG(waitStatus));
	}
	printf("\n");
    } else {
	doReboot = 1;
    }

    if (testing)
        exit(0);

    sync(); sync();

    if (!testing && !noKill) {
	printf("sending termination signals...");
	kill(-1, 15);
	sleep(2);
	printf("done\n");

	printf("sending kill signals...");
	kill(-1, 9);
	sleep(2);
	printf("done\n");
    }

    printf("disabling swap...\n");
    disableSwap();

    printf("unmounting filesystems...\n"); 
    unmountFilesystems();

    ejectCdrom();

    if (doReboot) {
	printf("rebooting system\n");
	sleep(2);

#if USE_MINILIBC
	reboot(0xfee1dead, 672274793, 0x1234567);
#else
# ifdef __alpha__
	reboot(RB_HALT_SYSTEM);
# else
	reboot(RB_AUTOBOOT);
# endif
#endif
    } else {
	printf("you may safely reboot your system\n");
	while (1);
    }

    exit(0);

    return 0;
}
