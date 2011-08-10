#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <fts.h>
#include <string.h>

#include "pkg.h"
#include "pkg_event.h"
#include "pkg_private.h"

static const char *packing_set_format(struct archive *a, pkg_formats format);

struct packing {
	struct archive *aread;
	struct archive *awrite;
	struct archive_entry *entry;
};

int
packing_init(struct packing **pack, const char *path, pkg_formats format)
{
	char archive_path[MAXPATHLEN];
	const char *ext;

	assert(pack != NULL);

	if ((*pack = calloc(1, sizeof(struct packing))) == NULL) {
		pkg_emit_errno("malloc", "packing");
		return (EPKG_FATAL);
	}

	(*pack)->aread = archive_read_disk_new();
	archive_read_disk_set_standard_lookup((*pack)->aread);
	archive_read_disk_set_symlink_physical((*pack)->aread);

	(*pack)->entry = archive_entry_new();

	if (!is_dir(path)) {
		(*pack)->awrite = archive_write_new();
		archive_write_set_format_pax_restricted((*pack)->awrite);
		if ((ext = packing_set_format((*pack)->awrite, format)) == NULL) {
			archive_read_finish((*pack)->aread);
			archive_write_finish((*pack)->awrite);
			archive_entry_free((*pack)->entry);
			*pack = NULL;
			return EPKG_FATAL; /* error set by _set_format() */
		}
		snprintf(archive_path, sizeof(archive_path), "%s.%s", path, ext);

		archive_write_open_filename((*pack)->awrite, archive_path);
	} else { /* pass mode directly write to the disk */
		(*pack)->awrite = archive_write_disk_new();
		archive_write_disk_set_options((*pack)->awrite, EXTRACT_ARCHIVE_FLAGS);
	}

	return (EPKG_OK);
}

int
packing_append_buffer(struct packing *pack, const char *buffer, const char *path, int size)
{
	archive_entry_clear(pack->entry);
	archive_entry_set_filetype(pack->entry, AE_IFREG);
	archive_entry_set_perm(pack->entry, 0644);
	archive_entry_set_gname(pack->entry, "wheel");
	archive_entry_set_uname(pack->entry, "root");
	archive_entry_set_pathname(pack->entry, path);
	archive_entry_set_size(pack->entry, size);
	archive_write_header(pack->awrite, pack->entry);
	archive_write_data(pack->awrite, buffer, size);
	archive_entry_clear(pack->entry);

	return (EPKG_OK);
}

int
packing_append_file(struct packing *pack, const char *filepath, const char *newpath)
{
	return (packing_append_file_attr(pack, filepath, newpath, NULL, NULL, 0));
}

int
packing_append_file_attr(struct packing *pack, const char *filepath, const char *newpath,
		const char *uname, const char *gname, mode_t perm)
{
	int fd;
	int len;
	char buf[BUFSIZ];
	int retcode = EPKG_OK;
	struct stat st;

	archive_entry_clear(pack->entry);
	archive_entry_copy_sourcepath(pack->entry, filepath);

	if (lstat(filepath, &st) != 0) {
		pkg_emit_errno("lstat", filepath);
		retcode = EPKG_FATAL;
		goto cleanup;
	}
	retcode = archive_read_disk_entry_from_file(pack->aread, pack->entry, -1, &st);
	if (retcode != ARCHIVE_OK) {
		pkg_emit_error("%s: %s", filepath, archive_error_string(pack->aread));
		retcode = EPKG_FATAL;
		goto cleanup;
	}
	retcode = EPKG_OK;

	if (newpath != NULL)
		archive_entry_set_pathname(pack->entry, newpath);

	if (archive_entry_filetype(pack->entry) != AE_IFREG) {
		archive_entry_set_size(pack->entry, 0);
	}

	if (uname != NULL)
		archive_entry_set_uname(pack->entry, uname);

	if (gname != NULL)
		archive_entry_set_gname(pack->entry, gname);

	if (perm != 0)
		archive_entry_set_perm(pack->entry, perm);

	archive_write_header(pack->awrite, pack->entry);

	if (archive_entry_size(pack->entry) > 0) {
		if ((fd = open(filepath, O_RDONLY)) < 0) {
			pkg_emit_errno("open", filepath);
			retcode = EPKG_FATAL;
			goto cleanup;
		}

		while ((len = read(fd, buf, sizeof(buf))) > 0 )
			archive_write_data(pack->awrite, buf, len);

		close(fd);
	}

	cleanup:
	archive_entry_clear(pack->entry);
	return (retcode);
}

int
packing_append_tree(struct packing *pack, const char *treepath, const char *newroot)
{
	FTS *fts = NULL;
	FTSENT *fts_e = NULL;
	size_t treelen;
	struct sbuf *sb;
	char *paths[2] = { __DECONST(char *, treepath), NULL };
 
	treelen = strlen(treepath);
	fts = fts_open(paths, FTS_PHYSICAL | FTS_XDEV, NULL);
	if (fts == NULL)
		goto cleanup;

	sb = sbuf_new_auto();
	while ((fts_e = fts_read(fts)) != NULL) {
		switch(fts_e->fts_info) {
		case FTS_D:
		case FTS_DEFAULT:
		case FTS_F:
		case FTS_SL:
		case FTS_SLNONE:
			 /* Skip entries that are shorter than the tree itself */
			 if (fts_e->fts_pathlen <= treelen)
				  break;
			 sbuf_clear(sb);
			 /* Strip the prefix to obtain the target path */
			 if (newroot) /* Prepend a root if one is specified */
				  sbuf_cat(sb, newroot);
			 sbuf_cat(sb, fts_e->fts_path + treelen + 1 /* skip trailing slash */);
			 sbuf_finish(sb);
			 packing_append_file(pack, fts_e->fts_name, sbuf_get(sb));
			 break;
		case FTS_DC:
		case FTS_DNR:
		case FTS_ERR:
		case FTS_NS:
			 /* XXX error cases, check fts_e->fts_errno and
			  *     bubble up the call chain */
			 break;
		default:
			 break;
		}   
	}
	sbuf_free(sb);
cleanup:
	fts_close(fts);
	return EPKG_OK;
}

int
packing_finish(struct packing *pack)
{
	assert(pack != NULL);

	archive_entry_free(pack->entry);
	archive_read_finish(pack->aread);

	archive_write_close(pack->awrite);
	archive_write_finish(pack->awrite);

	free(pack);

	return (EPKG_OK);
}

static const char *
packing_set_format(struct archive *a, pkg_formats format)
{
	switch (format) {
		case TXZ:
			if (archive_write_set_compression_xz(a) == ARCHIVE_OK) {
				return ("txz");
			} else {
				pkg_emit_error("%s", "xz is not supported, trying bzip2");
			}
		case TBZ:
			if (archive_write_set_compression_bzip2(a) == ARCHIVE_OK) {
				return ("tbz");
			} else {
				pkg_emit_error("%s", "bzip2 is not supported, trying gzip");
			}
		case TGZ:
			if (archive_write_set_compression_gzip(a) == ARCHIVE_OK) {
				return ("tgz");
			} else {
				pkg_emit_error("%s", "gzip is not supported, trying plain tar");
			}
		case TAR:
			archive_write_set_compression_none(a);
			return ("tar");
	}
	return (NULL);
}

pkg_formats
packing_format_from_string(const char *str)
{
	if (str == NULL)
		return TXZ;
	if (strcmp(str, "txz") == 0)
		return TXZ;
	if (strcmp(str, "tbz") == 0)
		return TBZ;
	if (strcmp(str, "tgz") == 0)
		return TGZ;
	if (strcmp(str, "tar") == 0)
		return TAR;
	pkg_emit_error("unknown format %s, using txz", str);
	return TXZ;
}
