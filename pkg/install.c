#include <sys/types.h>

#include <err.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <libutil.h>

#include <pkg.h>

#include "utils.h"
#include "install.h"

void
usage_install(void)
{
	fprintf(stderr, "usage: pkg install [-r reponame] [-ygxX] <pkg-name> <...>\n\n");
	fprintf(stderr, "For more information see 'pkg help install'.\n");
}

int
exec_install(int argc, char **argv)
{
	struct pkg *pkg = NULL;
	struct pkgdb_it *it = NULL;
	struct pkgdb *db = NULL;
	struct pkg_jobs *jobs = NULL;
	const char *reponame = NULL;
	int retcode = 1;
	int ch, yes = 0;
	int64_t dlsize = 0;
	int64_t oldsize = 0, newsize = 0;
	char size[7];
	match_t match = MATCH_EXACT;
	const char *assume_yes = NULL;

	while ((ch = getopt(argc, argv, "ygxXr:")) != -1) {
		switch (ch) {
			case 'y':
				yes = 1;
				break;
			case 'g':
				match = MATCH_GLOB;
				break;
			case 'x':
				match = MATCH_REGEX;
				break;
			case 'X':
				match = MATCH_EREGEX;
				break;
			case 'r':
				reponame = optarg;
				break;
			default:
				usage_install();
				return (EX_USAGE);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage_install();
		return (EX_USAGE);
	}

	if (geteuid() != 0) {
		warnx("installing packages can only be done as root");
		return (EX_NOPERM);
	}

	if (pkgdb_open(&db, PKGDB_REMOTE) != EPKG_OK) {
		return (EX_IOERR);
	}

	if (pkg_jobs_new(&jobs, PKG_JOBS_INSTALL, db) != EPKG_OK) {
		goto cleanup;
	}

	if ((it = pkgdb_query_installs(db, match, argc, argv, reponame)) == NULL)
		goto cleanup;

	while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC|PKG_LOAD_DEPS) == EPKG_OK) {
		pkg_jobs_add(jobs, pkg);
		pkg = NULL;
	}

	if (pkg_jobs_is_empty(jobs)) {
		printf("Nothing to do\n");
		retcode = 0;
		goto cleanup;
	}

	/* print a summary before applying the jobs */
	pkg = NULL;
	printf("The following packages will be installed:\n");

	while (pkg_jobs(jobs, &pkg) == EPKG_OK) {
		dlsize += pkg_new_pkgsize(pkg);
		if (pkg_get(pkg, PKG_NEWVERSION) != NULL) {
			printf("\tUpgrading %s: %s -> %s\n", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION), pkg_get(pkg,PKG_NEWVERSION));
			oldsize += pkg_flatsize(pkg);
			newsize += pkg_new_flatsize(pkg);
		} else {
			newsize += pkg_flatsize(pkg);
			printf("\tInstalling %s: %s\n", pkg_get(pkg, PKG_NAME), pkg_get(pkg, PKG_VERSION));
		}
	}

	if (oldsize > newsize) {
		newsize *= -1;
		humanize_number(size, sizeof(size), oldsize - newsize, "B", HN_AUTOSCALE, 0);
		printf("\nthe installation will save %s\n", size);
	} else {
		humanize_number(size, sizeof(size), newsize - oldsize, "B", HN_AUTOSCALE, 0);
		printf("\nthe installation will require %s more space\n", size);
	}
	humanize_number(size, sizeof(size), dlsize, "B", HN_AUTOSCALE, 0);
	printf("%s to be downloaded\n", size);
 
	assume_yes = pkg_config("ASSUME_ALWAYS_YES");
	if (assume_yes && (strcasecmp(assume_yes, "yes") == 0))
		yes = 1;

	if (yes == 0)
		yes = query_yesno("\nProceed with installing packages [y/N]: ");

	if (yes == 1)
		if (pkg_jobs_apply(jobs, 0) != EPKG_OK)
			goto cleanup;

	retcode = 0;

	cleanup:
	pkg_jobs_free(jobs);
	pkgdb_it_free(it);
	pkgdb_close(db);

	return (retcode);
}
