#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <libutil.h>
#include <sysexits.h>

#include <pkg.h>

#include "search.h"

void
usage_search(void)
{
	fprintf(stderr, "usage, pkg search [-gxXcd] pattern\n");
	fprintf(stderr, "For more information see 'pkg help search'.\n");
}

int
exec_search(int argc, char **argv)
{
	char *pattern;
	match_t match = MATCH_EXACT;
	int  retcode = EPKG_OK;
	pkgdb_field field = FIELD_NAME;
	int ch;
	int multi_repos = 0;
	char size[7];
	struct pkgdb *db = NULL;
	struct pkgdb_it *it = NULL;
	struct pkg *pkg = NULL;

	while ((ch = getopt(argc, argv, "gxXcd")) != -1) {
		switch (ch) {
			case 'g':
				match = MATCH_GLOB;
				break;
			case 'x':
				match = MATCH_REGEX;
				break;
			case 'X':
				match = MATCH_EREGEX;
				break;
			case 'c':
				field = FIELD_COMMENT;
				break;
			case 'd':
				field = FIELD_DESC;
				break;
			default:
				usage_search();
				return (EX_USAGE);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage_search();
		return (EX_USAGE);
	}

	pattern = argv[0];

	if (pkgdb_open(&db, PKGDB_REMOTE) != EPKG_OK)
		return (EPKG_FATAL);

	if ((it = pkgdb_rquery(db, pattern, match, field)) == NULL) {
		pkgdb_close(db);
		return (EPKG_FATAL);
	}

	if (((strcmp(pkg_config("PKG_MULTIREPOS"), "true") == 0)) && (pkg_config("PACKAGESITE") == NULL))
		multi_repos = 1;

	while ((retcode = pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC)) == EPKG_OK) {
		printf("Name       : %s\n", pkg_get(pkg, PKG_NAME));
		printf("Version    : %s\n", pkg_get(pkg, PKG_VERSION));
		printf("Origin     : %s\n", pkg_get(pkg, PKG_ORIGIN));
		printf("Arch:      : %s\n", pkg_get(pkg, PKG_ARCH));
		printf("Maintainer : %s\n", pkg_get(pkg, PKG_MAINTAINER));
		printf("WWW        : %s\n", pkg_get(pkg, PKG_WWW));
		printf("Comment    : %s\n", pkg_get(pkg, PKG_COMMENT));

		if (multi_repos == 1)
			printf("Repository : %s [%s]\n", pkg_get(pkg, PKG_REPONAME), pkg_get(pkg, PKG_REPOURL));

		humanize_number(size, sizeof(size), pkg_new_flatsize(pkg), "B", HN_AUTOSCALE, 0);
		printf("Flat size  : %s\n", size);
		humanize_number(size, sizeof(size), pkg_new_pkgsize(pkg), "B", HN_AUTOSCALE, 0);
		printf("Pkg size   : %s\n\n", size);
	}

	pkg_free(pkg);
	pkgdb_it_free(it);
	pkgdb_close(db);

	return (retcode);
}
