#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/hdreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "isys.h"
#include "probe.h"

static int dac960GetDevices(struct knownDevices * devices);
static int CompaqSmartArrayGetDevices(struct knownDevices * devices);
static int CompaqSmartArray5300GetDevices(struct knownDevices * devices);
/* Added support for I2O Block devices: Boji Kannanthanam 
	<boji.t.Kannanthanam@intel.com> */
static int ProcPartitionsGetDevices(struct knownDevices * devices);

static int readFD (int fd, char **buf)
{
    char *p;
    size_t size = 4096;
    int s, filesize;

    *buf = malloc (size);
    if (*buf == 0)
      return -1;

    filesize = 0;
    do {
	p = &(*buf) [filesize];
	s = read (fd, p, 4096);
	if (s < 0)
	    break;
	filesize += s;
	if (s != 4096)
	    break;
	size += 4096;
	*buf = realloc (*buf, size);
    } while (1);

    if (filesize == 0 && s < 0) {
	free (*buf);     
	*buf = NULL;
	return -1;
    }

    return filesize;
}

static int sortDevices(const void * a, const void * b) {
    const struct kddevice * one = a;
    const struct kddevice * two = b;

    return strcmp(one->name, two->name);
}

static int deviceKnown(struct knownDevices * devices, char * dev) {
    int i;

    for (i = 0; i < devices->numKnown; i++)
    	if (!strcmp(devices->known[i].name, dev)) return 1;

    return 0;
}

static void addDevice(struct knownDevices * devices, struct kddevice dev) {
    if (devices->numKnown == devices->numKnownAlloced) {
    	devices->numKnownAlloced += 5;
    	devices->known = realloc(devices->known, 
		sizeof(*devices->known) * devices->numKnownAlloced);
    }

    devices->known[devices->numKnown++] = dev;
}

void kdAddDevice(struct knownDevices * devices, enum deviceClass devClass, 
		 char * devName, char * devModel) {
    struct kddevice new;

    new.class = devClass;
    new.name = devName;
    new.model = devModel;

    addDevice(devices, new);
}

void kdFree(struct knownDevices * devices) {
    if (devices->known) free(devices->known);
    devices->known = NULL;
    devices->numKnown = devices->numKnownAlloced = 0;
}

int kdFindNetList(struct knownDevices * devices, int code) {
    int fd;
    char *buf;
    char * start, * end;
    struct kddevice newDevice;
    int s;

    if ((fd = open("/proc/net/dev", O_RDONLY)) < 0) {
	fprintf(stderr, "failed to open /proc/net/dev!\n");
	return 1;
    }

    s = readFD(fd, &buf);
    close(fd);
    if (s < 0) {
	fprintf(stderr, "error reading /proc/net/dev!\n");
	return 1;
    }

    buf[s] = '\0';

    /* skip the first two lines */
    start = strchr(buf, '\n');
    if (!start) goto bye;
    start = strchr(start + 1, '\n');
    if (!start) goto bye;

    start++;
    while (start && *start) {
	while (isspace(*start)) start++;
	end = strchr(start, ':');
	if (!end) goto bye;
	*end = '\0';
	
    	if (strcmp(start, "lo")) {
	    if (deviceKnown(devices, start)) continue;

	    newDevice.name = strdup(start);
	    newDevice.model = NULL;
	    newDevice.class = CLASS_NETWORK;
	    newDevice.code = code;
	    addDevice(devices, newDevice);
	}

	start = strchr(end + 1, '\n');
	if (start) start++;
    }

    qsort(devices->known, devices->numKnown, sizeof(*devices->known),
	  sortDevices);

bye:
    free (buf);
    return 0;
}

char * getDasdPorts () {
    char *line, *ports = NULL;
    char port[6];
    FILE *fd;
    fd = fopen ("/proc/dasd/devices", "r");
    if (!fd) {
       return;
    }
    line = (char *) malloc (100 * sizeof (char));
    while (fgets (line, 100, fd) != NULL) {
        if ((strstr (line, "unknown") != NULL)) {
	        continue;
	    }
        if (sscanf (line, "%[A-Za-z0-9](%*s", port)) {
	        if (!ports) {
	            ports = (char *) malloc (strlen ("dasd=") + strlen (port) + 1);
	            strcpy (ports, port);
	        } else {
	            ports = (char *) realloc (ports, strlen (ports) + strlen (port) + 2);	/* portnumber + ',' */
	            strcat (ports, ",");
	            strcat (ports, port);
	        }
	    }
    }
    if (fd)
        fclose (fd);
    return ports;
}

int kdFindDasdList(struct knownDevices * devices, int code) {
    char format[10], devname[10];
    char *line;
    int ret;
    FILE *fd;

    struct kddevice device;

    fd = fopen ("/proc/dasd/devices", "r");
    if(!fd) {
        return 0;
    }

    line = (char *)malloc(100*sizeof(char));
    while (fgets (line, 100, fd) != NULL) {
        if ((strstr(line, "active") == NULL)) {
	        continue;
        }
        ret = sscanf (line, "%*[A-Za-z0-9](%[A-Z]) at ( %*d: %*d) is %s : %*s",
		              format, devname);
        if (ret == 2) {
	        if(!deviceKnown(devices, devname)) {
		        device.code = code;
		        device.class = CLASS_HD;
		        device.name = strdup(devname);
		        device.model = strdup(format);
		        addDevice(devices, device);
            }
        } else
	          fprintf(stderr, "Error parsing /proc/dasd/devices\n%s\n", line);
    }
    if (fd) fclose(fd);
    qsort(devices->known, devices->numKnown, sizeof(*devices->known),
          sortDevices);
    return 0;
}

int kdFindIdeList(struct knownDevices * devices, int code) {
    return kdFindFilteredIdeList(devices, code, NULL);
}

int kdFindFilteredIdeList(struct knownDevices * devices, int code, 
			  kdFilterType filter) {
    DIR * dir;
    char path[80];
    int fd, i;
    struct dirent * ent;
    struct kddevice device;

    if (access("/proc/ide", R_OK)) return 0;

    if (!(dir = opendir("/proc/ide"))) {
	return 1;
    }

    /* set errno to 0, so we can tell when readdir() fails */
    errno = 0;
    while ((ent = readdir(dir))) {
    	if (!deviceKnown(devices, ent->d_name)) {
	    sprintf(path, "/proc/ide/%s/media", ent->d_name);
	    if ((fd = open(path, O_RDONLY)) >= 0) {
		i = read(fd, path, 50);
		close(fd);
		path[i - 1] = '\0';		/* chop off trailing \n */

		device.code = code;

		device.class = CLASS_UNSPEC;
		if (!strcmp(path, "cdrom")) 
		    device.class = CLASS_CDROM;
		else if (!strcmp(path, "disk"))
		    device.class = CLASS_HD;
		else if (!strcmp(path, "floppy"))
		    device.class = CLASS_FLOPPY;

		if (device.class != CLASS_UNSPEC) {
		    device.name = strdup(ent->d_name);

		    sprintf(path, "/proc/ide/%s/model", ent->d_name);
		    if ((fd = open(path, O_RDONLY)) >= 0) {
			i = read(fd, path, 50);
			close(fd);
			path[i - 1] = '\0';	/* chop off trailing \n */
			device.model = strdup(path);
		    }

		    if (filter && !filter(&device)) {
			free(device.model);
			free(device.name);
		    } else {
			addDevice(devices, device);
		    }
		}
	    }
	}

        errno = 0;          
    }

    closedir(dir);

    qsort(devices->known, devices->numKnown, sizeof(*devices->known),
	  sortDevices);

    return 0;
}

#define SCSISCSI_TOP	0
#define SCSISCSI_HOST 	1
#define SCSISCSI_VENDOR 2
#define SCSISCSI_TYPE 	3

int kdFindScsiList(struct knownDevices * devices, int code) {
    int fd;
    char *buf;
    char linebuf[80];
    char typebuf[10];
    int i, state = SCSISCSI_TOP;
    char * start, * chptr, * next, *end;
    int driveNum = 0;
    char cdromNum = '0';
    char tapeNum = '0';
    struct kddevice device;
    int val = 0;

    ProcPartitionsGetDevices(devices);
    
    if (access("/proc/scsi/scsi", R_OK)) {
	dac960GetDevices(devices);
	CompaqSmartArrayGetDevices(devices);
	CompaqSmartArray5300GetDevices(devices);
	return 0;
    }

    fd = open("/proc/scsi/scsi", O_RDONLY);
    if (fd < 0) return 1;
    
    i = readFD(fd, &buf);
    if (i < 1) {
        close(fd);
	return 1;
    }
    close(fd);
    buf[i] = '\0';

    if (!strncmp(buf, "Attached devices: none", 22)) {
	dac960GetDevices(devices);
	CompaqSmartArrayGetDevices(devices);
	CompaqSmartArray5300GetDevices(devices);
	goto bye;
    }

    start = buf;
    while (*start) {
	chptr = start;
 	while (*chptr != '\n') chptr++;
	*chptr = '\0';
	next = chptr + 1;

	switch (state) {
	  case SCSISCSI_TOP:
	    if (strcmp("Attached devices: ", start)) {
		val = -1;
		goto bye;
	    }
	    state = SCSISCSI_HOST;
	    break;

	  case SCSISCSI_HOST:
	    if (strncmp("Host: ", start, 6)) {
		val = -1;
		goto bye;
	    }

	    start = strstr(start, "Id: ");
	    if (!start) {
		val = -1;
		goto bye;
	    }
	    start += 4;

	    /*id = strtol(start, NULL, 10);*/

	    state = SCSISCSI_VENDOR;
	    break;

	  case SCSISCSI_VENDOR:
	    if (strncmp("  Vendor: ", start, 10)) {
		val = -1;
		goto bye;
	    }

	    start += 10;
	    end = chptr = strstr(start, "Model:");
	    if (!chptr) {
		val = -1;
		goto bye;
	    }

	    chptr--;
	    while (*chptr == ' ' && *chptr != ':' ) chptr--;
	    if (*chptr == ':') {
		    chptr++;
		    *(chptr + 1) = '\0';
		    strcpy(linebuf,"Unknown");
	    } else {
		    *(chptr + 1) = '\0';
		    strcpy(linebuf, start);
	    }
	    *linebuf = toupper(*linebuf);
	    chptr = linebuf + 1;
	    while (*chptr) {
		*chptr = tolower(*chptr);
		chptr++;
	    }

	    start = end;  /* beginning of "Model:" */
	    start += 7;
		
	    chptr = strstr(start, "Rev:");
	    if (!chptr) {
		val = -1;
		goto bye;
	    }
	   
	    chptr--;
	    while (*chptr == ' ') chptr--;
	    *(chptr + 1) = '\0';

	    strcat(linebuf, " ");
	    strcat(linebuf, start);

	    state = SCSISCSI_TYPE;

	    break;

	  case SCSISCSI_TYPE:
	    if (strncmp("  Type:", start, 7)) {
		val = -1;
		goto bye;
	    }
	    *typebuf = '\0';
	    if (strstr(start, "Direct-Access")) {
		if (driveNum < 26)
		    sprintf(typebuf, "sd%c", driveNum + 'a');
		else
		    sprintf(typebuf, "sd%c%c", (driveNum / 26 - 1) + 'a',
			    (driveNum % 26) + 'a');
		driveNum++;
		device.class = CLASS_HD;
	    } else if (strstr(start, "Sequential-Access")) {
		sprintf(typebuf, "st%c", tapeNum++);
		device.class = CLASS_TAPE;
	    } else if (strstr(start, "CD-ROM")) {
		sprintf(typebuf, "scd%c", cdromNum++);
		device.class = CLASS_CDROM;
	    }

	    if (*typebuf && !deviceKnown(devices, typebuf)) {
		device.name = strdup(typebuf);
		device.model = strdup(linebuf);
		device.code = code;

		/* Do we need this for anything?
		sdi[numMatches].bus = 0;
		sdi[numMatches].id = id;
		*/

		addDevice(devices, device);
	    }

	    state = SCSISCSI_HOST;
	}

	start = next;
    }

    dac960GetDevices(devices);
    CompaqSmartArrayGetDevices(devices);
    CompaqSmartArray5300GetDevices(devices);

    qsort(devices->known, devices->numKnown, sizeof(*devices->known),
	  sortDevices);

bye:
    free (buf);
    return val;
}

struct knownDevices kdInit(void) {
    struct knownDevices kd;

    memset(&kd, 0, sizeof(kd));

    return kd;
}

static int dac960GetDevices(struct knownDevices * devices) {
    struct kddevice newDevice;
    char ctl[50];
    int ctlNum = 0;
    char *buf = NULL;
    int fd;
    int i;
    char * start, * chptr;

    sprintf(ctl, "/proc/rd/c%d/current_status", ctlNum++);

    while ((fd = open(ctl, O_RDONLY)) >= 0) {
    	free (buf);
	i = readFD(fd, &buf);
	buf[i] = '\0';
	start = buf;
	while (start && (start = strstr(start, "/dev/rd/"))) {
	    start += 5;
	    chptr = strchr(start, ':');
	    if (!chptr)
		continue;

	    *chptr = '\0';
	    if (!deviceKnown(devices, start)) {
		newDevice.name = strdup(start);

		start = chptr + 2;
		chptr = strchr(start, '\n');
		*chptr = '\0';

		newDevice.model = strdup(start);
		newDevice.class = CLASS_HD;
		addDevice(devices, newDevice);

		*chptr = '\n';
	    } else {
		*chptr = '\0';
	    }

	    start = strchr(chptr, '\n');
	    if (start) start++;
	}

	sprintf(ctl, "/proc/rd/c%d/current_status", ctlNum++);
    }

    free (buf);
    return 0;
}

static int CompaqSmartArrayGetDevices(struct knownDevices * devices) {
    struct kddevice newDevice;
    FILE *f;
    char buf[256];
    char *ptr;
    int ctlNum = 0;
    char ctl[64];
    char *path;
	
    path = "/proc/driver/cpqarray";

    sprintf(ctl, "%s/ida%d", path, ctlNum++);
    f = fopen(ctl, "r");

    if (!f) {
	    path = "/proc/driver/array";
	    sprintf(ctl, "%s/ida%d", path, ctlNum++);
	    f = fopen(ctl, "r");
    }

    if (!f) {
	    path = "/proc/ida";
	    sprintf(ctl, "%s/ida%d", path, ctlNum++);
	    f = fopen(ctl, "r");
    }

    while (f) {
	while (fgets(buf, sizeof(buf) - 1, f)) {
	    if (!strncmp(buf, "ida/", 4)) {
		ptr = strchr(buf, ':');
		*ptr = '\0';

		if (!deviceKnown(devices, buf)) {
		    newDevice.name = strdup(buf);
		    newDevice.model = strdup("Compaq RAID logical disk");
		    newDevice.class = CLASS_HD;
		    addDevice(devices, newDevice);
		}
	    }
	}
	sprintf(ctl, "%s/ida%d", path, ctlNum++);
        fclose(f);
	f = fopen(ctl, "r");
    }

    
    return 0;
}

static int ProcPartitionsGetDevices(struct knownDevices * devices) {
    struct kddevice newDevice;
    int fd, i;
    char *buf;
    char * start, *chptr, *next, *end, *model;
    char ctl[40];

    /* Read from /proc/partitions */
    fd = open("/proc/partitions", O_RDONLY);
    if (fd < 0) 
	{
	    fprintf(stderr, "failed to open /proc/partitions!\n");
	    return 1;
	}

    i = readFD(fd, &buf);
    if (i < 1) {
        close(fd);
        free (buf);
	fprintf(stderr, "error reading /proc/partitions!\n");
        return 1;
    }
    close(fd);
    buf[i] = '\0';

    /* skip the first two lines */
    start = strchr(buf, '\n');
    if (!start) goto bye;

    start = strchr(start + 1, '\n');
    if (!start) goto bye;

    start++;
    end = start + strlen(start);
    while (*start && start < end) {
	/* parse till end of line and store the start of next line. */
	chptr = start;
	while (*chptr != '\n') chptr++;
	*chptr = '\0';
	next = chptr + 1;

	/* get rid of anything which is not alpha */
	while (!(isalpha(*start))) start++;

	model = NULL;
	    
	if (!strncmp("i2o/", start, 4))
	   model = "I2O Block Device";
	else if (!strncmp("ataraid/", start, 8))
	    model = "ATARAID Block Device";

	if (model) {
	    i = 0;
	    while(!(isspace(*start))) {
		ctl[i] = *start;
		i++;
		start++;
	    }
	    ctl[i] = '\0';
	    if (i < 1) { 
		free (buf);
		return 1; 
	    }
	    /* We don't want partitions just the disks ! */
	    if (!isdigit(ctl[i-1])){
		if (!deviceKnown(devices, ctl)) {
		    newDevice.name = strdup(ctl);
		    newDevice.model = strdup(model);
		    newDevice.class = CLASS_HD;
		    addDevice(devices, newDevice);
		}
	    }
	} /* end of if it is an /proc/partition device */
	start = next;
	end = start + strlen(start);
    } /* end of while */
 bye:
    free (buf);
    return 0;
}


static int CompaqSmartArray5300GetDevices(struct knownDevices * devices) {
    struct kddevice newDevice;
    FILE *f;
    char buf[256];
    char *ptr;
    int ctlNum = 0;
    char ctl[64];
    char *path;
	
    path = "/proc/driver/cciss";

    sprintf(ctl, "%s/cciss%d", path, ctlNum++);
		
    f = fopen(ctl, "r");
    if (!f) {
	    path = "/proc/cciss";
	    sprintf(ctl, "%s/cciss%d", path, ctlNum++);
	    f = fopen(ctl, "r");
    }

    while (f) {
	while (fgets(buf, sizeof(buf) - 1, f)) {
	    if (!strncmp(buf, "cciss/", 6)) {
		ptr = strchr(buf, ':');
		*ptr = '\0';

		if (!deviceKnown(devices, buf)) {
		    newDevice.name = strdup(buf);
		    newDevice.model = strdup("Compaq RAID logical disk");
		    newDevice.class = CLASS_HD;
		    addDevice(devices, newDevice);
		}
	    }
	}
	sprintf(ctl, "%s/cciss%d", path, ctlNum++);
	fclose(f);
	f = fopen (ctl, "r");
    }

    
    return 0;
}
