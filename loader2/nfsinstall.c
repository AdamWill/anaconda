/*
 * nfsinstall.c - code to set up nfs installs
 *
 * Erik Troan <ewt@redhat.com>
 * Matt Wilson <msw@redhat.com>
 * Michael Fulbright <msf@redhat.com>
 * Jeremy Katz <katzj@redhat.com>
 *
 * Copyright 1997 - 2002 Red Hat, Inc.
 *
 * This software may be freely redistributed under the terms of the GNU
 * General Public License.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <fcntl.h>
#include <newt.h>
#include <popt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loader.h"
#include "lang.h"
#include "loadermisc.h"
#include "log.h"
#include "method.h"
#include "nfsinstall.h"
#include "net.h"

#include "../isys/imount.h"

int nfsGetSetup(char ** hostptr, char ** dirptr) {
    struct newtWinEntry entries[3];
    char * buf;
    char * newServer = *hostptr ? strdup(*hostptr) : NULL;
    char * newDir = *dirptr ? strdup(*dirptr) : NULL;
    int rc;

    entries[0].text = _("NFS server name:");
    entries[0].value = &newServer;
    entries[0].flags = NEWT_FLAG_SCROLL;
    entries[1].text = _("Red Hat directory:");
    entries[1].value = &newDir;
    entries[1].flags = NEWT_FLAG_SCROLL;
    entries[2].text = NULL;
    entries[2].value = NULL;
    buf = sdupprintf(_(netServerPrompt), "NFS", PRODUCTNAME);
    rc = newtWinEntries(_("NFS Setup"), buf, 60, 5, 15,
                        24, entries, _("OK"), _("Back"), NULL);
    free(buf);

    if (rc == 2) {
        if (newServer) free(newServer);
        if (newDir) free(newDir);
        return LOADER_BACK;
    }

    if (*hostptr) free(*hostptr);
    if (*dirptr) free(*dirptr);
    *hostptr = newServer;
    *dirptr = newDir;

    return 0;
}



char * mountNfsImage(struct installMethod * method,
                     char * location, struct knownDevices * kd,
                     struct loaderData_s * loaderData,
                     moduleInfoSet modInfo, moduleList modLoaded,
                     moduleDeps * modDepsPtr, int flags) {
    static struct networkDeviceConfig netDev;
    char * devName = NULL;
    char * host = NULL;
    char * directory = NULL;
    char * fullPath = NULL;
    char * path;
    char * url = NULL;

    enum { NFS_STAGE_IFACE, NFS_STAGE_IP, NFS_STAGE_NFS, 
           NFS_STAGE_MOUNT, NFS_STAGE_DONE } stage = NFS_STAGE_IFACE;

    int rc;
    int dir = 1;

    initLoopback();

    memset(&netDev, 0, sizeof(netDev));
    netDev.isDynamic = 1;

    /* populate netDev based on any kickstart data */
    setupNetworkDeviceConfig(&netDev, loaderData, flags);

    /* JKFIXME: ASSERT -- we have a network device when we get here */
    while (stage != NFS_STAGE_DONE) {
        switch (stage) {
        case NFS_STAGE_IFACE:
            logMessage("going to pick interface");
            rc = chooseNetworkInterface(kd, loaderData, flags);
            if ((rc == LOADER_BACK) || (rc == LOADER_ERROR) ||
                ((dir == -1) && (rc == LOADER_NOOP))) return NULL;

            stage = NFS_STAGE_IP;
            dir = 1;
            logMessage("using interface %s", loaderData->netDev);
            devName = loaderData->netDev;
            break;
            
        case NFS_STAGE_IP:
            logMessage("going to do getNetConfig");
            rc = readNetConfig(devName, &netDev, flags);
            if (rc) {
                stage = NFS_STAGE_IFACE;
                dir = -1;
                break;
            }
            stage = NFS_STAGE_NFS;
            break;
            
        case NFS_STAGE_NFS:
            logMessage("going to do nfsGetSetup");
            if (loaderData->method &&
                !strncmp(loaderData->method, "nfs", 3) &&
                loaderData->methodData) {
                host = ((struct nfsInstallData *)loaderData->methodData)->host;
                directory = ((struct nfsInstallData *)loaderData->methodData)->directory;

                logMessage("host is %s, dir is %s", host, directory);

                if (!host || !directory) {
                    logMessage("missing host or directory specification");
                    free(loaderData->method);
                    loaderData->method = NULL;
                    break;
                }
            } else if (nfsGetSetup(&host, &directory) == LOADER_BACK) {
                stage = NFS_STAGE_IP;
                dir = -1;
                break;
            }
             
            stage = NFS_STAGE_MOUNT;
            dir = 1;
            break;

        case NFS_STAGE_MOUNT: {
            int foundinvalid = 0;
            char * buf;

            fullPath = alloca(strlen(host) + strlen(directory) + 2);
            sprintf(fullPath, "%s:%s", host, directory);

            logMessage("mounting nfs path %s", fullPath);

            if (FL_TESTING(flags)) {
                stage = NFS_STAGE_DONE;
                dir = 1;
                break;
            }

            stage = NFS_STAGE_NFS;

            if (!doPwMount(fullPath, "/mnt/source", "nfs", 1, 0, NULL, NULL)) {
                logMessage("mounted %s on /mnt/source", fullPath);
                if (!access("/mnt/source/RedHat/base/stage2.img", R_OK)) {
                    logMessage("can access stage2.img");
                    rc = mountStage2("/mnt/source/RedHat/base/stage2.img");
                    if (rc) {
                        umount("/mnt/source");
                        if (rc == -1) { 
                            foundinvalid = 1; 
                            logMessage("not the right one"); 
                        }
                    } else {
                        stage = NFS_STAGE_DONE;
                        url = "nfs://mnt/source/.";
                        break;
                    }
                }
                if ((path = validIsoImages("/mnt/source"))) {
		    logMessage("Path to valid iso is %s", path);
                    copyUpdatesImg("/mnt/source/updates.img");

                    if (mountLoopback(path, "/mnt/source2", "loop1")) 
                        logMessage("failed to mount iso %s loopback", path);
                    else {
                        rc = mountStage2("/mnt/source2/RedHat/base/stage2.img");
                        if (rc) {
                            umountLoopback("/mnt/source2", "loop1");
                            if (rc == -1)
				foundinvalid = 1;
                        } else {
                            queryIsoMediaCheck(path, flags);

                            stage = NFS_STAGE_DONE;
                            url = "nfsiso:/mnt/source";
                            break;
                        }
                    }
                }

                if (foundinvalid) 
                    buf = sdupprintf(_("The %s installation tree in that "
                                       "directory does not seem to match "
                                       "your boot media."), PRODUCTNAME);
                else
                    buf = sdupprintf(_("That directory does not seem to "
                                       "contain a %s installation tree."),
                                     PRODUCTNAME);
                newtWinMessage(_("Error"), _("OK"), buf);
                break;
            } else {
                newtWinMessage(_("Error"), _("OK"),
                               _("That directory could not be mounted from "
                                 "the server."));
                if (loaderData->method) {
                    free(loaderData->method);
                    loaderData->method = NULL;
                }
                break;
            }
        }

        case NFS_STAGE_DONE:
            break;
        }
    }

#if !defined (__s390__) && !defined (__s390x__)
    writeNetInfo("/tmp/netinfo", &netDev, kd);
#endif
    free(host);
    free(directory);

    return url;
}


void setKickstartNfs(struct loaderData_s * loaderData, int argc,
                     char ** argv, int * flagsPtr) {
    char * host, * dir;
    poptContext optCon;
    int rc;
    struct poptOption ksNfsOptions[] = {
        { "server", '\0', POPT_ARG_STRING, &host, 0 },
        { "dir", '\0', POPT_ARG_STRING, &dir, 0 },
        { 0, 0, 0, 0, 0 }
    };

    logMessage("kickstartFromNfs");
    optCon = poptGetContext(NULL, argc, (const char **) argv, ksNfsOptions, 0);
    if ((rc = poptGetNextOpt(optCon)) < -1) {
        newtWinMessage(_("Kickstart Error"), _("OK"),
                       _("Bad argument to NFS kickstart method "
                         "command %s: %s"),
                       poptBadOption(optCon, POPT_BADOPTION_NOALIAS), 
                       poptStrerror(rc));
        return;
    }

    loaderData->method = strdup("nfs");
    loaderData->methodData = calloc(sizeof(struct nfsInstallData *), 1);
    if (host)
        ((struct nfsInstallData *)loaderData->methodData)->host = host;
    if (dir)
        ((struct nfsInstallData *)loaderData->methodData)->directory = dir;

    logMessage("results of nfs, host is %s, dir is %s", host, dir);
}


int kickstartFromNfs(char * url, struct knownDevices * kd,
                     struct loaderData_s * loaderData, int flags) {
    char * host = NULL, * file = NULL;
    int failed = 0;

    if (kickstartNetworkUp(kd, loaderData, flags)) {
        logMessage("unable to bring up network");
        return 1;
    }

    host = url;
    /* JKFIXME: need to add this; abstract out the functionality
     * so that we can use it for all types */
    if (host[strlen(url) - 1] == '/') {
        logMessage("directory syntax not supported yet");
        return 1;
        /* host[strlen(host) - 1] = '\0';
           file = malloc(30);
           sprintf(file, "%s-kickstart", inet_ntoa(netDev.dev.ip));*/
    } else {
        file = strrchr(host, '/');
        if (!file) {
            file = "/";
        } else {
            *file++ = '\0';
        }
    }

    logMessage("ks location: nfs://%s/%s", host, file);

    if (!doPwMount(host, "/tmp/ks", "nfs", 1, 0, NULL, NULL)) {
        char * buf;

        buf = alloca(strlen(file) + 10);
        sprintf(buf, "/tmp/ks/%s", file);
        if (copyFile(buf, "/tmp/ks.cfg")) {
            logMessage("failed to copy ks.cfg to /tmp/ks.cfg");
            failed = 1;
        }
        
    } else {
        logMessage("failed to mount nfs source");
        failed = 1;
    }

    umount("/tmp/ks");
    unlink("/tmp/ks");

    return failed;
}
