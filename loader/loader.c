/*
 * loader.c
 * 
 * This is the installer loader.  Its job is to somehow load the rest
 * of the installer into memory and run it.  This may require setting
 * up some devices and networking, etc. The main point of this code is
 * to stay SMALL! Remember that, live by that, and learn to like it.
 *
 * Erik Troan <ewt@redhat.com>
 * Matt Wilson <msw@redhat.com>
 * Michael Fulbright <msf@redhat.com>
 * Jeremy Katzj <katzj@redhat.com>
 *
 * Copyright 1997 - 2002 Red Hat, Inc.
 *
 * This software may be freely redistributed under the terms of the GNU
 * public license.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <kudzu/kudzu.h>
#include <kudzu/device.h>
#include <net/if.h>
#include <newt.h>
#include <popt.h>
#include <syslog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <sys/vt.h>
#include <linux/fb.h>
#include <linux/cdrom.h>

#include <popt.h>
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

#include "balkan/balkan.h"
#include "isys/imount.h"
#include "isys/isys.h"
#include "isys/probe.h"
#include "stubs.h"

#include "cdrom.h"
#include "devices.h"
#include "kickstart.h"
#include "lang.h"
#include "loader.h"
#include "log.h"
#include "mediacheck.h"
#include "misc.h"
#include "modules.h"
#include "net.h"
#include "pcmcia.h"
#include "urls.h"
#include "windows.h"

int probe_main(int argc, char ** argv);
int combined_insmod_main(int argc, char ** argv);
int cardmgr_main(int argc, char ** argv);
int ourInsmodCommand(int argc, char ** argv);
int kon_main(int argc, char ** argv);
static int mountLoopback(char * fsystem, char * mntpoint, char * device);
static int umountLoopback(char * mntpoint, char * device);
int copyDirectory(char * from, char * to);
static char * mediaCheckISODir(char *path);
static void useMntSourceUpdates(char * path);
static int getISOStatusFromFD(int isof, char *mediasum);
static int getISOStatusFromFile(char *path, char *mediasum);
static int getISOStatusFromCDROM(char *cddriver, char *mediasum);
static void writeISOStatus(int status, char *mediasum);

#if defined(__ia64__)
static char * floppyDevice = "hda";
#else
static char * floppyDevice = "fd0";
#endif

struct knownDevices devices;

struct installMethod {
    char * name;
    int network;
    enum deviceClass deviceType;			/* for pcmcia */
    char * (*mountImage)(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags);
};

#ifdef INCLUDE_LOCAL
static char * mountCdromImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags);
static char * mountHardDrive(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags);
#endif
#ifdef INCLUDE_NETWORK
static char * mountNfsImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags);
static char * mountUrlImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags);
#endif

static struct installMethod installMethods[] = {
#if defined(INCLUDE_LOCAL)
    { N_("Local CDROM"), 0, CLASS_CDROM, mountCdromImage },
#endif
#if defined(INCLUDE_NETWORK)
    { N_("NFS image"), 1, CLASS_NETWORK, mountNfsImage },
    { "FTP", 1, CLASS_NETWORK, mountUrlImage },
    { "HTTP", 1, CLASS_NETWORK, mountUrlImage },
#endif
#if !defined(__ia64__)
#if defined(INCLUDE_LOCAL)
    { N_("Hard drive"), 0, CLASS_HD, mountHardDrive },
#endif
#endif
};
static int numMethods = sizeof(installMethods) / sizeof(struct installMethod);

static int memoryOverhead = 0;
static int newtRunning = 0;
int continuing = 0;
#ifdef INCLUDE_KON
int haveKon = 1;
#else
int haveKon = 0;
#endif
static int defaultLang = 0;

#define MAX_EXTRA_ARGS 128

void doSuspend(void) {
    newtFinished();
    exit(1);
}

static int setupRamdisk(void) {
    gzFile f;
    static int done = 0;

    if (done) return 0;

    done = 1;

    f = gunzip_open("/etc/ramfs.img");
    if (f) {
	char buf[10240];
	int i, j = 0;
	int fd;

	fd = open(RAMDISK_DEVICE, O_RDWR);
	logMessage("copying file to fd %d", fd);

	while ((i = gunzip_read(f, buf, sizeof(buf))) > 0) {
	    j += write(fd, buf, i);
	}

	logMessage("wrote %d bytes", j);
	close(fd);
	gunzip_close(f);
    }

    if (doPwMount(RAMDISK_DEVICE, "/tmp/ramfs", "ext2", 0, 0, NULL, NULL))
	logMessage("failed to mount ramfs image");

    return 0;
}

void startNewt(int flags) {
    if (!newtRunning) {
	char *buf = sdupprintf(_("Welcome to %s"), PRODUCTNAME);
	newtInit();
	newtCls();
	newtDrawRootText(0, 0, buf);
	free(buf);

	newtPushHelpLine(_("  <Tab>/<Alt-Tab> between elements  | <Space> selects | <F12> next screen "));

	newtRunning = 1;
        if (FL_TESTING(flags)) 
	    newtSetSuspendCallback((void *) doSuspend, NULL);
    }
}

void stopNewt(void) {
    if (newtRunning) newtFinished();
    newtRunning = 0;
}

static void spawnShell(int flags) {
    pid_t pid;
    int fd;

    if (FL_SERIAL(flags) || FL_NOSHELL(flags)) {
	logMessage("not spawning a shell");
	return;
    }

    fd = open("/dev/tty2", O_RDWR);
    if (fd < 0) {
	logMessage("cannot open /dev/tty2 -- no shell will be provided");
	return;
    } else if (access("/bin/sh",  X_OK))  {
	logMessage("cannot open shell - /bin/sh doesn't exist");
	return;
    }

    if (!(pid = fork())) {
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	close(fd);
	setsid();
	if (ioctl(0, TIOCSCTTY, NULL)) {
	    logMessage("could not set new controlling tty");
	}

	signal(SIGINT, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);

	setenv("LD_LIBRARY_PATH",
		"/lib:/usr/lib:/usr/X11R6/lib:/mnt/usr/lib:"
		"/mnt/sysimage/lib:/mnt/sysimage/usr/lib", 1);

	execl("/bin/sh", "-/bin/sh", NULL);
	logMessage("exec of /bin/sh failed: %s", strerror(errno));
    }

    close(fd);

    return;
}

static int detectHardware(moduleInfoSet modInfo, 
			  char *** modules, int flags) {
    struct device ** devices, ** device;
    char ** modList;
    int numMods;
    char *driver;

    logMessage("probing buses");

    devices = probeDevices(CLASS_UNSPEC,
			   BUS_PCI | BUS_SBUS,
			   PROBE_ALL);

    logMessage("finished bus probing");

    if (devices == NULL) {
        *modules = NULL;
	return LOADER_OK;
    }

    numMods = 0;
    for (device = devices; *device; device++) numMods++;

    if (!numMods) {
	*modules = NULL;
	return LOADER_OK;
    }

    modList = malloc(sizeof(*modList) * (numMods + 1));
    numMods = 0;

    for (device = devices; *device; device++) {
	driver = (*device)->driver;
	if (strcmp (driver, "ignore") && strcmp (driver, "unknown")
	    && strcmp (driver, "disabled")) {
	    modList[numMods++] = strdup(driver);
	}

	freeDevice (*device);
    }

    modList[numMods] = NULL;
    *modules = modList;

    free(devices);

    return LOADER_OK;
}

int addDeviceManually(moduleInfoSet modInfo, moduleList modLoaded, 
		      moduleDeps * modDepsPtr, struct knownDevices * kd, 
		      int flags) {
    char * pristineItems[] = { N_("SCSI"), N_("Network") };
    char * items[3];
    int i, rc;
    int choice = 0;
    enum deviceClass type;

    for (i = 0; i < sizeof(pristineItems) / sizeof(*pristineItems); i++) {
	items[i] = _(pristineItems[i]);
    }

    items[i] = NULL;

    do {
	rc = newtWinMenu(_("Devices"), 
		       _("What kind of device would you like to add"), 40,
		       0, 20, 2, items, &choice, _("OK"), _("Back"), NULL);
	if (rc == 2) return LOADER_BACK;

	if (choice == 1)
	    type = DRIVER_NET;
	else
	    type = DRIVER_SCSI;

	rc = devDeviceMenu(type, modInfo, modLoaded, modDepsPtr, 
			   floppyDevice, flags, NULL);
    } while (rc);

    return 0;
}

int manualDeviceCheck(moduleInfoSet modInfo, moduleList modLoaded, 
		      moduleDeps * modDepsPtr, struct knownDevices * kd, 
		      int flags) {
    int i, rc;
    char buf[2000];
    struct moduleInfo * mi;
    newtComponent done, add, text, items, form, answer;
    newtGrid grid, buttons;
    int numItems;
    int maxWidth;

    while (1) {
	numItems = 0;
        maxWidth = 0;
	for (i = 0, *buf = '\0'; i < modLoaded->numModules; i++) {
	    if (!modLoaded->mods[i].weLoaded) continue;

	    if (!(mi = isysFindModuleInfo(modInfo, modLoaded->mods[i].name))) {
		continue;
	    }

	    strcat(buf, "    ");
	    strcat(buf, mi->description);

	    if (maxWidth < strlen(mi->description)) 
		maxWidth = strlen(mi->description);

	    strcat(buf, "\n");
	    numItems++;
	}

        if (numItems > 0) {
	    text = newtTextboxReflowed(-1, -1, 
		_("The following devices have been found on your system:"), 
		40, 5, 20, 0);
	    buttons = newtButtonBar(_("Done"), &done, _("Add Device"), &add, 
				    NULL);
	    items = newtTextbox(-1, -1, maxWidth + 8, 
				numItems < 10 ? numItems : 10, 
				(numItems < 10 ? 0 : NEWT_FLAG_SCROLL));
				    
	    newtTextboxSetText(items, buf);

	    grid = newtGridSimpleWindow(text, items, buttons);
	    newtGridWrappedWindow(grid, _("Devices"));

	    form = newtForm(NULL, NULL, 0);
	    newtGridAddComponentsToForm(grid, form, 1);

	    answer = newtRunForm(form);
	    newtPopWindow();

	    newtGridFree(grid, 1);
	    newtFormDestroy(form);

	    if (answer != add)
		break;

	    addDeviceManually(modInfo, modLoaded, modDepsPtr, kd, flags);
	} else {
	    rc = newtWinChoice(_("Devices"), _("Done"), _("Add Device"), 
		    _("No special device drivers have been loaded for "
		      "your system. Would you like to load any now?"));
	    if (rc != 2)
		break;

	    addDeviceManually(modInfo, modLoaded, modDepsPtr, kd, flags);
	}
    } 


    return 0;
}

int busProbe(moduleInfoSet modInfo, moduleList modLoaded, moduleDeps modDeps,
	     int justProbe, struct knownDevices * kd, int flags) {
    int i;
    char ** modList;
    char modules[1024];

    if (FL_NOPROBE(flags)) return 0;

    if (!access("/proc/bus/pci/devices", R_OK) ||
        !access("/proc/openprom", R_OK)) {
        /* autodetect whatever we can */
        if (detectHardware(modInfo, &modList, flags)) {
	    logMessage("failed to scan pci bus!");
	    return 0;
	} else if (modList && justProbe) {
	    for (i = 0; modList[i]; i++)
		printf("%s\n", modList[i]);
	} else if (modList) {
	    *modules = '\0';

	    for (i = 0; modList[i]; i++) {
		if (i) strcat(modules, ":");
		strcat(modules, modList[i]);
	    }

	    mlLoadModuleSet(modules, modLoaded, modDeps, modInfo, flags);

	    kdFindScsiList(kd, 0);
	    kdFindNetList(kd, 0);
	} else 
	    logMessage("found nothing");
    }

    return 0;
}

static int setupStage2Image(int fd, char * dest, int flags,
			     char * device, char * mntpoint) {
    int rc;
    struct stat sb;

    rc = copyFileFd(fd, dest);
    stat(dest, &sb);
    logMessage("copied %ld bytes to %s", (long)sb.st_size, dest);

    if (rc) {
	/* just to make sure */
	unlink(dest);
	return 1;
    }

    if (mountLoopback(dest, mntpoint, device)) {
	newtWinMessage(_("Error"), _("OK"),
		"Error mounting /dev/%s on %s (%s). This shouldn't "
		    "happen, and I'm rebooting your system now.", 
		device, mntpoint, strerror(errno));
	exit(1);
    }

    return 0;
}

/* returns the *absolute* path (malloced) to the #1 iso image */
char * validIsoImages(char * dirName) {
    DIR * dir;
    struct dirent * ent;
    char isoImage[1024];

    if (!(dir = opendir(dirName))) {
	newtWinMessage(_("Error"), _("OK"), 
		       _("Failed to read directory %s: %s"),
		       dirName, strerror(errno));
	return 0;
    }

    /* Walk through the directories looking for a Red Hat CD image. */
    errno = 0;
    while ((ent = readdir(dir))) {
	sprintf(isoImage, "%s/%s", dirName, ent->d_name);

	if (fileIsIso(isoImage)) {
	    errno = 0;
	    continue;
	}

	if (mountLoopback(isoImage, "/tmp/loopimage", "loop0")) {
	    logMessage("failed to mount %s", isoImage);
	    errno = 0;
	    continue;
	}

	if (!access("/tmp/loopimage/RedHat/base/hdstg1.img", F_OK)) {
	    umountLoopback("/tmp/loopimage", "loop0");
	    break;
	}

	umountLoopback("/tmp/loopimage", "loop0");

	errno = 0;
    }

    closedir(dir);

    if (!ent) return NULL;

    return strdup(isoImage);
}

#ifdef INCLUDE_LOCAL
static int loadLocalImages(char * prefix, char * dir, int flags, 
			   char * device, char * mntpoint) {
    int fd, rc;
    char * path;

    /* In a kind world, this would do nothing more then mount a ramfs
     * or tmpfs. The world isn't kind. */

    setupRamdisk();

    path = alloca(50 + strlen(prefix) + (dir ? strlen(dir) : 2));

    sprintf(path, "%s/%s/RedHat/base/hdstg1.img", prefix, dir ? dir : "");

    if ((fd = open(path, O_RDONLY)) < 0) {
	logMessage("failed to open %s: %s", path, strerror(errno));
	return 1;
    } 

    /* handle updates.img now before we copy stage2 over... this allows
     * us to keep our ramdisk size as small as possible */
    sprintf(path, "%s/%s/RedHat/base/updates.img", prefix, dir ? dir : "");
    useMntSourceUpdates(path);

    rc = setupStage2Image(fd, "/tmp/ramfs/hdstg1.img", flags, device, mntpoint);

    close(fd);

    return rc;
}

static char * setupIsoImages(char * device, char * type, char * dirName, 
			     int flags) {
    int rc;
    char * url;
    char filespec[1024];
    char * path;

    logMessage("mounting device %s as %s", device, type);

    if (!FL_TESTING(flags)) {
	/* +5 skips over /dev/ */
	if (devMakeInode(device, "/tmp/hddev"))
	    logMessage("devMakeInode failed!");

	if (doPwMount("/tmp/hddev", "/tmp/hdimage", type, 1, 0, NULL, NULL))
	    return NULL;

	sprintf(filespec, "/tmp/hdimage/%s", dirName);

	path = validIsoImages(filespec);

	if (path) {
	    char * updatesPath;
	    char mediasum[33];
	    int isostatus;


	    /* handle updates.img now before we copy stage2 over... this allows
	     * us to keep our ramdisk size as small as possible */
	    updatesPath = alloca(50 + strlen(filespec));
	    sprintf(updatesPath, "%s/updates.img", filespec);
	    useMntSourceUpdates(updatesPath);

	    rc = mountLoopback(path, "/tmp/loopimage", "loop0");
	    if (!rc) {
		rc = loadLocalImages("/tmp/loopimage", "/", flags, "loop1",
				     "/mnt/runtime");
		if (rc) {
		  newtWinMessage(_("Error"), _("OK"),
			_("An error occured reading the install "
			  "from the ISO images. Please check your ISO "
			  "images and try again."));
		}
	    }

	    umountLoopback("/tmp/loopimage", "loop0");

	    isostatus = getISOStatusFromFile(path, mediasum);
	    writeISOStatus(isostatus, mediasum);

	    if (!FL_KICKSTART(flags) && FL_MEDIACHECK(flags))
		mediaCheckISODir("/mnt/source");

	} else {
	    rc = 1;
	}

	umount("/tmp/hdimage");

	if (rc) return NULL;
    }
   
    url = malloc(50 + strlen(dirName ? dirName : ""));
    sprintf(url, "hd://%s:%s/%s", device, type, dirName ? dirName : ".");

    return url;
}

static char * setupOldHardDrive(char * device, char * type, char * dir, 
			     int flags) {
    int rc;
    char * url;

    logMessage("mounting device %s as %s", device, type);

    if (!FL_TESTING(flags)) {
	/* +5 skips over /dev/ */
	if (devMakeInode(device, "/tmp/hddev"))
	    logMessage("devMakeInode failed!");

	if (doPwMount("/tmp/hddev", "/tmp/hdimage", type, 1, 0, NULL, NULL))
	    return NULL;

	rc = loadLocalImages("/tmp/hdimage", dir, flags, "loop0",
			     "/mnt/runtime");
	if (rc) umount("/mnt/hdimage");

	umount("/tmp/hdimage");

	if (rc) return NULL;
    }

    url = malloc(50 + strlen(dir ? dir : ""));
    sprintf(url, "oldhd://%s:%s/%s", device, type, dir ? dir : ".");

    return url;
}

#endif

static int umountLoopback(char * mntpoint, char * device) {
    int loopfd;

    umount(mntpoint);

    logMessage("umounting loopback %s %s", mntpoint, device);

    devMakeInode(device, "/tmp/loop");
    loopfd = open("/tmp/loop", O_RDONLY);

    if (ioctl(loopfd, LOOP_CLR_FD, 0) < 0)
	logMessage("LOOP_CLR_FD failed for %s %s", mntpoint, device);

    close(loopfd);

    return 0;
}


static int mountLoopback(char * fsystem, char * mntpoint, char * device) {
    struct loop_info loopInfo;
    int targfd, loopfd;
    char *filename;

    mkdirChain(mntpoint);
    filename = alloca(15 + strlen(device));
    sprintf(filename, "/tmp/%s", device);

    mkdirChain(mntpoint);

    targfd = open(fsystem, O_RDONLY);
    if (targfd < 0)
	logMessage("opening target filesystem %s failed", fsystem);

    devMakeInode(device, filename);
    loopfd = open(filename, O_RDONLY);
    logMessage("mntloop %s on %s as %s fd is %d", 
	       device, mntpoint, fsystem, loopfd);

    if (ioctl(loopfd, LOOP_SET_FD, targfd)) {
	logMessage("LOOP_SET_FD failed: %s", strerror(errno));
	close(targfd);
	close(loopfd);
	return LOADER_ERROR;
    }

    close(targfd);

    memset(&loopInfo, 0, sizeof(loopInfo));
    strcpy(loopInfo.lo_name, fsystem);

    if (ioctl(loopfd, LOOP_SET_STATUS, &loopInfo)) {
	logMessage("LOOP_SET_STATUS failed: %s", strerror(errno));
	close(loopfd);
	return LOADER_ERROR;
    }

    close(loopfd);

    if (doPwMount(filename, mntpoint, "iso9660", 1,
		  0, NULL, NULL)) {
	if (doPwMount(filename, mntpoint, "ext2", 1,
		      0, NULL, NULL)) {
	    if (doPwMount(filename, mntpoint, "cramfs", 1,
			  0, NULL, NULL)) {
	    
		logMessage("failed to mount loop: %s", 
			   strerror(errno));
		return LOADER_ERROR;
	    }
	}
    }

    return 0;
}

static int totalMemory(void) {
    int fd;
    int bytesRead;
    char buf[4096];
    char * chptr, * start;
    int total = 0;

    fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
	logMessage("failed to open /proc/meminfo: %s", strerror(errno));
	return 0;
    }

    bytesRead = read(fd, buf, sizeof(buf) - 1);
    if (bytesRead < 0) {
	logMessage("failed to read from /proc/meminfo: %s", strerror(errno));
	close(fd);
	return 0;
    }

    close(fd);
    buf[bytesRead] = '\0';

    chptr = buf;
    while (*chptr && !total) {
	if (*chptr != '\n' || strncmp(chptr + 1, "MemTotal:", 9)) {
	    chptr++;
	    continue;
	}

	start = ++chptr ;
	while (*chptr && *chptr != '\n') chptr++;

	*chptr = '\0';

	while (!isdigit(*start) && *start) start++;
	if (!*start) {
	    logMessage("no number appears after MemTotal tag");
	    return 0;
	}

	chptr = start;
	while (*chptr && isdigit(*chptr)) {
	    total = (total * 10) + (*chptr - '0');
	    chptr++;
	}
    }

    logMessage("%d kB are available", total);

    return total;
}

/* try to use the provided updates.img at path
 */
static void useMntSourceUpdates(char * path) {
  if (!access(path, R_OK)) {
    if (!mountLoopback(path,
		       "/tmp/update-disk", "loop7")) {
	copyDirectory("/tmp/update-disk", "/tmp/updates");
	umountLoopback("/tmp/update-disk", "loop7");
    }
  }
}

/* get description of ISO image from stamp file */
char *getReleaseDescriptorFromIso(char *file) {
    DIR * dir;
    FILE *f;
    struct dirent * ent;
    struct stat sb;
    char *stampfile;
    char *descr;
    char tmpstr[1024];
    int  filetype;

    lstat(file, &sb);
    if (S_ISBLK(sb.st_mode)) {
	filetype = 1;
	if (doPwMount(file, "/tmp/testmnt",
		      "iso9660", 1, 0, NULL, NULL)) {
	    logMessage("Failed to mount device %s to get description", file);
	    return NULL;
	}
    } else if (S_ISREG(sb.st_mode)) {
	filetype = 2;
	if (mountLoopback(file, "/tmp/testmnt", "loop6")) {
	    logMessage("Failed to mount iso %s to get description", file);
	    return NULL;
	}
    } else {
	    logMessage("Unknown type of file %s to get description", file);
	    return NULL;
    }

    if (!(dir = opendir("/tmp/testmnt"))) {
	umount("/tmp/testmnt");
	if (filetype == 2)
	    umountLoopback("tmp/testmnt", "loop6");
	return NULL;
    }

    errno = 0;
    stampfile = NULL;
    while ((ent = readdir(dir))) {
	if (!strncmp(ent->d_name, ".discinfo", 9)) {
	    stampfile = strdup(".discinfo");
	    break;
	}
    }

    closedir(dir);
    descr = NULL;
    if (stampfile) {
	snprintf(tmpstr, sizeof(tmpstr), "/tmp/testmnt/%s", stampfile);
	f = fopen(tmpstr, "r");
	if (f) {
	    char *tmpptr;

	    /* skip over time stamp line */
	    tmpptr = fgets(tmpstr, sizeof(tmpstr), f);
	    /* now read OS description line */
	    if (tmpptr)
		tmpptr = fgets(tmpstr, sizeof(tmpstr), f);

	    if (tmpptr)
		descr = strdup(tmpstr);

	    /* skip over arch */
	    if (tmpptr)
		tmpptr = fgets(tmpstr, sizeof(tmpstr), f);

	    /* now get the CD number */
	    if (tmpptr) {
		unsigned int len;
		char *p, *newstr;

		tmpptr = fgets(tmpstr, sizeof(tmpstr), f);
		
		/* nuke newline from end of descr, stick number on end*/
		for (p=descr+strlen(descr); p != descr && !isspace(*p); p--);

		*p = '\0';
		len = strlen(descr) + strlen(tmpstr) + 10;
		newstr = malloc(len);
		strncpy(newstr, descr, len-1);
		strncat(newstr, " ", len-1);

		/* is this a DVD or not?  If disc id has commas, like */
		/* "1,2,3", its a DVD                                 */
		if (strchr(tmpstr, ','))
		    strncat(newstr, "DVD\n", len-1);
		else {
		    strncat(newstr, "disc ", len-1);
		    strncat(newstr, tmpstr, len-1);
		}

		free(descr);
		descr = newstr;
	    }

	    fclose(f);
	}
    }

    free(stampfile);

    umount("/tmp/testmnt");
    if (filetype == 2)
	umountLoopback("tmp/testmnt", "loop6");

    if (descr) {
	char *dupdescr;
	
	dupdescr = strdup(descr);
	dupdescr[strlen(dupdescr)-1] = '\0';
        return dupdescr;
    } else {
	return descr;
    }
}



/* XXX this ignores "location", which should be fixed */
static char * mediaCheckISODir(char *path) {
    DIR * dir;
    struct dirent * ent;
    char isoImage[1024];
    char tmpmessage[1024];
    int rc;


    if (!(dir = opendir(path))) {
	newtWinMessage(_("Error"), _("OK"), 
		       _("Failed to read directory %s: %s"),
		       path, strerror(errno));
	return 0;
    }

    /* Walk through the directories looking for a Red Hat CD images. */
    errno = 0;
    while ((ent = readdir(dir))) {
	sprintf(isoImage, "%s/%s", path, ent->d_name);

	if (fileIsIso(isoImage)) {
	    errno = 0;
	    continue;
	}


	snprintf(tmpmessage, sizeof(tmpmessage),
		 _("Would you like to perform a checksum "
		   "test of the ISO image:\n\n   %s?"), isoImage);

	rc = newtWinChoice(_("Checksum Test"), _("Test"), _("Skip"),
			   tmpmessage);

	if (rc == 2) {
	    logMessage("mediacheck: skipped checking of %s", isoImage);
	    continue;
	    /*
	    closedir(dir);
	    return NULL;
	    */
	} else {
	    char *descr;

	    descr = getReleaseDescriptorFromIso(isoImage);
	    mediaCheckFile(isoImage, descr);
	    if (descr)
		free(descr);

	    continue;
	}
    }

    closedir(dir);
    return NULL;
}

static int getISOStatusFromCDROM(char *cddriver, char *mediasum) {
    int isofd;
    int isostatus;

    devMakeInode(cddriver, "/tmp/cdrom");
    isofd = open("/tmp/cdrom", O_RDONLY);
    if (isofd < 0) {
	logMessage("Could not check iso status: %s", strerror(errno));
	unlink("/tmp/cdrom");
	return 0;
    }

    isostatus = getISOStatusFromFD(isofd, mediasum);

    close(isofd);
    unlink("/tmp/cdrom");

    return isostatus;
}

static int getISOStatusFromFile(char *path, char *mediasum) {
    int isofd;
    int isostatus;

    isofd = open(path, O_RDONLY);
    if (isofd < 0) {
	logMessage("Could not check iso status: %s", strerror(errno));
	return 0;
    }

    isostatus = getISOStatusFromFD(isofd, mediasum);

    close(isofd);

    return isostatus;
}

/* get support status */
/* if returns 1 we found status, and mediasum will be checksum */
static int getISOStatusFromFD(int isofd, char *mediasum) {
    unsigned char tmpsum[33];
    int skipsectors, isostatus;
    long long isosize, pvd_offset;

    if (mediasum)
	mediasum[0] = '\0';

    if ((pvd_offset = parsepvd(isofd, tmpsum, &skipsectors, &isosize, &isostatus)) < 0) {
	logMessage("Could not parse pvd");
	return 0;
    }

    if (mediasum)
	strcpy(mediasum, tmpsum);

    return isostatus;
}

static void writeISOStatus(int status, char *mediasum) {
    FILE *f;

    if (!(f = fopen("/tmp/isoinfo", "w")))
	return;

    fprintf(f, "ISOSTATUS=%d\n", status);
    fprintf(f, "MEDIASUM=%s\n", mediasum);

    fclose(f);

}

#ifdef INCLUDE_LOCAL

static char * mountHardDrive(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags) {
    int rc;
    int fd;
    int i, j;
#if defined (__s390__) || defined (__s390x__)
    static struct networkDeviceConfig netDev;
    struct iurlinfo ui;
    char * devName;
#endif
    struct {
	char name[20];
	int type;
    } partitions[1024], * part;
    struct partitionTable table;
    newtComponent listbox, label, dirEntry, form, okay, back, text;
    struct newtExitStruct es;
    newtGrid entryGrid, grid, buttons;
    int done = 0;
    char * dir = strdup("");
    char * tmpDir;
    char * type;
    char * url = NULL;
    char * buf;
    int numPartitions;
    #ifdef __sparc__
    static int ufsloaded;
    #endif
    #if defined (__s390__) || defined (__s390x__)
    char c;
    #endif

    while (!done) {
	numPartitions = 0;
	for (i = 0; i < kd->numKnown; i++) {
	    if (kd->known[i].class == CLASS_HD) {
		devMakeInode(kd->known[i].name, "/tmp/hddevice");
		if ((fd = open("/tmp/hddevice", O_RDONLY)) >= 0) {
		    if ((rc = balkanReadTable(fd, &table))) {
			logMessage("failed to read partition table for "
				   "device %s: %d", kd->known[i].name, rc);
		    } else {
			for (j = 0; j < table.maxNumPartitions; j++) {
			    switch (table.parts[j].type) {
#ifdef __sparc__
			      case BALKAN_PART_UFS:
				if (!ufsloaded) {
				    ufsloaded = 1;
				    mlLoadModuleSet("ufs", modLoaded, 
						 *modDepsPtr, modInfo, 
						 flags);
				}
				/* FALLTHROUGH */
#endif
			      case BALKAN_PART_DOS:
			      case BALKAN_PART_EXT2:
				  if (!strncmp (kd->known[i].name, "cciss/", 6) ||
				      !strncmp (kd->known[i].name, "ida/", 4) ||
				      !strncmp (kd->known[i].name, "rd/", 3))
				      
				      sprintf(partitions[numPartitions].name, 
					      "/dev/%sp%d", kd->known[i].name, j + 1);
				  else
				      sprintf(partitions[numPartitions].name, 
					      "/dev/%s%d", kd->known[i].name, j + 1);
				  
				partitions[numPartitions].type = 
					table.parts[j].type;
				numPartitions++;
			    }
			}
		    }

		    close(fd);
		} else {
		    /* XXX ignore errors on removable drives? */
		}

		unlink("/tmp/hddevice");
	    }
	}
	
	if (!numPartitions) {
	    rc = newtWinChoice(_("Hard Drives"), _("Yes"), _("Back"),
			    _("You don't seem to have any hard drives on "
			      "your system! Would you like to configure "
			      "additional devices?"));
	    if (rc == 2) return NULL;

	    devDeviceMenu(DRIVER_SCSI, modInfo, modLoaded, modDepsPtr, 
			  floppyDevice, flags, 
			  NULL);
	    kdFindScsiList(kd, 0);

	    continue;
	}

#if !defined (__s390__) && !defined (__s390x__)
	/* s390 */
	memset(&ui, 0, sizeof(ui));
        memset(&netDev, 0, sizeof(netDev));
        netDev.isDynamic = 1;
	i = ensureNetDevice(kd, modInfo, modLoaded, modDepsPtr, flags, &devName);
        if (i) return NULL;
	rc = readNetConfig(devName, &netDev, flags);
	if (rc) {
                if (!FL_TESTING(flags)) pumpDisableInterface(devName);
                return NULL;
	}
	setupRemote(&ui);
	mlLoadModule("isofs", modLoaded, *modDepsPtr,
                 NULL, modInfo, flags);

#endif

	buf = sdupprintf(_("What partition and directory on that "
			   "partition hold the CD (iso9660) images "
			   "for %s? If you don't see the disk drive "
			   "you're using listed here, press F2 "
			   "to configure additional devices."), PRODUCTNAME);
	text = newtTextboxReflowed(-1, -1, buf, 62, 5, 5, 0);
	free(buf);
	
	listbox = newtListbox(-1, -1, numPartitions > 5 ? 5 : numPartitions,
			      NEWT_FLAG_RETURNEXIT | 
			      (numPartitions > 5 ? NEWT_FLAG_SCROLL : 0));
	
	for (i = 0; i < numPartitions; i++) 
	    newtListboxAppendEntry(listbox, partitions[i].name, 
				   partitions + i);
	
	label = newtLabel(-1, -1, _("Directory holding images:"));

	dirEntry = newtEntry(28, 11, dir, 28, &tmpDir, NEWT_ENTRY_SCROLL);
	
	entryGrid = newtGridHStacked(NEWT_GRID_COMPONENT, label,
				     NEWT_GRID_COMPONENT, dirEntry,
				     NEWT_GRID_EMPTY);

	buttons = newtButtonBar(_("OK"), &okay, _("Back"), &back, NULL);
	
	grid = newtCreateGrid(1, 4);
	newtGridSetField(grid, 0, 0, NEWT_GRID_COMPONENT, text,
			 0, 0, 0, 1, 0, 0);
	newtGridSetField(grid, 0, 1, NEWT_GRID_COMPONENT, listbox,
			 0, 0, 0, 1, 0, 0);
	newtGridSetField(grid, 0, 2, NEWT_GRID_SUBGRID, entryGrid,
			 0, 0, 0, 1, 0, 0);
	newtGridSetField(grid, 0, 3, NEWT_GRID_SUBGRID, buttons,
			 0, 0, 0, 0, 0, NEWT_GRID_FLAG_GROWX);
	
	newtGridWrappedWindow(grid, _("Select Partition"));
	
	form = newtForm(NULL, NULL, 0);
	newtFormAddHotKey(form, NEWT_KEY_F2);
	newtFormAddHotKey(form, NEWT_KEY_F12);

	newtGridAddComponentsToForm(grid, form, 1);
	newtGridFree(grid, 1);

	newtFormRun(form, &es);

	part = newtListboxGetCurrent(listbox);
	
	free(dir);
	if (tmpDir && *tmpDir) {
	    /* Protect from form free. */
	    dir = strdup(tmpDir);
	} else  {
	    dir = strdup("");
	}
	
	newtFormDestroy(form);
	newtPopWindow();

	if (es.reason == NEWT_EXIT_COMPONENT && es.u.co == back) {
	    return NULL;
	} else if (es.reason == NEWT_EXIT_HOTKEY && es.u.key == NEWT_KEY_F2) {
	    devDeviceMenu(DRIVER_SCSI, modInfo, modLoaded, modDepsPtr, 
			  floppyDevice, flags, 
			  NULL);
	    kdFindScsiList(kd, 0);
	    continue;
	}

	logMessage("partition %s selected", part->name);
	
	switch (part->type) {
	#ifdef __sparc__
	  case BALKAN_PART_UFS:     type = "ufs"; 		break;
	#endif
	  case BALKAN_PART_EXT2:    type = "ext2"; 		break;
	  case BALKAN_PART_DOS:	    type = "vfat"; 		break;
	  default:	continue;
	}

	url = setupIsoImages(part->name + 5, type, dir, flags);
	if (!url) {
	    newtWinMessage(_("Error"), _("OK"), 
			_("Device %s does not appear to contain "
			  "Red Hat CDROM images."), part->name);
	    continue;
	}

	done = 1; 

	umount("/tmp/hdimage");
	rmdir("/tmp/hdimage");
    }

    free(dir);
#if defined (__s390__) || defined (__s390x__)
    writeNetInfo("/tmp/netinfo", &netDev, kd);
#endif

    return url;
}


void ejectCdrom(void) {
  int ejectfd;

  logMessage("ejecting /tmp/cdrom...");
  if ((ejectfd = open("/tmp/cdrom", O_RDONLY | O_NONBLOCK, 0)) >= 0) {
      if (ioctl(ejectfd, CDROMEJECT, 0))
        logMessage("eject failed %d ", errno);
      close(ejectfd);
  } else {
      logMessage("eject failed %d ", errno);
  }
}

/* XXX this ignores "location", which should be fixed */
static char * mediaCheckCdrom(char *cddriver) {
    int rc;
    int first;

    devMakeInode(cddriver, "/tmp/cdrom");

    first = 1;
    do {
	char *descr=NULL;
	/* if first time through, see if they want to eject the CD      */
	/* currently in the drive (most likely the CD they booted from) */
	/* and test a different disk.  Otherwise just test the disk in  */
	/* the drive since it was inserted in the previous pass through */
	/* this loop, so they want it tested.                           */
	if (first) {
	    first = 0;
	    rc = newtWinChoice(_("Media Check"), _("Test"), _("Eject CD"),
			       _("Choose \"%s\" to test the CD currently in "
				 "the drive, or \"%s\" to eject the CD and "
				 "insert another for testing."), _("Test"),
			       _("Eject CD"));

	    if (rc == 1) {
		descr = getReleaseDescriptorFromIso("/tmp/cdrom");
		mediaCheckFile("/tmp/cdrom", descr);
	    }

	} else {
	    descr = getReleaseDescriptorFromIso("/tmp/cdrom");
	    mediaCheckFile("/tmp/cdrom", descr);
	}

	if (descr)
	    free(descr);

	ejectCdrom();
	
	rc = newtWinChoice(_("Media Check"), _("Test"), _("Continue"),
			   _("If you would like to test additional media, "
			     "insert the next CD and press \"%s\". "
			     "You do not have to test all CDs, although "
			     "it is recommended you do so at least once.\n\n"
			     "To begin the installation process "
			     "insert CD #1 into the drive "
			     "and press \"%s\"."),
			   _("Test"), _("Continue"));

	if (rc == 2) {
	    unlink("/tmp/cdrom");
	    return NULL;
	} else {
	    continue;
	}
    } while (1);
    
    return NULL;
}

static void wrongCDMessage(void) {
    char *buf = sdupprintf(_("The %s CD was not found "
			     "in any of your CDROM drives. Please insert "
			     "the %s CD and press %s to retry."), PRODUCTNAME,
			   PRODUCTNAME, _("OK"));
    newtWinMessage(_("Error"), _("OK"), buf, _("OK"));
    free(buf);
}

/* put mounts back and continue */
static void mountCdromStage2(char *cddev) {
    int gotcd1=0;

    devMakeInode(cddev, "/tmp/cdrom");
    do {
	do {
	    if (doPwMount("/tmp/cdrom", "/mnt/source", 
			  "iso9660", 1, 0, NULL, NULL)) {
		ejectCdrom();
		wrongCDMessage();
	    } else {
		break;
	    }
	} while (1);
	
	if (mountLoopback("/mnt/source/RedHat/base/stage2.img",
			  "/mnt/runtime", "loop0")) {
	    umount("/mnt/source");
	    ejectCdrom();
	    wrongCDMessage();
	} else {
	    gotcd1 = 1;
	}
    } while (!gotcd1);
}

/* ask about doing media check */
static void queryMediaCheck(char *name, int flags) {
  int rc;
  char mediasum[33];
  int isostatus;

  /* dont bother to test in automated installs */
  if (FL_KICKSTART(flags))
      return;

  /* see what status is */
  isostatus = getISOStatusFromCDROM(name, mediasum);
  writeISOStatus(isostatus, mediasum);

  /* see if we should check image(s) */
  if (!isostatus || FL_MEDIACHECK(flags)) {
    
    startNewt(flags);
    rc = newtWinChoice(_("CD Found"), _("OK"),
		       _("Skip"), 
       _("To begin testing the CD media before installation press %s.\n\n"
	 "Choose %s to skip the media test and start the installation."), _("OK"), _("Skip"));

    if (rc != 2) {
      
      /* unmount CD now we've identified */
      /* a valid disc #1 is present */
      umount("/mnt/runtime");
      umountLoopback("/mnt/runtime", "loop0");
      umount("/mnt/source");
      
      /* test CD(s) */
      mediaCheckCdrom(name);
      
      /* remount stage2 from CD #1 and proceed */
      mountCdromStage2(name);
    }
  }
}

/* XXX this ignores "location", which should be fixed */
static char * setupCdrom(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags, int probeQuickly,
		      int needRedHatCD) {
    int i;
    int rc;
    int hasCdrom = 0;
    char * buf;

    do {
	for (i = 0; i < kd->numKnown; i++) {
	    if (kd->known[i].class != CLASS_CDROM) continue;

	    hasCdrom = 1;

	    logMessage("trying to mount device %s", kd->known[i].name);
	    devMakeInode(kd->known[i].name, "/tmp/cdrom");
	    if (!doPwMount("/tmp/cdrom", "/mnt/source", "iso9660", 1, 0, NULL, 
			  NULL)) {
		/* if probe quickly, then we're looking for a kickstart config
		 * and should just return if we can mount it */
		if (probeQuickly && !needRedHatCD) {
		    buf = malloc(200);
		    sprintf(buf, "cdrom://%s/mnt/source", kd->known[i].name);
		    return buf;
		}

		if (!needRedHatCD || 
		    !access("/mnt/source/RedHat/base/stage2.img", R_OK)) {
		    if (!mountLoopback("/mnt/source/RedHat/base/stage2.img",
				       "/mnt/runtime", "loop0")) {
		        useMntSourceUpdates("/mnt/source/RedHat/base/updates.img");
		      
			buf = malloc(200);
			sprintf(buf, "cdrom://%s/mnt/source", kd->known[i].name);
			queryMediaCheck(kd->known[i].name, flags);
			return buf;
		    }
		}
		umount("/mnt/source");
	    }
	    unlink("/tmp/cdrom");
	}

	if (probeQuickly) return NULL;

	if (hasCdrom) {
	    char *buf = sdupprintf(_("The %s CD was not found in any of your "
				     "CDROM drives. Please insert the %s CD "
				     "and press %s to retry."), PRODUCTNAME,
				   PRODUCTNAME, _("OK"));
	    rc = newtWinChoice(_("Error"), _("OK"), _("Back"), buf, _("OK"));
	    free(buf);
	    if (rc == 2) return NULL;
	} else {
	    rc = setupCDdevice(kd, modInfo, modLoaded, modDepsPtr, 
			       floppyDevice, flags);
	    if (rc == LOADER_BACK) return NULL;
	}
    } while (1);

    abort();

    return NULL;
}

static char * mountCdromImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags) {

    /* first do media check if necessary */
    
    return setupCdrom(method, location, kd, modInfo, modLoaded, modDepsPtr,
		      flags, 0, 1);
}

int kickstartFromCdrom(char * ksFile, char * fromFile, 
		       struct knownDevices * kd, 
    		       moduleInfoSet modInfo, moduleList modLoaded,
		       moduleDeps * modDepsPtr, int flags) {
    char * fullFn;

    if (!setupCdrom(NULL, NULL, kd, modInfo, modLoaded, modDepsPtr, 
		    flags, 1, 0)) {
	logMessage("kickstart failed to find CD device");
	return 1;
    }

    fullFn = alloca(strlen(fromFile) + 20);
    sprintf(fullFn, "/mnt/source/%s", fromFile);
    copyFile(fullFn, ksFile);
    umount("/mnt/source");
    unlink("/tmp/cdrom");

    return 0;
}

#endif

#ifdef INCLUDE_NETWORK

static int ensureNetDevice(struct knownDevices * kd,
    		         moduleInfoSet modInfo, moduleList modLoaded,
		         moduleDeps * modDepsPtr, int flags, 
			 char ** devNamePtr) {
    int i, rc;
    char ** devices;
    int deviceNums = 0;
    int deviceNum;

    for (i = 0; i < kd->numKnown; i++) 
	if (kd->known[i].class == CLASS_NETWORK) 
	    break;

    /* Give them a chance to insert a module. */
    if (i == kd->numKnown) {
	rc = devDeviceMenu(DRIVER_NET, modInfo, modLoaded, modDepsPtr, 
			   floppyDevice, flags, NULL);
	if (rc) return rc;
	kdFindNetList(kd, 0);
    }

    devices = alloca((kd->numKnown + 1) * sizeof(*devices));
    for (i = 0; i < kd->numKnown; i++) {
	if (kd->known[i].class == CLASS_NETWORK) {
	    devices[deviceNums++] = kd->known[i].name;
	}
    }
    devices[deviceNums] = NULL;

    /* This shouldn't happen. devDeviceMenu() should get us a network device,
       or return LOADER_BACK, in which case we don't get here. */
    if (!deviceNums) return LOADER_ERROR;

    if (deviceNums == 1 || FL_KICKSTART(flags) || FL_KSNFS(flags)) {
	*devNamePtr = devices[0];
	return 0;
    }

    startNewt(flags);

    deviceNum = 0;
    rc = newtWinMenu(_("Networking Device"), 
		     _("You have multiple network devices on this system. "
		       "Which would you like to install through?"), 40, 10, 10, 
		     deviceNums < 6 ? deviceNums : 6, devices,
		     &deviceNum, _("OK"), _("Back"), NULL);
    if (rc == 2)
	return LOADER_BACK;

    *devNamePtr = devices[deviceNum];

    return 0;
}

#endif

#ifdef INCLUDE_NETWORK

#define NFS_STAGE_IP	1
#define NFS_STAGE_NFS	2
#define NFS_STAGE_MOUNT	3
#define NFS_STAGE_DONE	4

static char * mountNfsImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		         moduleInfoSet modInfo, moduleList modLoaded,
		         moduleDeps * modDepsPtr, int flags) {
    static struct networkDeviceConfig netDev;
    struct iurlinfo ui;
    char * devName;
    int i, rc;
    char * host = NULL;
    char * dir = NULL;
    char * fullPath;
    char * path;
    char * url = NULL;
    int stage = NFS_STAGE_IP;

    initLoopback();

    memset(&ui, 0, sizeof(ui));
    memset(&netDev, 0, sizeof(netDev));
    netDev.isDynamic = 1;
    
    i = ensureNetDevice(kd, modInfo, modLoaded, modDepsPtr, flags, &devName);
    if (i) return NULL;

    while (stage != NFS_STAGE_DONE) {
        switch (stage) {
	  case NFS_STAGE_IP:
	    logMessage("going to do getNetConfig");
	    rc = readNetConfig(devName, &netDev, flags);
	    if (rc) {
		if (!FL_TESTING(flags)) pumpDisableInterface(devName);
		return NULL;
	    }
#if defined (__s390__) || defined (__s390x__)
	    setupRemote(&ui);
	    host = ui.address;
	    dir = ui.prefix;
#endif
	    stage = NFS_STAGE_NFS;
	    break;

	  case NFS_STAGE_NFS:
	    logMessage("going to do nfsGetSetup");
	    if (nfsGetSetup(&host, &dir) == LOADER_BACK)
            {
#if defined (__s390__) || defined (__s390x__)
                return NULL;
                break;
#endif
		stage = NFS_STAGE_IP;
            } else
		stage = NFS_STAGE_MOUNT;
	    break;

	  case NFS_STAGE_MOUNT:
	    if (FL_TESTING(flags)) {
		stage = NFS_STAGE_DONE;
		break;
	    }

	    fullPath = alloca(strlen(host) + strlen(dir) + 2);
	    sprintf(fullPath, "%s:%s", host, dir);

	    logMessage("mounting nfs path %s", fullPath);

	    stage = NFS_STAGE_NFS;

	    if (!doPwMount(fullPath, "/mnt/source", "nfs", 1, 0, NULL, NULL)) {
		if (!access("/mnt/source/RedHat/base/stage2.img", R_OK)) {
		    if (!mountLoopback("/mnt/source/RedHat/base/stage2.img",
				       "/mnt/runtime", "loop0")) {
			rmdir("/mnt/source");
			symlink("/mnt/source", "/mnt/source");
		        useMntSourceUpdates("/mnt/source/RedHat/base/updates.img");
			stage = NFS_STAGE_DONE;
			url = "nfs://mnt/source/.";
		    }
		} else if ((path = validIsoImages("/mnt/source"))) {
		    useMntSourceUpdates("/mnt/source/updates.img");

		    if (mountLoopback(path, "/mnt/source2", "loop1"))
			logMessage("failed to mount iso loopback!");
		    else {
			if (mountLoopback("/mnt/source2/RedHat/base/stage2.img",
				         "/mnt/runtime", "loop0")) {
			    logMessage("failed to mount install loopback!");
			} else {
			    char mediasum[33];
			    int isostatus;

			    useMntSourceUpdates("/mnt/source/RedHat/base/updates.img");
			    stage = NFS_STAGE_DONE;
			    url = "nfsiso:/mnt/source";

			    isostatus = getISOStatusFromFile(path, mediasum);
			    writeISOStatus(isostatus, mediasum);

			    if (!FL_KICKSTART(flags) && FL_MEDIACHECK(flags))
				mediaCheckISODir("/mnt/source");

			}
		    }
		} else {
		    umount("/mnt/source");
		    newtWinMessage(_("Error"), _("OK"), 
				   _("That directory does not seem to contain "
				     "a Red Hat installation tree."));
		}
	    } else {
		newtWinMessage(_("Error"), _("OK"), 
		        _("That directory could not be mounted from the server"));
	    }

	    break;	    /* from switch */
        }
    }

    writeNetInfo("/tmp/netinfo", &netDev, kd);

    free(host);
    free(dir);

    return url;
}

#endif

#ifdef INCLUDE_NETWORK

static int loadSingleUrlImage(struct iurlinfo * ui, char * file, int flags, 
			char * dest, char * mntpoint, char * device,
			int silentErrors) {
    int fd;
    int rc;
    char * newFile = NULL;

    fd = urlinstStartTransfer(ui, file, 1);

    if (fd == -2) return 1;

    if (fd < 0) {
	/* file not found */

	newFile = alloca(strlen(device) + 20);
	sprintf(newFile, "disc1/%s", file);

	fd = urlinstStartTransfer(ui, newFile, 1);

	if (fd == -2) return 1;
	if (fd < 0) {
	  if (!silentErrors) 
	    newtWinMessage(_("Error"), _("OK"),
			    _("File %s/%s not found on server."), 
			    ui->prefix, file);
	    return 1;
	}
    }

    rc = setupStage2Image(fd, dest, flags, device, mntpoint);

    urlinstFinishTransfer(ui, fd);

    if (newFile) {
	newFile = malloc(strlen(ui->prefix ) + 20);
	sprintf(newFile, "%s/disc1", ui->prefix);
	free(ui->prefix);
	ui->prefix = newFile;
    }

    return rc;
}

static int loadUrlImages(struct iurlinfo * ui, int flags) {
    setupRamdisk();

    /* try to pull the updates.img before getting the netstg1.img so
     * we can minimize our ramdisk size */
    if (!loadSingleUrlImage(ui, "RedHat/base/updates.img", flags,
			    "/tmp/ramfs/updates-disk.img",
			    "/tmp/update-disk", "loop7", 1)) {
	/* copy the updates, then unmount the loopback and unlink the img */
	copyDirectory("/tmp/update-disk", "/tmp/updates");
	umountLoopback("/tmp/update-disk", "loop7");
	unlink("/tmp/ramfs/updates-disk.img");
    }
	
    if (loadSingleUrlImage(ui, "RedHat/base/netstg1.img", flags, 
			   "/tmp/ramfs/netstg1.img",
			   "/mnt/runtime", "loop0", 0)) {
	newtWinMessage(ui->protocol == URL_METHOD_FTP ?
			_("FTP") : _("HTTP"), _("OK"), 
	       _("Unable to retrieve the first install image"));
	return 1;
    }
    
    return 0;
}

static char * getLoginName(char * login, struct iurlinfo ui) {
    int i;

    i = 0;
    /* password w/o login isn't useful */
    if (ui.login && strlen(ui.login)) {
	i += strlen(ui.login) + 5;
	if (strlen(ui.password))
	    i += 3*strlen(ui.password) + 5;

	if (ui.login || ui.password) {
	    login = malloc(i);
	    strcpy(login, ui.login);
	    if (ui.password) {
		char * chptr;
		char code[4];

		strcat(login, ":");
		for (chptr = ui.password; *chptr; chptr++) {
		    sprintf(code, "%%%2x", *chptr);
		    strcat(login, code);
		}
		strcat(login, "@");
	    }
	}
    }

    return login;
}

#define URL_STAGE_IP			1
#define URL_STAGE_MAIN			2
#define URL_STAGE_SECOND		3
#define URL_STAGE_FETCH			4
#define URL_STAGE_DONE			20

static char * mountUrlImage(struct installMethod * method,
		      char * location, struct knownDevices * kd,
    		      moduleInfoSet modInfo, moduleList modLoaded,
		      moduleDeps * modDepsPtr, int flags) {
    int i, rc;
    int stage = URL_STAGE_IP;
    char * devName;
    struct iurlinfo ui;
    char needsSecondary = ' ';
    static struct networkDeviceConfig netDev;
    char * url;
    char * login;
    char * finalPrefix;
    int dir = 1;
    enum urlprotocol_t proto = 
	!strcmp(method->name, "FTP") ? URL_METHOD_FTP : URL_METHOD_HTTP;

    if (totalMemory() < 18000) {
	newtWinMessage(_("Error"), _("OK"), _("FTP and HTTP installs "
			"require 20MB or more of system memory."));

	return NULL;
    }

    initLoopback();
    i = ensureNetDevice(kd, modInfo, modLoaded, modDepsPtr, flags, &devName);
    if (i) return NULL;

    memset(&ui, 0, sizeof(ui));
    memset(&netDev, 0, sizeof(netDev));
    netDev.isDynamic = 1;

    while (stage != URL_STAGE_DONE) {
        switch (stage) {
	  case URL_STAGE_IP:
	    rc = readNetConfig(devName, &netDev, flags);
	    if (rc) {
		if (!FL_TESTING(flags)) pumpDisableInterface(devName);
		return NULL;
	    }
#if defined (__s390__) || defined (__s390x__)
	    if (dir == -1) {
	      return NULL;
	    }
	    setupRemote(&ui);
#endif
	    stage = URL_STAGE_MAIN;
	    dir = 1;

	  case URL_STAGE_MAIN:
	    rc = urlMainSetupPanel(&ui, proto, &needsSecondary);
	    if (rc) {
#if defined (__s390__) || defined (__s390x__)
      return NULL;
#endif
			stage = URL_STAGE_IP;
			dir = -1;
	    } else {
			stage = needsSecondary != ' ' ? URL_STAGE_SECOND : URL_STAGE_FETCH;
			dir = 1;
	    }
	    break;

	  case URL_STAGE_SECOND:
	    rc = urlSecondarySetupPanel(&ui, proto);
	    if (rc) {
	        stage = URL_STAGE_MAIN;
		dir = -1;
	    } else {
	        stage = URL_STAGE_FETCH;
	        dir = 1;
	    }
	    break;

	  case URL_STAGE_FETCH:
	    if (FL_TESTING(flags)) {
		stage = URL_STAGE_DONE;
		dir = 1;
		break;
	    }

	    if (loadUrlImages(&ui, flags)) {
		stage = URL_STAGE_MAIN;
		dir = -1;
	    } else { 
		stage = URL_STAGE_DONE;
		dir = 1;
	    }
	    break;
        }
    }

    login = "";
    login = getLoginName(login, ui);

    if (!strcmp(ui.prefix, "/"))
	finalPrefix = "/.";
    else
	finalPrefix = ui.prefix;

    url = malloc(strlen(finalPrefix) + 25 + strlen(ui.address) + strlen(login));
    sprintf(url, "%s://%s%s/%s", 
	    ui.protocol == URL_METHOD_FTP ? "ftp" : "http",
	    login, ui.address, finalPrefix);

    writeNetInfo("/tmp/netinfo", &netDev, kd);

    return url;
}

#endif
    
static char * doMountImage(char * location,
			   struct knownDevices * kd,
			   moduleInfoSet modInfo,
			   moduleList modLoaded,
			   moduleDeps * modDepsPtr,
			   char ** lang,
			   char ** keymap,
			   char ** kbdtype,
			   int flags) {
    static int defaultMethod = 0;
    int i, rc, dir = 1;
    int validMethods[10];
    int numValidMethods = 0;
    char * installNames[10];
    int methodNum = 0;
    int networkAvailable = 0;
    int localAvailable = 0;
    void * class;
    char * url = NULL;
    enum { STEP_LANG, STEP_KBD, STEP_METHOD, STEP_URL, STEP_DONE } step;

    if ((class = isysGetModuleList(modInfo, DRIVER_NET))) {
	networkAvailable = 1;
	free(class);
    }

    if ((class = isysGetModuleList(modInfo, DRIVER_SCSI))) {
	localAvailable = 1;
	free(class);
    }

#if defined(__s390__) || defined(__s390x__)
    #define STARTMETHOD 1
#else
    #define STARTMETHOD 0
#endif

#if defined(__alpha__) || defined(__ia64__) \
    || defined(__s390__ ) || defined(__s390x__)
#if defined (__s390__) || defined (__s390x__)
    i = 1;  /* No CDROM */
#else
    i = 0;
#endif
   for (; i < numMethods; i++) {

	installNames[numValidMethods] = _(installMethods[i].name);
	validMethods[numValidMethods++] = i;
    }
#else
    /* platforms with split boot/bootnet disks */
#if defined(INCLUDE_PCMCIA)
    for (i = 0; i < numMethods; i++) {
	int j;

	for (j = 0; j < kd->numKnown; j++)
	    if (installMethods[i].deviceType == kd->known[j].class) break;

	if (j < kd->numKnown) {
	    if (i == defaultMethod) methodNum = numValidMethods;

	    installNames[numValidMethods] = _(installMethods[i].name);
	    validMethods[numValidMethods++] = i;
	}
    }
#endif

    if (!numValidMethods) {
	for (i = 0; i < numMethods; i++) {
	    if ((networkAvailable && installMethods[i].network) ||
		    (localAvailable && !installMethods[i].network)) {
		if (i == defaultMethod) methodNum = numValidMethods;

		installNames[numValidMethods] = _(installMethods[i].name);
		validMethods[numValidMethods++] = i;
	    }
	}
    }
#endif

    installNames[numValidMethods] = NULL;

    if (!numValidMethods) {
	logMessage("no install methods have the required devices!\n");
	exit(1);
    }

    /* This is a check for NFS or CD-ROM rooted installs */
    if (!access("/mnt/source/RedHat/instimage/usr/bin/anaconda", X_OK))
	return "cdrom://unknown/mnt/source/.";
    
#if defined (INCLUDE_LOCAL) || defined (__sparc__) || defined (__alpha__)
# if defined (__sparc__) || defined (__alpha__)
    /* Check any attached CDROM device for a
       Red Hat CD. If there is one there, just die happy */
    if (!FL_ASKMETHOD(flags)) {
# else
    /* If no network is available, check any attached CDROM device for a
       Red Hat CD. If there is one there, just die happy */
    if (!FL_ASKMETHOD(flags)) {
# endif
       url = setupCdrom(NULL, location, kd, modInfo, modLoaded, modDepsPtr,
			flags, 1, 1);
       if (url) return url;
    }
#endif /* defined (INCLUDE_LOCAL) || defined (__sparc__) */

    startNewt(flags);

#ifdef INCLUDE_KON
    if (continuing)
	step = STEP_KBD;
    else
	step = STEP_LANG;
#else
    step = STEP_LANG;
#endif
	
    while (step != STEP_DONE) {
	switch (step) {
	case STEP_LANG:
	    defaultLang = 0;
	    step = STEP_KBD;
            dir = 1;
	    break;
	    
	case STEP_KBD:
#if !defined (__s390__) && !defined (__s390x__)
	    rc = chooseKeyboard (keymap, kbdtype, flags);

            if (rc == LOADER_NOOP) {
                if (dir == -1)
                    step = STEP_LANG;
                else
                    step = STEP_METHOD;
                break;
            }
            
	    if (rc == LOADER_BACK) {
		step = STEP_LANG;
                dir = -1;
            } else {
		step = STEP_METHOD;
                dir = 1;
            }
#else
	    step = STEP_METHOD;
            dir = 1;
#endif
	    break;
	    
	case STEP_METHOD:
	    rc = newtWinMenu(FL_RESCUE(flags) ? _("Rescue Method") :
			     _("Installation Method"), 
			     FL_RESCUE(flags) ?
			     _("What type of media contains the rescue image?")
			     :
			     _("What type of media contains the packages to be "
			       "installed?"), 
			     30, 10, 20, 6, installNames, 
			     &methodNum, _("OK"), _("Back"), NULL);
	    if (rc && rc != 1) {
		step = STEP_KBD;
                dir = -1;
            } else {
		step = STEP_URL;
                dir = 1;
            }
	    break;
	case STEP_URL:
logMessage("starting to STEP_URL");
	    url = installMethods[validMethods[methodNum]].mountImage(
		   installMethods + validMethods[methodNum], location,
    		   kd, modInfo, modLoaded, modDepsPtr, flags);
	    logMessage("got url %s", url);
	    if (!url) {
		step = STEP_METHOD;
                dir = -1;
	    } else {
		step = STEP_DONE;
                dir = 1;
            }
	    break;
	default:
	    break;
	}
	
    }

    return url;
}

static int kickstartDevices(struct knownDevices * kd, moduleInfoSet modInfo, 
			    moduleList modLoaded, moduleDeps * modDepsPtr, 
			    int flags) {
    char ** ksArgv = NULL;
    int ksArgc, rc;
    char * opts, * device, * type;
    char ** optv;
    poptContext optCon;
    int doContinue, missingOkay;	/* obsolete */
    char * fsType = "ext2";
    char * fsDevice = NULL;
    struct moduleInfo * mi;
    struct driverDiskInfo * ddi;
    struct poptOption diskTable[] = {
	    { "type", 't', POPT_ARG_STRING, &fsType, 0 },
	    { 0, 0, 0, 0, 0 }
	};
    struct poptOption table[] = {
	    { "continue", '\0', POPT_ARG_STRING, &doContinue, 0 },
	    { "missingok", '\0', POPT_ARG_STRING, &missingOkay, 0 },
	    { "opts", '\0', POPT_ARG_STRING, &opts, 0 },
	    { 0, 0, 0, 0, 0 }
	};

    if (!ksGetCommand(KS_CMD_DRIVERDISK, NULL, &ksArgc, &ksArgv)) {
	optCon = poptGetContext(NULL, ksArgc, (const char **) ksArgv, diskTable, 0);

	ddi = calloc(sizeof(*ddi), 1);

	do {
	    if ((rc = poptGetNextOpt(optCon)) < -1) {
		logMessage("bad argument to kickstart driverdisk command "
			"%s: %s",
		       poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
		       poptStrerror(rc));
		break;
	    }

	    fsDevice = (char *) poptGetArg(optCon);

	    if (!fsDevice || poptGetArg(optCon)) {
		logMessage("bad arguments to kickstart driverdisk command");
		break;
	    } 

	    ddi->fs = strdup(fsType);

	    if (strcmp(ddi->fs, "nfs")) {
		ddi->device = strdup(fsDevice);
		ddi->mntDevice = "/tmp/disk";

		devMakeInode(ddi->device, ddi->mntDevice);
	    } else {
		ddi->mntDevice = fsDevice;
	    }

	    logMessage("looking for driver disk (%s, %s, %s)",
		       ddi->fs, ddi->device, ddi->mntDevice);

	    if (doPwMount(ddi->mntDevice, "/tmp/drivers", ddi->fs, 1, 0, 
			  NULL, NULL)) {
		logMessage("failed to mount %s", ddi->mntDevice);
		break;
	    } 

	    if (devInitDriverDisk(modInfo, modLoaded, modDepsPtr, flags,
				  "/tmp/drivers", ddi)) {
		logMessage("driver information missing!");
	    }

	    umount("/tmp/drivers");
	} while (0);
    }

    ksArgv = NULL;
    while (!ksGetCommand(KS_CMD_DEVICE, ksArgv, &ksArgc, &ksArgv)) {
	opts = NULL;

	optCon = poptGetContext(NULL, ksArgc, (const char **) ksArgv, table, 0);

	if ((rc = poptGetNextOpt(optCon)) < -1) {
	    logMessage("bad argument to kickstart device command %s: %s",
		       poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
		       poptStrerror(rc));
	    continue;
	}

	type = (char *) poptGetArg(optCon);
	device = (char *) poptGetArg(optCon);

	if (!type || !device || poptGetArg(optCon)) {
	    logMessage("bad arguments to kickstart device command");
	    poptFreeContext(optCon);
	    continue;
	}

        if (!(mi = isysFindModuleInfo(modInfo, device))) {
	    logMessage("unknown module %s", device);
	    continue;
	}

	logMessage("found information on module %s", device);

        if (opts)
	    poptParseArgvString(opts, &rc, (const char ***) &optv);
	else
	    optv = NULL;

	rc = mlLoadModule(device, modLoaded, 
			  *modDepsPtr, optv, modInfo, flags);
	if (optv) free(optv);

	if (rc)
	    logMessage("module %s failed to insert", device);
	else
	    logMessage("module %s inserted successfully", device);
    }

    if (!ksGetCommand(KS_CMD_DEVICEPROBE, ksArgv, &ksArgc, &ksArgv)) {
	if (ksArgc != 1) {
	    logMessage("unexpected arguments to deviceprobe command");
	}

	logMessage("forcing device probe");

	busProbe(modInfo, modLoaded, *modDepsPtr, 0, kd, flags);
    }

    kdFindScsiList(kd, 0);
    kdFindNetList(kd, 0);

    return 0;
}

static char * setupKickstart(char * location, struct knownDevices * kd,
    		             moduleInfoSet modInfo,
			     moduleList modLoaded,
		             moduleDeps * modDepsPtr, int * flagsPtr,
			     char * netDevice) {
    char ** ksArgv;
    int ksArgc;
    int ksType;
    int i, rc;
    int flags = *flagsPtr;
    enum deviceClass ksDeviceType;
    struct poptOption * table;
    poptContext optCon;
    char * dir = NULL;
    char * imageUrl;
#ifdef INCLUDE_NETWORK
    struct iurlinfo ui;
    char * chptr;
    static struct networkDeviceConfig netDev;
    char * host = NULL, * url = NULL, * proxy = NULL, * proxyport = NULL;
    char * fullPath, *isopath;

    struct poptOption ksNfsOptions[] = {
	    { "server", '\0', POPT_ARG_STRING, &host, 0 },
	    { "dir", '\0', POPT_ARG_STRING, &dir, 0 },
	    { 0, 0, 0, 0, 0 }
	};
    
    struct poptOption ksUrlOptions[] = {
	    { "url", '\0', POPT_ARG_STRING, &url, 0 },
	    { "proxy", '\0', POPT_ARG_STRING, &proxy, 0 },
	    { "proxyport", '\0', POPT_ARG_STRING, &proxyport, 0 },
	    { 0, 0, 0, 0, 0 }
	};
#endif
#ifdef INCLUDE_LOCAL
    char * partname = NULL;
    struct poptOption ksHDOptions[] = {
	    { "dir", '\0', POPT_ARG_STRING, &dir, 0 },
	    { "partition", '\0', POPT_ARG_STRING, &partname, 0 },
	    { 0, 0, 0, 0, 0 }
    };
#endif

    kickstartDevices(kd, modInfo, modLoaded, modDepsPtr, flags);

    if (0) {
#ifdef INCLUDE_NETWORK
    } else if (ksHasCommand(KS_CMD_NFS)) {
	ksDeviceType = CLASS_NETWORK;
	ksType = KS_CMD_NFS;
	table = ksNfsOptions;
    } else if (ksHasCommand(KS_CMD_URL)) {
	ksDeviceType = CLASS_NETWORK;
	ksType = KS_CMD_URL;
	table = ksUrlOptions;
#endif
#ifdef INCLUDE_LOCAL
    } else if (ksHasCommand(KS_CMD_CDROM)) {
	ksDeviceType = CLASS_CDROM;
	ksType = KS_CMD_CDROM;
	table = NULL;
    } else if (ksHasCommand(KS_CMD_HD)) {
	ksDeviceType = CLASS_UNSPEC;
	ksType = KS_CMD_HD;
	table = ksHDOptions;
#endif
    } else {
	logMessage("no install method specified for kickstart");
	return NULL;
    }

    if (ksDeviceType != CLASS_UNSPEC) {
	if (!netDevice) {
	    for (i = 0; i < kd->numKnown; i++)
		if (kd->known[i].class == ksDeviceType) break;

	    if (i == kd->numKnown) {
		logMessage("no appropriate device for kickstart method is "
			   "available");
		return NULL;
	    }

	    netDevice = kd->known[i].name;
	}

	logMessage("kickstarting through device %s", netDevice);
    }

    if (!ksGetCommand(KS_CMD_XDISPLAY, NULL, &ksArgc, &ksArgv)) {
	setenv("DISPLAY", ksArgv[1], 1);
    }

    if (!ksGetCommand(KS_CMD_TEXT, NULL, &ksArgc, &ksArgv))
	(*flagsPtr) = (*flagsPtr) | LOADER_FLAGS_TEXT;

    if (table) {
	ksGetCommand(ksType, NULL, &ksArgc, &ksArgv);

	optCon = poptGetContext(NULL, ksArgc, (const char **) ksArgv, table, 0);

	if ((rc = poptGetNextOpt(optCon)) < -1) {
	    logMessage("bad argument to kickstart method command %s: %s",
		       poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
		       poptStrerror(rc));
	    return NULL;
	}
    }

    chooseKeyboard(NULL, NULL, flags);

#ifdef INCLUDE_NETWORK
    if (ksType == KS_CMD_NFS || ksType == KS_CMD_URL) {
	startNewt(flags);
	if (kickstartNetwork(&netDevice, &netDev, NULL, flags)) return NULL;
	writeNetInfo("/tmp/netinfo", &netDev, kd);
    }
#endif

    imageUrl = NULL;

#ifdef INCLUDE_NETWORK
    if (ksType == KS_CMD_NFS) {
	int count = 0;
	fullPath = alloca(strlen(host) + strlen(dir) + 2);
	sprintf(fullPath, "%s:%s", host, dir);

	logMessage("mounting nfs path %s", fullPath);

	while (count < 3
	       && doPwMount(fullPath, "/mnt/source", "nfs", 1, 0, NULL, NULL)) {
	    logMessage("mount failed, retrying after 3 second sleep");
	    sleep(3);
	    count++;
	}
	
	if (count == 3)
	    return NULL;

	if (!access("/mnt/source/RedHat/base/stage2.img", R_OK)) {
	    if (!mountLoopback("/mnt/source/RedHat/base/stage2.img",
			       "/mnt/runtime", "loop0")) {
		rmdir("/mnt/source");
		symlink("/mnt/source", "/mnt/source");
		useMntSourceUpdates("/mnt/source/RedHat/base/updates.img");
		imageUrl = "nfs://mnt/source/.";
	    }
	} else if ((isopath = validIsoImages("/mnt/source"))) {
	    useMntSourceUpdates("/mnt/source/updates.img");

	    if (mountLoopback(isopath, "/mnt/source2", "loop1"))
		logMessage("failed to mount iso loopback!");
	    else {
		if (mountLoopback("/mnt/source2/RedHat/base/stage2.img",
				  "/mnt/runtime", "loop0"))
		    logMessage("failed to mount install loopback!");
		else {
		    useMntSourceUpdates("/mnt/source2/RedHat/base/updates.img");
		    imageUrl = "nfsiso:/mnt/source";
		}
	    }
	} else {
	    logMessage("No valid tree or isos found in %s", fullPath);
	    umount("/mnt/source");
	    return NULL;
	}

	if (!imageUrl)
	    return NULL;

    } else if (ksType == KS_CMD_URL) {
        char * finalPrefix;
	char * login;
	memset(&ui, 0, sizeof(ui));

	imageUrl = strdup(url);

	if (!strncmp("ftp://", url, 6)) {
	    ui.protocol = URL_METHOD_FTP;
	    url += 6;

	    /* There could be a username/password on here */
	    if ((chptr = strchr(url, '@'))) {
		if ((chptr = strchr(url, ':'))) {
		    *chptr = '\0';
		    ui.login = strdup(url);
		    url = chptr + 1;

		    chptr = strchr(url, '@');
		    *chptr = '\0';
		    ui.password = strdup(url);
		    url = chptr + 1;
		} else {
		    *chptr = '\0';
		    ui.login = strdup(url);
		    url = chptr + 1;
		}
	    }
	} else if (!strncmp("http://", url, 7)) {
	    ui.protocol = URL_METHOD_HTTP;
	    url +=7;
	} else {
	    logMessage("unknown url protocol '%s'", url);
	    return NULL;
	}

	/* url is left pointing at the hostname */
	chptr = strchr(url, '/');
	*chptr = '\0';
	ui.address = strdup(url);
	url = chptr;
	*url = '/';
	ui.prefix = strdup(url);

	logMessage("url address %s", ui.address);
	logMessage("url prefix %s", ui.prefix);

	if (loadUrlImages(&ui, flags)) {
	    logMessage("failed to retrieve second stage");
	    return NULL;
	}

	/* now that we've loaded images, the url could have changed to handle
	   the multi-disc loopback stuff */
	if (!strcmp(ui.prefix, "/"))
	    finalPrefix = "/.";
	else
	    finalPrefix = ui.prefix;

	login = "";
	login = getLoginName(login, ui);

	url = malloc(strlen(finalPrefix) + 25 + strlen(ui.address) + strlen(login));
	sprintf(url, "%s://%s%s/%s", 
		ui.protocol == URL_METHOD_FTP ? "ftp" : "http",
		login, ui.address, finalPrefix);
    }
#endif

#ifdef INCLUDE_LOCAL
    if (ksType == KS_CMD_CDROM) {
	imageUrl = setupCdrom(NULL, location, kd, modInfo, modLoaded, 
			  modDepsPtr, flags, 1, 1);
    } else if (ksType == KS_CMD_HD) {
	char *hdfstypes[]={"ext2", "vfat", "ufs", NULL};
	int i;

	if (!strncmp(partname, "/dev/", 5))
	    partname += 5;

	logMessage("partname is %s", partname);

	for (i=0; hdfstypes[i]; i++) {
	    logMessage("Trying to find hdtree %s %s %s", partname, hdfstypes[i], dir);
	    imageUrl = setupOldHardDrive(partname, hdfstypes[i], dir, flags);
	    if (imageUrl)
		break;
	}

	if (!imageUrl) {
	    for (i=0; hdfstypes[i]; i++) {
		logMessage("Trying to find hdiso %s %s %s", partname, hdfstypes[i], dir);
		imageUrl = setupIsoImages(partname, hdfstypes[i], dir, flags);
		if (imageUrl) {
		    logMessage("returned imageUrl = %s", imageUrl);
		    break;
		}
	    }
	}

	if (!imageUrl)
	    logMessage ("Failed to mount hd kickstart media");
    }
#endif

   return imageUrl;
}

static int parseCmdLineFlags(int flags, char * cmdLine, char ** ksSource,
			     char ** ksDevice, char ** instClass, char *extraArgs[]) {
    int fd;
    char buf[500];
    int len;
    char ** argv;
    int argc;
    int i;
    int numExtraArgs = 0;

    logMessage("here with cmdLine %s", cmdLine);

    if (!cmdLine) {
	if ((fd = open("/proc/cmdline", O_RDONLY)) < 0) return flags;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0) return flags;

	buf[len] = '\0';
	cmdLine = buf;
    }

    if (poptParseArgvString(cmdLine, &argc, (const char ***) &argv)) return flags;

    for (i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i], "expert"))
	    flags |= (LOADER_FLAGS_EXPERT | LOADER_FLAGS_MODDISK | 
		      LOADER_FLAGS_ASKMETHOD);
	else if (!strcasecmp(argv[i], "askmethod"))
	    flags |= LOADER_FLAGS_ASKMETHOD;
        else if (!strcasecmp(argv[i], "telnet"))
	    flags |= LOADER_FLAGS_TELNETD;
        else if (!strcasecmp(argv[i], "noshell"))
	    flags |= LOADER_FLAGS_NOSHELL;
	else if (!strcasecmp(argv[i], "mediacheck"))
            flags |= LOADER_FLAGS_MEDIACHECK;
	else if (!strcasecmp(argv[i], "nousbstorage"))
  	    flags |= LOADER_FLAGS_NOUSBSTORAGE;
        else if (!strcasecmp(argv[i], "nousb"))
	    flags |= LOADER_FLAGS_NOUSB;
        else if (!strcasecmp(argv[i], "nofirewire"))
	    flags |= LOADER_FLAGS_NOIEEE1394;
        else if (!strcasecmp(argv[i], "noprobe"))
	    flags |= LOADER_FLAGS_NOPROBE;
        else if (!strcasecmp(argv[i], "nopcmcia"))
	    flags |= LOADER_FLAGS_NOPCMCIA;
        else if (!strcasecmp(argv[i], "text"))
	    flags |= LOADER_FLAGS_TEXT;
        else if (!strcasecmp(argv[i], "updates"))
	    flags |= LOADER_FLAGS_UPDATES;
	else if (!strncasecmp(argv[i], "class=", 6))
	    *instClass = argv[i] + 6;
        else if (!strcasecmp(argv[i], "isa"))
	    flags |= LOADER_FLAGS_ISA;
        else if (!strcasecmp(argv[i], "mcheck"))
	    flags |= LOADER_FLAGS_MCHECK;
        else if (!strcasecmp(argv[i], "dd"))
	    flags |= LOADER_FLAGS_MODDISK;
        else if (!strcasecmp(argv[i], "driverdisk"))
	    flags |= LOADER_FLAGS_MODDISK;
        else if (!strcasecmp(argv[i], "rescue"))
	    flags |= LOADER_FLAGS_RESCUE;
        else if (!strcasecmp(argv[i], "nopass"))
	    flags |= LOADER_FLAGS_NOPASS;
	else if (!strncasecmp(argv[i], "ksdevice=", 9)) {
	    *ksDevice = argv[i] + 9;
	} else if (!strcasecmp(argv[i], "serial"))
	    flags |= LOADER_FLAGS_SERIAL;
        else if (!strcasecmp(argv[i], "ks")) {
	    flags |= LOADER_FLAGS_KSNFS;
	    *ksSource = NULL;
        } else if (!strncasecmp(argv[i], "ks=cdrom:", 9)) {
	    flags |= LOADER_FLAGS_KSCDROM;
	    *ksSource = argv[i] + 9;
        } else if (!strncasecmp(argv[i], "ks=nfs:", 7)) {
	    flags |= LOADER_FLAGS_KSNFS;
	    *ksSource = argv[i] + 7;
	} else if (!strncasecmp(argv[i], "ks=http://", 10)) {
            flags |= LOADER_FLAGS_KSHTTP;
	    *ksSource = argv[i] + 10;
        } else if (!strcasecmp(argv[i], "ks=floppy"))
	    flags |= LOADER_FLAGS_KSFLOPPY;
	else if (!strncasecmp(argv[i], "display=", 8))
	    setenv("DISPLAY", argv[i] + 8, 1);
        else if (!strncasecmp(argv[i], "ks=hd:", 6)) {
	    flags |= LOADER_FLAGS_KSHD;
	    *ksSource = argv[i] + 6;
        } else if (!strncasecmp(argv[i], "ks=file:", 8)) {
	    flags |= LOADER_FLAGS_KSFILE;
	    *ksSource = argv[i] + 8;
	} else if (!strncasecmp(argv[i], "lang=", 5)) {
	    /* For Japanese, we have two options.  We should just
	       display them so we don't have to start kon if it is not needed. */
#ifndef INCLUDE_KON
	    setLanguage (argv[i] + 5, flags);
	    defaultLang = 1;
#endif
	} else if (numExtraArgs < (MAX_EXTRA_ARGS - 1)) {
	    /* go through and append args we just want to pass on to */
	    /* the anaconda script, but don't want to represent as a */
	    /* LOADER_FLAG_XXX since loader doesn't care about these */
	    /* particular options.                                   */
	    if (!strncasecmp(argv[i], "resolution=", 11) ||
		!strncasecmp(argv[i], "lowres", 6) ||
		!strncasecmp(argv[i], "skipddc", 7) ||
		!strncasecmp(argv[i], "nomount", 7)) {
		int arglen;

		arglen = strlen(argv[i])+3;
		extraArgs[numExtraArgs] = (char *) malloc(arglen*sizeof(char));
		snprintf(extraArgs[numExtraArgs], arglen, "--%s", argv[i]);
		numExtraArgs = numExtraArgs + 1;
		
		if (numExtraArgs > (MAX_EXTRA_ARGS - 2)) {
		    logMessage("Too many command line arguments (128), "
			       "rest will be dropped.");
		}
	    }
	}
    }

    /* NULL terminate the array of extra args */
    extraArgs[numExtraArgs] = NULL;

    return flags;
}

#ifdef INCLUDE_NETWORK
int kickstartFromNfs(struct knownDevices * kd, char * location, 
		     moduleInfoSet modInfo, moduleList modLoaded, 
		     moduleDeps * modDepsPtr, int flags, char * ksSource,
		     char * ksDevice) {
    struct networkDeviceConfig netDev;
    char * file, * fullFn;
    char * ksPath;
    char * devName;

    if (!ksDevice) {
	if (ensureNetDevice(kd, modInfo, modLoaded, modDepsPtr, flags, 
			    &devName))
	    return 1;
    } else {
	devName = ksDevice;
    }

    if (kickstartNetwork(&devName, &netDev, "dhcp", flags)) {
        logMessage("no dhcp response received");
	return 1;
    }

    writeNetInfo("/tmp/netinfo", &netDev, kd);

    if (!(netDev.dev.set & PUMP_INTFINFO_HAS_NEXTSERVER)) {
	logMessage("no bootserver was found");
	return 1;
    }

    if (!(netDev.dev.set & PUMP_INTFINFO_HAS_BOOTFILE)) {
	file = "/kickstart/";
	logMessage("bootp: no bootfile received");
    } else {
	file = netDev.dev.bootFile;
    }

    if (ksSource) {
	ksPath = alloca(strlen(ksSource) + 1);
	strcpy(ksPath, ksSource);
    } else {
	ksPath = alloca(strlen(file) + 
			strlen(inet_ntoa(netDev.dev.nextServer)) + 70);
	strcpy(ksPath, inet_ntoa(netDev.dev.nextServer));
	strcat(ksPath, ":");
	strcat(ksPath, file);
    }

    if (ksPath[strlen(ksPath) - 1] == '/') {
	ksPath[strlen(ksPath) - 1] = '\0';
	file = malloc(30);
	sprintf(file, "%s-kickstart", inet_ntoa(netDev.dev.ip));
    } else {
	file = strrchr(ksPath, '/');
	if (!file) {
	    file = ksPath;
	    ksPath = "/";
	} else {
	    *file++ = '\0';
	}
    }

    logMessage("ks server: %s file: %s", ksPath, file);

    if (doPwMount(ksPath, "/tmp/nfskd", "nfs", 1, 0, NULL, NULL)) {
	logMessage("failed to mount %s", ksPath);
	return 1;
    }

    fullFn = malloc(strlen(file) + 20);
    sprintf(fullFn, "/tmp/nfskd/%s", file);
    copyFile(fullFn, location);

    umount("/tmp/nfskd");

    return 0;
}

int kickstartFromHttp(struct knownDevices * kd, char * location, 
		     moduleInfoSet modInfo, moduleList modLoaded, 
		     moduleDeps * modDepsPtr, int flags, char * ksSource,
		     char * ksDevice) {
    struct networkDeviceConfig netDev;
    struct iurlinfo ui;
    char * file;
    char * ksPath;
    char * devName;
    char * chptr;
    int fd, rc;

    memset(&ui, 0, sizeof(ui));

    if (!ksDevice) {
        if (ensureNetDevice(kd, modInfo, modLoaded, modDepsPtr, flags, 
                            &devName))
            return 1;
    } else {
        devName = ksDevice;
    }

    if (kickstartNetwork(&devName, &netDev, "dhcp", flags)) {
        logMessage("no dhcp response received");
        return 1;
    }

    writeNetInfo("/tmp/netinfo", &netDev, kd);

    if (ksSource) {
        ksPath = alloca(strlen(ksSource) + 1);
        strcpy(ksPath, ksSource);
    } else {
        logMessage("no location specified");
        return 1;
    }

    if (ksPath[strlen(ksPath) - 1] == '/') {
        ksPath[strlen(ksPath) - 1] = '\0';
        file = malloc(30);
        sprintf(file, "%s-kickstart", inet_ntoa(netDev.dev.ip));
    } else {
        file = strrchr(ksPath, '/');
        if (!file) {
	    file = ksPath;
	    ksPath = "/";
        } else {
	    *file++ = '\0';
        }
    }

    logMessage("ks location: http://%s/%s", ksPath, file);

    ui.protocol = URL_METHOD_HTTP;
    chptr = strchr(ksPath, '/');
    if (chptr == NULL) {
        ui.address = strdup(ksPath);
        ui.prefix = strdup("/");
    } else {
        *chptr = '\0';
        ui.address = strdup(ksPath);
        ksPath = chptr;
        *ksPath = '/';
        ui.prefix = strdup(ksPath);
    }

    fd = urlinstStartTransfer(&ui, file, 1);
    if (fd < 0) {
        logMessage("failed to retrieve http://%s/%s/%s", ui.address, ui.prefix, file);
        return 1;
    }

    rc = copyFileFd(fd, "/tmp/ks.cfg");
    if (rc) {
        unlink("/tmp/ks.cfg");
        logMessage("failed to copy ks.cfg to /tmp/ks.cfg");
        return 1;
    }

    urlinstFinishTransfer(&ui, fd);

    return 0;
}
#endif

int kickstartFromHardDrive(char * location, 
			   moduleList modLoaded, moduleDeps * modDepsPtr, 
			   char * source, int flags) {
    char * device;
    char * fileName;
    char * fullFn;

#ifdef __sparc__
    mlLoadModuleSet("ufs", modLoaded, *modDepsPtr, NULL, flags);
#endif

    fileName = strchr(source, '/');
    /* XXX FIX ME FOR THE NEXT RELEASE */
    /* change syntax to ks=hd:[device]:/path/to/ks.cfg */
    if (!strncmp (source, "cciss", 5) ||
	!strncmp (source, "ida", 3) ||
	!strncmp (source, "i2o", 3) ||
	!strncmp (source, "rd", 2)) {
	/* chomp in the next part */
	fileName++;
	fileName = strchr(fileName, '/');
    }
    *fileName = '\0';
    fileName++;
    device = source;

    if (devMakeInode(device, "/tmp/hddevice")) {
	logMessage("failed to make device %s", device);
	return 1;
    }

    if (doPwMount("/tmp/hddevice", "/mnt/hddrive", "ext2", 1, 0, 
		  NULL, NULL) &&
	doPwMount("/tmp/hddevice", "/mnt/hddrive", "vfat", 1, 0, 
		  NULL, NULL)) {
	logMessage("failed to mount %s", device);
    }

    fullFn = alloca(strlen(fileName) + 20);
    sprintf(fullFn, "/mnt/hddrive/%s", fileName);
    copyFile(fullFn, location);

    umount("/mnt/hddrive");

    return 0;
}

int kickstartFromFloppy(char * location, moduleList modLoaded,
			moduleDeps * modDepsPtr, int flags) {

    if (devMakeInode(floppyDevice, "/tmp/floppy"))
	return 1;

    if ((doPwMount("/tmp/floppy", "/tmp/ks", "vfat", 1, 0, NULL, NULL)) && 
	doPwMount("/tmp/floppy", "/tmp/ks", "ext2", 1, 0, NULL, NULL)) {
	logMessage("failed to mount floppy: %s", strerror(errno));
	return 1;
    }

    if (access("/tmp/ks/ks.cfg", R_OK)) {
	newtWinMessage(_("Error"), _("OK"),
		_("Cannot find ks.cfg on boot floppy."));
	return 1;
    }

    copyFile("/tmp/ks/ks.cfg", location);

    umount("/tmp/ks");
    unlink("/tmp/floppy");

    logMessage("kickstart file copied to %s", location);

    return 0;
}

/* Recursive */
int copyDirectory(char * from, char * to) {
    DIR * dir;
    struct dirent * ent;
    int fd, outfd;
    char buf[4096];
    int i;
    struct stat sb;
    char filespec[256];
    char filespec2[256];
    char link[1024];

    mkdir(to, 0755);

    if (!(dir = opendir(from))) {
	newtWinMessage(_("Error"), _("OK"),
		       _("Failed to read directory %s: %s"),
		       from, strerror(errno));
	return 1;
    }

    errno = 0;
    while ((ent = readdir(dir))) {
	if (ent->d_name[0] == '.') continue;

	sprintf(filespec, "%s/%s", from, ent->d_name);
	sprintf(filespec2, "%s/%s", to, ent->d_name);

	lstat(filespec, &sb);

	if (S_ISDIR(sb.st_mode)) {
	    logMessage("recursively copying %s", filespec);
	    if (copyDirectory(filespec, filespec2)) return 1;
	} else if (S_ISLNK(sb.st_mode)) {
	    i = readlink(filespec, link, sizeof(link) - 1);
	    link[i] = '\0';
	    if (symlink(link, filespec2)) {
		logMessage("failed to symlink %s to %s: %s",
		    filespec2, link, strerror(errno));
	    }
	} else {
	    fd = open(filespec, O_RDONLY);
	    if (fd < 0) {
		logMessage("failed to open %s: %s", filespec,
			   strerror(errno));
		return 1;
	    } 
	    outfd = open(filespec2, O_RDWR | O_TRUNC | O_CREAT, 0644);
	    if (outfd < 0) {
		logMessage("failed to create %s: %s", filespec2,
			   strerror(errno));
	    } else {
		fchmod(outfd, sb.st_mode & 07777);

		while ((i = read(fd, buf, sizeof(buf))) > 0)
		    write(outfd, buf, i);
		close(outfd);
	    }

	    close(fd);
	}

	errno = 0;
    }

    closedir(dir);

    return 0;
}

void loadUpdates(struct knownDevices *kd, moduleList modLoaded,
	         moduleDeps * modDepsPtr, int flags) {
    int done = 0;
    int rc;

    startNewt(flags);

    do { 
	rc = newtWinChoice(_("Updates Disk"), _("OK"), _("Cancel"),
		_("Insert your updates disk and press \"OK\" to continue."));

	if (rc == 2) return;

#if 0
    _("The floppy disk you inserted is not a valid update disk "
      "for this release of %s."), PRODUCTNAME
#endif

logMessage("UPDATES floppy device is %s", floppyDevice);

	devMakeInode(floppyDevice, "/tmp/floppy");
	if (doPwMount("/tmp/floppy", "/tmp/update-disk", "ext2", 1, 0, NULL, 
		      NULL)) {
	    newtWinMessage(_("Error"), _("OK"), 
			   _("Failed to mount floppy disk."));
	} else {
	    /* Copy everything to /tmp/updates so .so files don't get run
	       from /dev/floppy. We could (and probably should) get smarter 
	       about this at some point. */
	    winStatus(40, 3, _("Updates"), _("Reading anaconda updates..."));
	    if (!copyDirectory("/tmp/update-disk", "/tmp/updates")) done = 1;
	    newtPopWindow();
	    umount("/tmp/update-disk");
	}
    } while (!done);

    return;
}

#ifdef __sparc__
/* Don't load the large ufs module if it will not be needed
   to save some memory on lowmem SPARCs. */
void loadUfs(struct knownDevices *kd, moduleList modLoaded,
	     moduleDeps * modDepsPtr, int flags) {
    int i, j, fd, rc;
    struct partitionTable table;
    int ufsloaded = 0;

    for (i = 0; i < kd->numKnown; i++) {
	if (kd->known[i].class == CLASS_HD) {
	    devMakeInode(kd->known[i].name, "/tmp/hddevice");
	    if ((fd = open("/tmp/hddevice", O_RDONLY)) >= 0) {
		if ((rc = balkanReadTable(fd, &table))) {
		    logMessage("failed to read partition table for "
			       "device %s: %d", kd->known[i].name, rc);
		} else {
		    for (j = 0; j < table.maxNumPartitions; j++) {
			if (table.parts[j].type == BALKAN_PART_UFS) {
			    if (!ufsloaded) {
				mlLoadModuleSet("ufs", modLoaded, 
					     *modDepsPtr, NULL, flags);
				ufsloaded = 1;
			    }
			}
		    }
		}

		close(fd);
	    }
	    unlink("/tmp/hddevice");
	}
    }
}
#else
#define loadUfs(kd,modLoaded,modDepsPtr,flags) do { } while (0)
#endif

/* return value of global floppyDevice */
char *getCurrentFloppyDevice() {
    return floppyDevice;
}
void setFloppyDevice(int flags) {
#if defined(__i386__) || defined(__ia64__)
    struct device ** devices;
    int foundFd0 = 0;
    int i = 0;

    logMessage("probing for floppy devices");

    devices = probeDevices(CLASS_FLOPPY, BUS_IDE | BUS_SCSI | BUS_MISC, PROBE_ALL);

    if (!devices) {
        logMessage("no floppy devices found");
        return;
    }

    for(i=0;devices[i];i++) {
      if (devices[i]->detached == 0) {
	logMessage("first non-detached floppy is %s", devices[i]->device);
        foundFd0 = 1;
        break;
      }
    }

    if (foundFd0) {
        floppyDevice = strdup(devices[i]->device);
    }

#endif	
    logMessage("system floppy device is %s", floppyDevice);
}

static int usbInitialize(moduleList modLoaded, moduleDeps modDeps,
			 moduleInfoSet modInfo, int flags) {
    struct device ** devices;
    char * buf;

    if (FL_NOUSB(flags)) return 0;

    logMessage("looking for usb controllers");

    devices = probeDevices(CLASS_USB, BUS_PCI, PROBE_ALL);

    if (!devices) {
	logMessage("no usb controller found");
	return 0;
    }

    logMessage("found USB controller %s", devices[0]->driver);

    if (mlLoadModuleSet(devices[0]->driver, modLoaded, modDeps, modInfo, 
			flags)) {
	logMessage("failed to insert usb module");
	/* dont return, just keep going. */
	/* may have USB built into kernel */
	/* return 1; */
    }

    if (FL_TESTING(flags)) return 0;

    if (doPwMount("/proc/bus/usb", "/proc/bus/usb", "usbdevfs", 0, 0, 
		  NULL, NULL))
	logMessage("failed to mount device usbdevfs: %s", strerror(errno));

    /* sleep so we make sure usb devices get properly initialized */
    sleep(2);

    buf = alloca(40);
    sprintf(buf, "hid:keybdev%s", 
		  (FL_NOUSBSTORAGE(flags) ? "" : ":usb-storage"));
    mlLoadModuleSet(buf, modLoaded, modDeps, modInfo, flags);
    sleep(1);

    return 0;
}

static int firewireInitialize(moduleList modLoaded, moduleDeps modDeps,
			      moduleInfoSet modInfo, int flags) {
    struct device ** devices;
    int i = 0;

    if (FL_NOIEEE1394(flags)) return 0;

    devices = probeDevices(CLASS_FIREWIRE, BUS_PCI, PROBE_ALL);

    if (!devices) {
	logMessage("no firewire controller found");
	return 0;
    }

    logMessage("found firewire controller %s", devices[0]->driver);

    startNewt(flags);
    /* not the best message in the world, but better than sitting
     * and looking silly */
    winStatus(40, 3, _("Loading"), _("Loading %s driver..."), 
              devices[0]->driver);

    if (mlLoadModuleSet(devices[0]->driver, modLoaded, modDeps, modInfo,
			flags)) {
	logMessage("failed to insert firewire module");
	newtPopWindow();
	return 1;
    }

    sleep(3);
    newtPopWindow();

    logMessage("probing for firewire scsi devices");
    devices = probeDevices(CLASS_SCSI, BUS_FIREWIRE, PROBE_ALL);

    if (!devices) {
	logMessage("no firewire scsi devices found");
	return 0;
    }

    for (i=0;devices[i];i++) {
	if ((devices[i]->detached == 0) && (devices[i]->driver != NULL)) {
	    logMessage("found firewire device using %s", devices[i]->device);
	    mlLoadModuleSet(devices[i]->driver, modLoaded, modDeps, 
			    modInfo, flags);
	}
    }

    return 0;
}

/* This loads the necessary parallel port drivers for printers so that
   kudzu can autodetect and setup printers in post install*/
static void initializeParallelPort(moduleList modLoaded, moduleDeps modDeps,
				   moduleInfoSet modInfo, int flags) {
#if !defined (__i386__)
    return;
#endif
    if (FL_NOPARPORT(flags)) return;

    logMessage("loading parallel port drivers...");
    if (mlLoadModuleSet("parport_pc", modLoaded, modDeps, modInfo, flags)) {
	logMessage("failed to load parport_pc module");
	return;
    }
}

/* This forces a pause between initializing usb and trusting the /proc 
   stuff */
static void usbInitializeMouse(moduleList modLoaded, moduleDeps modDeps,
			      moduleInfoSet modInfo, int flags) {

#if defined (__s390__) && defined (__s390x__)
	return;
#else
    if (FL_NOUSB(flags)) return;

    if (access("/proc/bus/usb/devices", R_OK)) return;

    logMessage("looking for USB mouse...");
    if (probeDevices(CLASS_MOUSE, BUS_USB, PROBE_ALL)) {
	logMessage("USB mouse found, loading mousedev module");
	if (mlLoadModuleSet("mousedev", modLoaded, modDeps, modInfo, flags)) {
	    logMessage ("failed to loading mousedev module");
	    return;
	}
    }
#endif
}


static int agpgartInitialize(moduleList modLoaded, moduleDeps modDeps,
			     moduleInfoSet modInfo, int flags) {
    struct device ** devices, *p;
    int i;

#if defined (__s390__) && defined (__s390x__)
	/* obviously no agp on s/390 :) */
	return 0;
#else

    if (FL_TESTING(flags)) return 0;

    logMessage("looking for video cards requiring agpgart module");

    devices = probeDevices(CLASS_VIDEO, BUS_UNSPEC, PROBE_ALL);

    if (!devices) {
	logMessage("no video cards found");
	return 0;
    }

    /* loop thru cards, see if we need agpgart */
    for (i=0; devices[i]; i++) {
	p = devices[i];
	logMessage("found video card controller %s", p->driver);

    /* HACK - need to have list of cards which match!! */
	if (!strcmp(p->driver, "Card:Intel 810") ||
	    !strcmp(p->driver, "Card:Intel 815")) {
	    logMessage("found %s card requiring agpgart, loading module",
		       p->driver+5);
	    
	    if (mlLoadModuleSet("agpgart", modLoaded, modDeps, modInfo, 
				flags)) {
		logMessage("failed to insert agpgart module");
		return 1;
	    } else {
		/* only load it once! */
		return 0;
	    }

	}
    }

    return 0;
#endif
}

static void scsiSetup(moduleList modLoaded, moduleDeps modDeps,
			      moduleInfoSet modInfo, int flags,
			      struct knownDevices * kd) {
    mlLoadModuleSet("sd_mod:sr_mod", modLoaded, modDeps, modInfo, flags);
}

static void ideSetup(moduleList modLoaded, moduleDeps modDeps,
			      moduleInfoSet modInfo, int flags,
			      struct knownDevices * kd) {

    /* This is fast enough that we don't need a screen to pop up */
    mlLoadModuleSet("ide-cd", modLoaded, modDeps, modInfo, flags);

    kdFindIdeList(kd, 0);
}

static void dasdSetup(moduleList modLoaded, moduleDeps modDeps,
               moduleInfoSet modInfo, int flags,
               struct knownDevices * kd) {
    mlLoadModuleSet("dasd_eckd_mod:dasd_mod", modLoaded, modDeps, modInfo, flags);
    kdFindDasdList(kd, 0);
}

static void checkForRam(int flags) {
    if (!FL_EXPERT(flags) && (totalMemory() < MIN_RAM)) {
	char *buf;
	buf = sdupprintf(_("You do not have enough RAM to install %s "
			   "on this machine."), PRODUCTNAME);
	startNewt(flags);
	newtWinMessage(_("Error"), _("OK"), buf);
	free(buf);
	stopNewt();
	exit(0);
    }
}

/* verify that the stamp files in / of the initrd and the stage2 match */
static void verifyImagesMatched() {
    char *stamp1;
    char *stamp2;
    FILE *f;

    stamp1 = alloca(13);
    stamp2 = alloca(13);

    /* grab the one from the initrd */
    f = fopen("/.buildstamp", "r");
    if (!f) {
	/* hrmm... not having them won't be fatal for now */
	return;
    }
    fgets(stamp1, 13, f);

    /* and the runtime */
    f = fopen("/mnt/runtime/.buildstamp", "r");
    if (!f) {
	return;
    }
    fgets(stamp2, 13, f);

    if (strncmp(stamp1, stamp2, 12) != 0) {
	newtWinMessage(_("Error"), _("OK"),
		       _("The second stage of the install which you have "
			 "selected does not match the boot disk which you "
			 "are using.  This shouldn't happen, and I'm "
			 "rebooting your system now."));
	stopNewt();
	exit(1);
    }
}

static int checkFrameBuffer() {
    int fd;
    int rc = 0;
    struct fb_fix_screeninfo fix;

    if ((fd = open("/dev/fb0", O_RDONLY)) == -1) {
	return 0;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) >= 0) {
	rc = 1;
    }
    close(fd);
    return rc;
}


int main(int argc, char ** argv) {
    char ** argptr;
    char * anacondaArgs[50];
    char * arg, * url = NULL;
    poptContext optCon;
    int probeOnly = 0;
    moduleList modLoaded;
    char * cmdLine = NULL;
    moduleDeps modDeps;
    int i, rc;
    int flags = 0;
    int testing = 0;
    int mediacheck = 0;
    int useRHupdates = 0;
    char * lang = NULL;
    char * keymap = NULL;
    char * kbdtype = NULL;
    char * instClass = NULL;
    struct knownDevices kd;
    moduleInfoSet modInfo;
    char * where;
    char ** tmparg;
#ifdef INCLUDE_PCMCIA
    char pcic[20] = "";
#endif
    struct moduleInfo * mi;
    char twelve = 12;
    char * ksFile = NULL, * ksSource = NULL;
    char * ksNetDevice = NULL;
    char * extraArgs[MAX_EXTRA_ARGS];
    struct stat sb;
    struct poptOption optionTable[] = {
    	    { "cmdline", '\0', POPT_ARG_STRING, &cmdLine, 0 },
	    { "ksfile", '\0', POPT_ARG_STRING, &ksFile, 0 },
	    { "probe", '\0', POPT_ARG_NONE, &probeOnly, 0 },
	    { "test", '\0', POPT_ARG_NONE, &testing, 0 },
	    { "mediacheck", '\0', POPT_ARG_NONE, &mediacheck, 0},
	    { 0, 0, 0, 0, 0 }
    };

    if (!strcmp(argv[0] + strlen(argv[0]) - 6, "insmod"))
	return ourInsmodCommand(argc, argv);
    else if (!strcmp(argv[0] + strlen(argv[0]) - 5, "rmmod"))
	return combined_insmod_main(argc, argv);
    else if (!strcmp(argv[0] + strlen(argv[0]) - 8, "modprobe"))
	return ourInsmodCommand(argc, argv);

#ifdef INCLUDE_KON
    else if (!strcmp(argv[0] + strlen(argv[0]) - 3, "kon")) {
        setenv("TERM", "kon", 1);
	i = kon_main(argc, argv);
	return i;
    } else if (!strcmp(argv[0] + strlen(argv[0]) - 8, "continue")) {
        setenv("TERM", "kon", 1);
	continuing = 1;
    }
#endif

#ifdef INCLUDE_PCMCIA
    else if (!strcmp(argv[0] + strlen(argv[0]) - 7, "cardmgr"))
	return cardmgr_main(argc, argv);
    else if (!strcmp(argv[0] + strlen(argv[0]) - 5, "probe"))
	return probe_main(argc, argv);
#endif

    /* The fstat checks disallows serial console if we're running through
       a pty. This is handy for Japanese. */
    fstat(0, &sb);
    if (major(sb.st_rdev) != 3 && major(sb.st_rdev) != 136) {
	if (ioctl (0, TIOCLINUX, &twelve) < 0)
	    flags |= LOADER_FLAGS_SERIAL;
    }

#if defined (__s390__) || defined (__s390x__)
    textdomain("anaconda");
    /* modules.conf is already written on S/390 by linuxrc. There`s no
     * parallel port on S/390
     */
#else

    /* don't start modules.conf if continuing as there could be modules 
       already loaded from a driver disk */
    if ((!FL_TESTING(flags)) && !continuing) {
        int fd;

	fd = open("/tmp/modules.conf", O_WRONLY | O_CREAT, 0666);
	if (fd < 0) {
	    logMessage("error creating /tmp/modules.conf: %s\n", 
	    	       strerror(errno));
	} else {
	    /* HACK - notting */
#ifdef __sparc__
	    write(fd,"alias parport_lowlevel parport_ax\n",34);
#else
	    write(fd,"alias parport_lowlevel parport_pc\n",34);
#endif
	    close(fd);
	}
    }
#endif

    optCon = poptGetContext(NULL, argc, (const char **) argv, optionTable, 0);

    if ((rc = poptGetNextOpt(optCon)) < -1) {
	fprintf(stderr, "bad option %s: %s\n",
		       poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
		       poptStrerror(rc));
	exit(1);
    }

    if ((arg = (char *) poptGetArg(optCon))) {
	fprintf(stderr, "unexpected argument: %s\n", arg);
	exit(1);
    }

    /* Turn on mediacheck for testing */
    /*    mediacheck = 1; */

    if (testing) flags |= LOADER_FLAGS_TESTING;
    if (mediacheck) flags |= LOADER_FLAGS_MEDIACHECK;

    /* we can't do kon on fb console (#60844) */
    if (checkFrameBuffer() == 1)  haveKon = 0;

#if defined (__s390__) && !defined (__s390x__)
    flags |= LOADER_FLAGS_NOSHELL | LOADER_FLAGS_NOUSB;
#endif
    
    extraArgs[0] = NULL;
    flags = parseCmdLineFlags(flags, cmdLine, &ksSource, &ksNetDevice,
			      &instClass, extraArgs);

    if (FL_SERIAL(flags) && !getenv("DISPLAY"))
	flags |= LOADER_FLAGS_TEXT;

    arg = FL_TESTING(flags) ? "./module-info" : "/modules/module-info";
    modInfo = isysNewModuleInfoSet();

    if (isysReadModuleInfo(arg, modInfo, NULL)) {
        fprintf(stderr, "failed to read %s\n", arg);
	sleep(5);
	exit(1);
    }

#if !defined (__s390__) && !defined (__s390x__)
    openLog(FL_TESTING(flags));
#else
    openLog(1);
#endif
    if (!FL_TESTING(flags))
		openlog("loader", 0, LOG_LOCAL0);

    checkForRam(flags);

    kd = kdInit();
    mlReadLoadedList(&modLoaded);
    modDeps = mlNewDeps();
    mlLoadDeps(&modDeps, "/modules/modules.dep");

#if !defined (__s390__) && !defined (__s390x__)
    mlLoadModuleSet("cramfs:vfat:nfs", modLoaded, modDeps, modInfo, flags);

    if (!continuing) {
	ideSetup(modLoaded, modDeps, modInfo, flags, &kd);
	scsiSetup(modLoaded, modDeps, modInfo, flags, &kd);

	/* Note we *always* do this. If you could avoid this you could get
	   a system w/o USB keyboard support, which would be bad. */
	usbInitialize(modLoaded, modDeps, modInfo, flags);

	/* now let's initialize any possible firewire.  fun */
	firewireInitialize(modLoaded, modDeps, modInfo, flags);
    }

    setFloppyDevice(flags);

    if (FL_KSFLOPPY(flags)) {
	startNewt(flags);
	ksFile = "/tmp/ks.cfg";
	kickstartFromFloppy(ksFile, modLoaded, &modDeps, flags);
	flags |= LOADER_FLAGS_KICKSTART;
    }
#else
    mlLoadModuleSet("cramfs:loop", modLoaded, modDeps, modInfo, flags);
    if (!continuing) {
	 dasdSetup(modLoaded, modDeps, modInfo, flags, &kd);
    }
#endif

#ifdef INCLUDE_KON
    if (continuing)
	setLanguage ("ja", flags);
    startNewt(flags);
#endif

#ifdef INCLUDE_PCMCIA
    startNewt(flags);

    if (!continuing && !FL_NOPCMCIA(flags)) {
	startPcmcia(floppyDevice, modLoaded, modDeps, modInfo, pcic, &kd, 
		    flags);
    }
#endif

    /* if we're in PCMCIA, we're always going to pass the PCMCIA code
       to the probe */
#ifdef INCLUDE_PCMCIA
    kdFindIdeList(&kd, CODE_PCMCIA);
    kdFindScsiList(&kd, CODE_PCMCIA);
    kdFindNetList(&kd, CODE_PCMCIA);
#else
    /* but if we're not in PCMCIA, there is a chance that we were run in
       kon mode which means that the probes were done and modules were
       inserted, but they're *not* PCMCIA */
    kdFindIdeList(&kd, continuing ? 0 : CODE_PCMCIA);
    kdFindScsiList(&kd, continuing ? 0 : CODE_PCMCIA);
    kdFindDasdList(&kd, continuing ? 0 : CODE_PCMCIA);
    kdFindNetList(&kd, continuing ? 0 : CODE_PCMCIA);
#endif

#if !defined (__s390__) && !defined (__s390x__)
    /* we have to explicitly read this to let libkudzu know we want to
       merge in future tables rather then replace the initial one */
    pciReadDrivers("/modules/pcitable");

    if (!continuing) {
	if ((access("/proc/bus/pci/devices", R_OK) &&
	      access("/proc/openprom", R_OK)) || FL_MODDISK(flags)) { 
	    startNewt(flags);
	    devLoadDriverDisk(modInfo, modLoaded, &modDeps, flags, 1, 1,
			      floppyDevice);
	}
   
	busProbe(modInfo, modLoaded, modDeps, probeOnly, &kd, flags);
	if (probeOnly) exit(0);
    }
#endif

    if (FL_KSHD(flags)) {
	ksFile = "/tmp/ks.cfg";
	kickstartFromHardDrive(ksFile, modLoaded, &modDeps, ksSource, flags);
	flags |= LOADER_FLAGS_KICKSTART;
    } else if (FL_KSFILE(flags)) {
	ksFile = ksSource;
	flags |= LOADER_FLAGS_KICKSTART;
    } 

#ifdef INCLUDE_LOCAL
    if (FL_KSCDROM(flags)) {
	ksFile = "/tmp/ks.cfg";
	kickstartFromCdrom(ksFile, ksSource, &kd, modInfo, modLoaded, &modDeps,
			   flags);
	flags |= LOADER_FLAGS_KICKSTART;
    }
#endif
    
#ifdef INCLUDE_NETWORK
    if (FL_KSNFS(flags)) {
	ksFile = "/tmp/ks.cfg";
	startNewt(flags);
	if (!kickstartFromNfs(&kd, ksFile, modInfo, modLoaded, &modDeps, flags, 
			      ksSource, ksNetDevice))
	    flags |= LOADER_FLAGS_KICKSTART;
    }
    if (FL_KSHTTP(flags)) {
        ksFile = "/tmp/ks.cfg";
	startNewt(flags);
	if (!kickstartFromHttp(&kd, ksFile, modInfo, modLoaded, &modDeps, flags,
                               ksSource, ksNetDevice))
	    flags |= LOADER_FLAGS_KICKSTART;
    }
#endif

    if (ksFile) {
	startNewt(flags);
	ksReadCommands(ksFile);
	url = setupKickstart("/mnt/source", &kd, modInfo, modLoaded, &modDeps, 
			     &flags, ksNetDevice);
    }

#ifdef INCLUDE_NETWORK
    if (FL_TELNETD(flags)) {
	struct networkDeviceConfig netDev;

	if (!ksNetDevice) {
	    if (ensureNetDevice(&kd, modInfo, modLoaded, &modDeps, flags, 
				&ksNetDevice))
		return 1;
	}

	if (!FL_TESTING(flags)) {
	    kickstartNetwork(&ksNetDevice, &netDev, NULL, flags);
	    writeNetInfo("/tmp/netinfo", &netDev, &kd);
	}

	if (!beTelnet(flags)) {
	    flags |= LOADER_FLAGS_TEXT | LOADER_FLAGS_NOSHELL;
	    haveKon = 0;
	}
    }
#endif

    if (!url) {
	url = doMountImage("/mnt/source", &kd, modInfo, modLoaded, &modDeps,
			   &lang, &keymap, &kbdtype,
			   flags);
    }

    if (!FL_TESTING(flags)) {
       int fd;
     
	unlink("/usr");
	symlink("mnt/runtime/usr", "/usr");
#if !defined (__s390__) && !defined (__s390x__)
	unlink("/lib");
	symlink("mnt/runtime/lib", "/lib");
#else
#define LD_SO_CONF_STR "/lib/\n/mnt/runtime/lib\n/usr/lib\n/usr/X11R6/lib\n"
  fd = open("/etc/ld.so.conf", O_WRONLY|O_CREAT, 0644);
  if (fd >= 0) {
     const char *buf = LD_SO_CONF_STR;
     write(fd, buf, sizeof(LD_SO_CONF_STR));
     close(fd);
  }
  system("/sbin/ldconfig 2>/dev/null >/dev/null");
#endif

	unlink("/modules/modules.dep");
	unlink("/modules/module-info");
	unlink("/modules/pcitable");

	symlink("../mnt/runtime/modules/modules.dep",
		"/modules/modules.dep");
	symlink("../mnt/runtime/modules/module-info",
		"/modules/module-info");
	symlink("../mnt/runtime/modules/pcitable",
		"/modules/pcitable");

#ifndef __sparc__
	unlink("/modules/modules.cgz");

	symlink("../mnt/runtime/modules/modules.cgz",
		"/modules/modules.cgz");
#else
	/* All sparc32 modules are on the first stage image, if it is sparc64,
	   then we must keep both the old /modules/modules.cgz which may
	   either contain all modules, or the basic set + one of net or scsi
	   and we extend it with the full set of net + scsi modules. */
	symlink("../mnt/runtime/modules/modules64.cgz",
		"/modules/modules65.cgz");
#endif
    }

#if !defined (__s390__) && !defined (__s390x__)
    logMessage("getting ready to spawn shell now");

    spawnShell(flags);			/* we can attach gdb now :-) */
#endif

    verifyImagesMatched();

    /* XXX should free old Deps */
    modDeps = mlNewDeps();
    mlLoadDeps(&modDeps, "/modules/modules.dep");

#if !defined (__s390__) && !defined (__s390x__)
    /* merge in any new pci ids */
    pciReadDrivers("/modules/pcitable");
#endif

    /* We reinit this from the beginning because we could have lost drivers
       when we switched media, and we don't want to list ones that don't
       exist. This is a bit unfortunate in that we lose information on
       drivers we've loaded as well, which could include ISA drivers which
       kudzu won't reprobe! */
    modInfo = isysNewModuleInfoSet();

    if (isysReadModuleInfo(arg, modInfo, NULL)) {
        fprintf(stderr, "failed to read %s\n", arg);
	sleep(5);
	exit(1);
    }

    /* merge in drivers we know about from a driver disk so we probe things
       properly */
    ddReadDriverDiskModInfo(modInfo);

    if (ksFile)
	kickstartDevices(&kd, modInfo, modLoaded, &modDeps, flags);

    /* We may already have these modules loaded, but trying again won't
       hurt. */
    ideSetup(modLoaded, modDeps, modInfo, flags, &kd);
    scsiSetup(modLoaded, modDeps, modInfo, flags, &kd);
    dasdSetup(modLoaded, modDeps, modInfo, flags, &kd);

    busProbe(modInfo, modLoaded, modDeps, 0, &kd, flags);

    /* look for hard drives; if there aren't any warn the user and
       let him add drivers manually */
    for (i = 0; i < kd.numKnown; i++)
	if (kd.known[i].class == CLASS_HD) break;

    if (i == kd.numKnown) {
	int rc;

	startNewt(flags);
	rc = newtWinChoice(_("Warning"), _("Yes"), _("No"),
		_("No hard drives have been found. You probably need to "
		  "manually choose device drivers for the installation to "
		  "succeed. Would you like to select drivers now?"));

	if (rc != 2) flags |= LOADER_FLAGS_ISA;
    }

#if !defined (__s390__) && !defined (__s390x__)
    if (((access("/proc/bus/pci/devices", R_OK) &&
	  access("/proc/openprom", R_OK)) || 
	  FL_ISA(flags) || FL_NOPROBE(flags)) && !ksFile) {
	startNewt(flags);
	manualDeviceCheck(modInfo, modLoaded, &modDeps, &kd, flags);
    }
#endif

    if (FL_UPDATES(flags))
        loadUpdates(&kd, modLoaded, &modDeps, flags);

    loadUfs(&kd, modLoaded, &modDeps, flags);

    /* We must look for cards which require the agpgart module */
    agpgartInitialize(modLoaded, modDeps, modInfo, flags);

#if !defined (__s390__) && !defined (__s390x__)	 
    mlLoadModuleSet("raid0:raid1:raid5:msdos:ext3:reiserfs:jfs:xfs:lvm-mod", 
		    modLoaded, modDeps, modInfo, flags);
#else
    mlLoadModuleSet("raid0:raid1:raid5:ext3:jfs:xfs:lvm-mod",
          modLoaded, modDeps, modInfo, flags);
#endif

    initializeParallelPort(modLoaded, modDeps, modInfo, flags);

    usbInitializeMouse(modLoaded, modDeps, modInfo, flags);

#if 0
    for (i = 0; i < kd.numKnown; i++) {
    	printf("%-5s ", kd.known[i].name);
	if (kd.known[i].class == CLASS_CDROM)
	    printf("cdrom");
	else if (kd.known[i].class == CLASS_HD)
	    printf("disk ");
	else if (kd.known[i].class == CLASS_NETWORK)
	    printf("net  ");
    	if (kd.known[i].model)
	    printf(" %s\n", kd.known[i].model);
	else
	    printf("\n");
    }
#endif

    /* Just in case */
    /* only use RHupdates if we're NFS, otherwise we'll use files on */
    /* the first ISO image and we won't be able to umount it */
    useRHupdates = 0;
    if (!strncmp(url, "nfs:", 4)) {
	logMessage("NFS install method detected, will use RHupdates/");
	useRHupdates = 1;
    }
	
    if (useRHupdates)
	setenv("PYTHONPATH", "/tmp/updates:/mnt/source/RHupdates", 1);
    else
	setenv("PYTHONPATH", "/tmp/updates", 1);

    argptr = anacondaArgs;

    if (!access("/tmp/updates/anaconda", X_OK))
	*argptr++ = "/tmp/updates/anaconda";
    else if (useRHupdates && !access("/mnt/source/RHupdates/anaconda", X_OK))
	*argptr++ = "/mnt/source/RHupdates/anaconda";
    else
	*argptr++ = "/usr/bin/anaconda";

    logMessage("Running anaconda script %s", *(argptr-1));

    *argptr++ = "-m";
    if (strncmp(url, "ftp:", 4)) {
	*argptr++ = url;
    } else {
	int fd;

	fd = open("/tmp/method", O_CREAT | O_TRUNC | O_RDWR, 0600);
	write(fd, url, strlen(url));
	write(fd, "\r", 1);
	close(fd);
	*argptr++ = "@/tmp/method";
    }

    /* add extra args - this potentially munges extraArgs */
    tmparg = extraArgs;
    while (*tmparg) {
	char *idx;

	logMessage("adding extraArg %s", *tmparg);
	idx = strchr(*tmparg, '=');
	if (idx &&  ((idx-*tmparg) < strlen(*tmparg))) {
	    *idx = '\0';
	    *argptr++ = *tmparg;
	    *argptr++ = idx+1;
	} else {
	    *argptr++ = *tmparg;
	}

	tmparg++;
    }

    if (FL_RESCUE(flags)) {
	startNewt(flags);

	if (!lang) {
	    int rc;

	    do {
		chooseLanguage(&lang, flags);
		defaultLang = 0;
		rc = chooseKeyboard (&keymap, &kbdtype, flags);
	    } while ((rc) && (rc != LOADER_NOOP));
	}
	*argptr++ = "--rescue";
    } else {
	if (FL_SERIAL(flags))
	    *argptr++ = "--serial";
	if (FL_MCHECK(flags))
	    setenv("MALLOC_CHECK_", "2", 1);
	if (FL_TEXT(flags))
	    *argptr++ = "-T";
	if (FL_EXPERT(flags))
	    *argptr++ = "--expert";

	if (FL_KICKSTART(flags)) {
	    *argptr++ = "--kickstart";
	    *argptr++ = ksFile;
	}

	if (!lang)
	    lang = getenv ("LC_ALL");
	
	if (lang && !defaultLang && !FL_NOPASS(flags)) {
	    *argptr++ = "--lang";
	    *argptr++ = lang;
	}
	
	if (keymap && !FL_NOPASS(flags)) {
	    *argptr++ = "--keymap";
	    *argptr++ = keymap;
	}

	if (kbdtype && !FL_NOPASS(flags)) {
	    *argptr++ = "--kbdtype";
	    *argptr++ = kbdtype;
	}

	if (instClass) {
	    *argptr++ = "--class";
	    *argptr++ = instClass;
	}

	if (memoryOverhead) {
	    *argptr++ = "--overhead";
	    *argptr = malloc(20);
	    sprintf(*argptr, "%d", memoryOverhead);
	    argptr++;
	}

	for (i = 0; i < modLoaded->numModules; i++) {
	    if (!modLoaded->mods[i].path) continue;

	    mi = isysFindModuleInfo(modInfo, modLoaded->mods[i].name);
	    if (!mi) continue;
	    if (mi->major == DRIVER_NET)
		where = "net";
	    else if (mi->major == DRIVER_SCSI)
		where = "scsi";
	    else
		continue;

	    *argptr++ = "--module";
	    *argptr = alloca(80);
	    sprintf(*argptr, "%s:%s:%s", modLoaded->mods[i].path, where,
		    modLoaded->mods[i].name);

	    argptr++;
	}
    }
    
    *argptr = NULL;

    stopNewt();
    closeLog();

    if (!FL_TESTING(flags)) {
	char *buf = sdupprintf(_("Running anaconda, the %s system installer - please wait...\n"), PRODUCTNAME);
	printf("%s", buf);
    	execv(anacondaArgs[0], anacondaArgs);
        perror("exec");
    }

    return 1;
}
