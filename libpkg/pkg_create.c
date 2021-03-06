/*
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"

static int pkg_create_from_dir(struct pkg *, const char *, struct packing *);

static int
pkg_create_from_dir(struct pkg *pkg, const char *root, struct packing *pkg_archive)
{
	char fpath[MAXPATHLEN + 1];
	struct pkg_file *file = NULL;
	struct pkg_dir *dir = NULL;
	char *m;
	int ret;
	const char *mtree;
	bool developer;
	struct stat st;
	char sha256[SHA256_DIGEST_LENGTH * 2 + 1];

	if (pkg_is_valid(pkg) != EPKG_OK) {
		pkg_emit_error("the package is not valid");
		return (EPKG_FATAL);
	}
	/*
	 * if the checksum is not provided in the manifest recompute it
	 */
	while (pkg_files(pkg, &file) == EPKG_OK) {
		if (root != NULL)
			snprintf(fpath, sizeof(fpath), "%s%s", root, pkg_file_get(file, PKG_FILE_PATH));
		else
			strlcpy(fpath, pkg_file_get(file, PKG_FILE_PATH), sizeof(fpath));

		if ((pkg_file_get(file, PKG_FILE_SUM) == NULL || pkg_file_get(file, PKG_FILE_SUM)[0] == '\0') && lstat(fpath, &st) == 0 && !S_ISLNK(st.st_mode)) {
			if (sha256_file(fpath, sha256) != EPKG_OK)
				return (EPKG_FATAL);
			strlcpy(file->sum, sha256, sizeof(file->sum));
		}
	}

	pkg_emit_manifest(pkg, &m);
	packing_append_buffer(pkg_archive, m, "+MANIFEST", strlen(m));
	free(m);

	pkg_get(pkg, PKG_MTREE, &mtree);
	if (mtree != NULL)
		packing_append_buffer(pkg_archive, mtree, "+MTREE_DIRS", strlen(mtree));

	while (pkg_files(pkg, &file) == EPKG_OK) {
		if (root != NULL)
			snprintf(fpath, sizeof(fpath), "%s%s", root, pkg_file_get(file, PKG_FILE_PATH));
		else
			strlcpy(fpath, pkg_file_get(file, PKG_FILE_PATH), sizeof(fpath));

		ret = packing_append_file_attr(pkg_archive, fpath, pkg_file_get(file, PKG_FILE_PATH), file->uname, file->gname, file->perm);
		pkg_config_bool(PKG_CONFIG_DEVELOPER_MODE, &developer);
		if (developer && ret != EPKG_OK)
			return (ret);
	}

	while (pkg_dirs(pkg, &dir) == EPKG_OK) {
		if (root != NULL)
			snprintf(fpath, sizeof(fpath), "%s%s", root, pkg_dir_path(dir));
		else
			strlcpy(fpath, pkg_dir_path(dir), sizeof(fpath));

		ret = packing_append_file_attr(pkg_archive, fpath, pkg_dir_path(dir), dir->uname, dir->gname, dir->perm);
		pkg_config_bool(PKG_CONFIG_DEVELOPER_MODE, &developer);
		if (developer && ret != EPKG_OK)
			return (ret);
	}

	return (EPKG_OK);
}

static struct packing *
pkg_create_archive(const char *outdir, struct pkg *pkg, pkg_formats format, int required_flags)
{
	char *pkg_path = NULL;
	struct packing *pkg_archive = NULL;
	const char *pkgname, *pkgversion;

	/*
	 * Ensure that we have all the information we need
	 */
	assert((pkg->flags & required_flags) == required_flags);

	if (mkdirs(outdir) != EPKG_OK)
		return NULL;

	pkg_get(pkg, PKG_NAME, &pkgname, PKG_VERSION, &pkgversion);
	if (asprintf(&pkg_path, "%s/%s-%s", outdir, pkgname, pkgversion) == -1) {
		pkg_emit_errno("asprintf", "");
		return (NULL);
	}

	if (packing_init(&pkg_archive, pkg_path, format) != EPKG_OK)
		pkg_archive = NULL;

	if (pkg_path != NULL)
		free(pkg_path);

	return pkg_archive;
}

static const char * const scripts[] = {
	"+INSTALL",
	"+PRE_INSTALL",
	"+POST_INSTALL",
	"+POST_INSTALL",
	"+DEINSTALL",
	"+PRE_DEINSTALL",
	"+POST_DEINSTALL",
	"+UPGRADE",
	"+PRE_UPGRADE",
	"+POST_UPGRADE",
	"pkg-install",
	"pkg-pre-install",
	"pkg-post-install",
	"pkg-deinstall",
	"pkg-pre-deinstall",
	"pkg-post-deinstall",
	"pkg-upgrade",
	"pkg-pre-upgrade",
	"pkg-post-upgrade",
	NULL
};

int
pkg_create_staged(const char *outdir, pkg_formats format, const char *rootdir, const char *metadatadir, char *plist)
{
	struct pkg *pkg = NULL;
	struct pkg_file *file = NULL;
	struct pkg_dir *dir = NULL;
	struct packing *pkg_archive = NULL;
	char *manifest = NULL;
	char path[MAXPATHLEN];
	char arch[BUFSIZ];
	int ret = ENOMEM;
	char *buf;
	int i;
	regex_t preg;
	regmatch_t pmatch[2];
	size_t size;
	char *www;

	/* Load the manifest from the metadata directory */
	if (snprintf(path, sizeof(path), "%s/+MANIFEST", metadatadir) == -1)
		goto cleanup;

	pkg_new(&pkg, PKG_FILE);
	if (pkg == NULL)
		goto cleanup;

	if ((ret = pkg_load_manifest_file(pkg, path)) != EPKG_OK) {
		ret = EPKG_FATAL;
		goto cleanup;
	}

	/* if no descriptions provided then try to get it from a file */

	pkg_get(pkg, PKG_DESC, &buf);
	if (buf == NULL) {
		if (snprintf(path, sizeof(path), "%s/+DESC", metadatadir) == -1)
			goto cleanup;
		if (access(path, F_OK) == 0)
			pkg_set_from_file(pkg, PKG_DESC, path);
	}

	/* if no message try to get it from a file */
	pkg_get(pkg, PKG_MESSAGE, &buf);
	if (buf == NULL) {
		if (snprintf(path, sizeof(path), "%s/+DISPLAY", metadatadir) == -1)
			goto cleanup;
		if (access(path, F_OK) == 0)
			pkg_set_from_file(pkg, PKG_MESSAGE, path);
	}

	/* if no arch autodetermine it */
	pkg_get(pkg, PKG_ARCH, &buf);
	if (buf == NULL) {
		pkg_get_myarch(arch, BUFSIZ);
		pkg_set(pkg, PKG_ARCH, arch);
	}

	/* if no mtree try to get it from a file */
	pkg_get(pkg, PKG_MTREE, &buf);
	if (buf == NULL) {
		if (snprintf(path, sizeof(path), "%s/+MTREE_DIRS", metadatadir) == -1)
			goto cleanup;
		if (access(path, F_OK) == 0)
			pkg_set_from_file(pkg, PKG_MTREE, path);
	}

	for (i = 0; scripts[i] != NULL; i++) {
		snprintf(path, sizeof(path), "%s/%s", metadatadir, scripts[i]);
		if (access(path, F_OK) == 0)
			pkg_addscript_file(pkg, path);
	}

	if (plist != NULL && ports_parse_plist(pkg, plist, rootdir) != EPKG_OK) {
		ret = EPKG_FATAL;
		goto cleanup;
	}

	/* if www is not given then try to determine it from description */
	pkg_get(pkg, PKG_WWW, &www);
	if (www == NULL) {
		pkg_get(pkg, PKG_DESC, &buf);
		regcomp(&preg, "^WWW:[[:space:]]*(.*)$", REG_EXTENDED|REG_ICASE|REG_NEWLINE);
		if (regexec(&preg, buf, 2, pmatch, 0) == 0) {
			size = pmatch[1].rm_eo - pmatch[1].rm_so;
			www = strndup(&buf[pmatch[1].rm_so], size);
			pkg_set(pkg, PKG_WWW, www);
			free(www);
		} else {
			pkg_set(pkg, PKG_WWW, "UNKNOWN");
		}
		regfree(&preg);
	} else {
		pkg_set(pkg, PKG_WWW, www);
		free(www);
	}

	/* Create the archive */
	pkg_archive = pkg_create_archive(outdir, pkg, format, 0);
	if (pkg_archive == NULL) {
		ret = EPKG_FATAL; /* XXX do better */
		goto cleanup;
	}

	if (pkg_files(pkg, &file) != EPKG_OK && pkg_dirs(pkg, &dir) != EPKG_OK) {
		/* Now traverse the file directories, adding to the archive */
		packing_append_tree(pkg_archive, metadatadir, NULL);
		packing_append_tree(pkg_archive, rootdir, "/");
	} else {
		pkg_create_from_dir(pkg, rootdir, pkg_archive);
	}

	ret = EPKG_OK;

cleanup:
	if (pkg != NULL)
		free(pkg);
	if (manifest != NULL)
		free(manifest);
	if (ret == EPKG_OK)
		ret = packing_finish(pkg_archive);
	return ret;
}

int
pkg_create_installed(const char *outdir, pkg_formats format, const char *rootdir, struct pkg *pkg)
{
	struct packing *pkg_archive;
	int required_flags = PKG_LOAD_DEPS | PKG_LOAD_FILES | PKG_LOAD_CATEGORIES |
	    PKG_LOAD_DIRS | PKG_LOAD_SCRIPTS | PKG_LOAD_OPTIONS |
	    PKG_LOAD_MTREE | PKG_LOAD_LICENSES ;

	assert(pkg->type == PKG_INSTALLED);

	pkg_archive = pkg_create_archive(outdir, pkg, format, required_flags);
	if (pkg_archive == NULL) {
		pkg_emit_error("unable to create archive");
		return (EPKG_FATAL);
	}

	pkg_create_from_dir(pkg, rootdir, pkg_archive);

	return packing_finish(pkg_archive);
}
