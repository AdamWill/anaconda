#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "cpio.h"
#include "isys.h"
#include "stubs.h"

/* hack */
int combined_insmod_main(int argc, char ** argv);

int ourInsmodCommand(int argc, char ** argv) {
    char * file;
    char finalName[100];
    char * chptr;
    gzFile fd;
    int rc, rmObj = 0;
    int sparc64 = 0, i;
    int force = 0;
    char * ballPath = NULL;
    char fullName[100];
    FILE *fptr;
    struct utsname u;

    uname(&u);

#ifdef __sparc__
    if (!strcmp(u.machine, "sparc64"))
       sparc64 = 1;
#endif

    if (argc < 2) {
	fprintf(stderr, "usage: insmod [-p <path>] <module>.o [params]\n");
	return 1;
    }

    /* hidden parameter for S390 installer usage */
    if (!strcmp(argv[1], "-f")) {
       force = 1;
    }

    /* test if module is available in /lib */
    fptr = fopen(argv[1+force], "r");
    if (fptr) {
       fclose(fptr);
       argv[0] = "insmod";
        return combined_insmod_main(argc, argv);
    }

    if(force) {
       argc--;
       argv++;
    }

    if (!strcmp(argv[1], "-p")) {
	ballPath = malloc(strlen(argv[2]) + 30);
	sprintf(ballPath, "%s/%s", argv[2], sparc64 ?
		    "modules64.cgz" : "modules.cgz");
	argv += 2;
	argc -= 2;
    } else {
	ballPath = strdup(sparc64 ?
			  "/modules/modules64.cgz" :
			  "/modules/modules.cgz");

    }

    file = argv[1];

    if (access(file, R_OK)) {
	/* Try two balls on sparc64, one elsewhere */
	for (i = 0; ; i++) {
	    /* it might be having a ball */
	    fd = gunzip_open(ballPath);
	    if (!fd) {
		fprintf(stderr, "cannot unzip %s\n", ballPath);
		free(ballPath);
		return 1;
	    }

	    chptr = strrchr(file, '/');
	    if (chptr) file = chptr + 1;
	    sprintf(finalName, "/tmp/%s", file);

	    sprintf(fullName, "%s/%s", u.release, file);

	    if (installCpioFile(fd, fullName, finalName, 0)) {
		if (i < sparc64) {
		    ballPath[strlen(ballPath)-5] = '5';
		    continue;
		}
		free(ballPath);
		return 1;
	    }

	    rmObj = 1;
	    file = finalName;
	    break;
	}
    }

    free(ballPath);

    argv[0] = "insmod";
    argv[1] = file;

    rc = combined_insmod_main(argc, argv);
    
    if (rmObj) unlink(file);

    return rc;
}

int rmmod(char * modName) {
    pid_t child;
    int status;
    char * argv[] = { "/bin/rmmod", modName, NULL };
    int argc = 2;
    int rc = 0;

    if ((child = fork()) == 0) {
	exit(combined_insmod_main(argc, argv));
    }

    waitpid(child, &status, 0);

    if (WIFEXITED(status))
       rc = WEXITSTATUS(status);
    else
       rc = -1;

    return rc;
}

int insmod(char * modName, char * path, char ** args) {
    int argc;
    char ** argv;
    int rc = 0;
    pid_t child;
    int status;
    int count;

    argc = 0;
    for (argv = args; argv && *argv; argv++, argc++);

    argv = alloca(sizeof(*argv) * (argc + 5));
    argv[0] = "/bin/insmod";
    count = 1;
    if (path) {
	argv[1] = "-p";
	argv[2] = path;
	count += 2;
    }

    argv[count] = modName;
    count++;

    if (args)
	memcpy(argv + count, args, sizeof(*args) * argc);

    argv[argc + count] = NULL;

    argc += count;

    if ((child = fork()) == 0) {
#if defined(__s390__) || defined(__s390x__)
	execv("/bin/loaderbin", argv);
#else
	execv("/bin/loader", argv);
#endif
	exit(1);
    }

    waitpid(child, &status, 0);

    if (WIFEXITED(status))
       rc = WEXITSTATUS(status);
    else
       rc = -1;

    return rc;
}
