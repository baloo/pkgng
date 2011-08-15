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
	fprintf(stderr, "usage: pkg search [-gxXcd] pattern\n\n");
	fprintf(stderr, "For more information see 'pkg help search'.\n");
}

int
exec_search(int argc, char **argv)
{
	const char *pattern = NULL;
	match_t match = MATCH_EXACT;
	int  retcode = EPKG_OK;
	pkgdb_field field = FIELD_NAME;
	char size[7];
	int ch;
	int multi_repos = 0;
	int flags = PKG_LOAD_BASIC|PKG_LOAD_CATEGORIES|PKG_LOAD_LICENSES|PKG_LOAD_OPTIONS;
	struct pkgdb *db = NULL;
	struct pkgdb_it *it = NULL;
	struct pkg *pkg = NULL;
	struct pkg_category *cat = NULL;
	struct pkg_license *lic = NULL;
	struct pkg_option *opt = NULL;

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

	while ((retcode = pkgdb_it_next(it, &pkg, flags)) == EPKG_OK) {
		printf("Name       : %s\n", pkg_get(pkg, PKG_NAME));
		printf("Version    : %s\n", pkg_get(pkg, PKG_VERSION));
		printf("Origin     : %s\n", pkg_get(pkg, PKG_ORIGIN));
		printf("Prefix     : %s\n", pkg_get(pkg, PKG_PREFIX));
		printf("Arch:      : %s\n", pkg_get(pkg, PKG_ARCH));

		if (multi_repos == 1)
			printf("Repository : %s [%s]\n", pkg_get(pkg, PKG_REPONAME), pkg_get(pkg, PKG_REPOURL));

		if (!pkg_list_empty(pkg, PKG_CATEGORIES)) {
			printf("Categories :");
			while (pkg_categories(pkg, &cat) == EPKG_OK)
				printf(" %s", pkg_category_name(cat));
			printf("\n");
		}

		if (!pkg_list_empty(pkg, PKG_LICENSES)) {
			printf("Licenses   :");
			while (pkg_licenses(pkg, &lic) == EPKG_OK) {
				printf(" %s", pkg_license_name(lic));
				if (pkg_licenselogic(pkg) != 1)
					printf(" %c", pkg_licenselogic(pkg));
				else
					printf(" ");
			}
			printf("\b \n");
		}

		printf("Maintainer : %s\n", pkg_get(pkg, PKG_MAINTAINER));
		printf("WWW        : %s\n", pkg_get(pkg, PKG_WWW));
		printf("Comment    : %s\n", pkg_get(pkg, PKG_COMMENT));

		if (!pkg_list_empty(pkg, PKG_OPTIONS)) {
			printf("Options    :\n");
			while (pkg_options(pkg, &opt) == EPKG_OK)
				printf("\t%s: %s\n", pkg_option_opt(opt), pkg_option_value(opt));
		}

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
