#include <sys/types.h>
#include <sys/sbuf.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "pkg.h"
#include "pkg_event.h"
#include "pkg_util.h"
#include "pkg_private.h"

#define PKG_UNKNOWN -1
#define PKG_DEPS -2
#define PKG_CONFLICTS -3
#define PKG_FILES -4
#define PKG_DIRS -5
#define PKG_FLATSIZE -6
#define PKG_SCRIPTS -7
#define PKG_CATEGORIES -8
#define PKG_LICENSELOGIC -9
#define PKG_LICENSES -10
#define PKG_OPTIONS -11
#define PKG_USERS -12
#define PKG_GROUPS -13

static void parse_mapping(struct pkg *, yaml_node_pair_t *, yaml_document_t *, int);
static void parse_node(struct pkg *, yaml_node_t *, yaml_document_t *, int);

static struct manifest_key {
	const char *key;
	int type;
} manifest_key[] = {
	{ "name", PKG_NAME },
	{ "origin", PKG_ORIGIN },
	{ "version", PKG_VERSION },
	{ "arch", PKG_ARCH },
	{ "osversion", PKG_OSVERSION },
	{ "www", PKG_WWW },
	{ "comment", PKG_COMMENT},
	{ "maintainer", PKG_MAINTAINER},
	{ "prefix", PKG_PREFIX},
	{ "deps", PKG_DEPS},
	{ "conflicts", PKG_CONFLICTS},
	{ "files", PKG_FILES},
	{ "dirs", PKG_DIRS},
	{ "flatsize", PKG_FLATSIZE},
	{ "licenselogic", PKG_LICENSELOGIC },
	{ "licenses", PKG_LICENSES },
	{ "desc", PKG_DESC },
	{ "scripts", PKG_SCRIPTS},
	{ "message", PKG_MESSAGE},
	{ "categories", PKG_CATEGORIES},
	{ "options", PKG_OPTIONS},
	{ "users", PKG_USERS},
	{ "groups", PKG_GROUPS}
};

#define manifest_key_len (int)(sizeof(manifest_key)/sizeof(manifest_key[0]))

static int manifest_type(const char *key) {
	int i = 0;
	
	for (i = 0; i < manifest_key_len; i++) {
		if (!strcasecmp(key, manifest_key[i].key))
			return (manifest_key[i].type);
	}

	return (PKG_UNKNOWN);
}

int
pkg_load_manifest_file(struct pkg *pkg, const char *fpath)
{
	char *manifest = NULL;
	off_t sz;
	int ret = EPKG_OK;

	if ((ret = file_to_buffer(fpath, &manifest, &sz)) != EPKG_OK)
		return (ret);

	ret = pkg_parse_manifest(pkg, manifest);
	free(manifest);

	return (ret);
}

static void
parse_mapping(struct pkg *pkg, yaml_node_pair_t *pair, yaml_document_t *document, int pkgtype)
{
	int type;
	yaml_node_t *key, *subkey;
	yaml_node_t *val, *subval;
	yaml_node_pair_t *subpair;
	char origin[BUFSIZ];
	char version[BUFSIZ];
	char sum[SHA256_DIGEST_LENGTH * 2 + 1];
	char uname[MAXLOGNAME + 1];
	char gname[MAXLOGNAME + 1];
	void *set;
	mode_t perm;
	pkg_script_t script_type;

	key = yaml_document_get_node(document, pair->key);
	val = yaml_document_get_node(document, pair->value);

	switch (pkgtype) {
		case PKG_FILES:
			if (val->type == YAML_SCALAR_NODE) {
				pkg_addfile(pkg, key->data.scalar.value, val->data.scalar.length == 64 ? val->data.scalar.value : NULL);
			} else {
				subpair = val->data.mapping.pairs.start;
				sum[0] = '\0';
				uname[0] = '\0';
				gname[0] = '\0';
				perm = 0;
				set = NULL;
				while (subpair < val->data.mapping.pairs.top) {
					subkey = yaml_document_get_node(document, subpair->key);
					subval = yaml_document_get_node(document, subpair->value);
					if (!strcasecmp(subkey->data.scalar.value, "sum") && subval->data.scalar.length == 64)
						strlcpy(sum, subval->data.scalar.value, sizeof(sum));
					else if (!strcasecmp(subkey->data.scalar.value, "uname") && subval->data.scalar.length <= MAXLOGNAME)
						strlcpy(uname, subval->data.scalar.value, sizeof(uname));
					else if (!strcasecmp(subkey->data.scalar.value, "gname") && subval->data.scalar.length <= MAXLOGNAME)
						strlcpy(gname, subval->data.scalar.value, sizeof(gname));
					else if (!strcasecmp(subkey->data.scalar.value, "perm") && subval->data.scalar.length > 0) {
						if ((set = setmode(subval->data.scalar.value)) == NULL)
							EMIT_PKG_ERROR("Not a valide mode: %s", subval->data.scalar.value);
						else
							perm = getmode(set, 0);
					}
					++subpair;
				}
				pkg_addfile_attr(pkg, key->data.scalar.value, sum[0] != '\0' ? sum : NULL,
						uname[0] != '\0' ? uname : NULL, gname[0] != '\0' ? gname : NULL,
						perm);
			}
			break;
		case PKG_OPTIONS:
			pkg_addoption(pkg, key->data.scalar.value, val->data.scalar.value);
			break;
		case PKG_DEPS:
			subpair = val->data.mapping.pairs.start;
			while (subpair < val->data.mapping.pairs.top) {
				subkey = yaml_document_get_node(document, subpair->key);
				subval = yaml_document_get_node(document, subpair->value);
				if (!strcasecmp(subkey->data.scalar.value, "origin"))
					strlcpy(origin, subval->data.scalar.value, BUFSIZ);
				else if (!strcasecmp(subkey->data.scalar.value, "version"))
					strlcpy(version, subval->data.scalar.value, BUFSIZ);
				else
					EMIT_PKG_ERROR("Ignoring key: (%s: %s) for dependency %s",
								   subkey->data.scalar.value,
								   subval->data.scalar.value,
								   key->data.scalar.value);
				++subpair;
			}
			pkg_adddep(pkg, key->data.scalar.value, origin, version);
			break;
		case PKG_SCRIPTS:
			if (strcmp(key->data.scalar.value, "pre-install") == 0) {
				script_type = PKG_SCRIPT_PRE_INSTALL;
			} else if (strcmp(key->data.scalar.value, "install") == 0) {
				script_type = PKG_SCRIPT_INSTALL;
			} else if (strcmp(key->data.scalar.value, "post-install") == 0) {
				script_type = PKG_SCRIPT_POST_INSTALL;
			} else if (strcmp(key->data.scalar.value, "pre-upgrade") == 0) {
				script_type = PKG_SCRIPT_PRE_UPGRADE;
			} else if (strcmp(key->data.scalar.value, "upgrade") == 0) {
				script_type = PKG_SCRIPT_UPGRADE;
			} else if (strcmp(key->data.scalar.value, "post-upgrade") == 0) {
				script_type = PKG_SCRIPT_POST_UPGRADE;
			} else if (strcmp(key->data.scalar.value, "pre-deinstall") == 0) {
				script_type = PKG_SCRIPT_PRE_DEINSTALL;
			} else if (strcmp(key->data.scalar.value, "deinstall") == 0) {
				script_type = PKG_SCRIPT_DEINSTALL;
			} else if (strcmp(key->data.scalar.value, "post-deinstall") == 0) {
				script_type = PKG_SCRIPT_POST_DEINSTALL;
			} else
				break;

			pkg_addscript(pkg, val->data.scalar.value, script_type);
			break;
		default:
			type = manifest_type(key->data.scalar.value);
			if (type == -1) {
				if (val->type == YAML_SCALAR_NODE)
					EMIT_PKG_ERROR("Unknown line: (%s: %s)\n",
								   key->data.scalar.value,
								   val->data.scalar.value);
				else
					EMIT_PKG_ERROR("Unknown key: (%s)\n",
								   key->data.scalar.value);
				++pair;
				break;
			}
			if (val->type == YAML_SCALAR_NODE) {
				/* just ignore empty lines */
				if (val->data.scalar.length <= 0)
					break;
				type = manifest_type(key->data.scalar.value);
				switch (type) {
					case -1:
						EMIT_PKG_ERROR("Unknown line: (%s: %s)\n",
								key->data.scalar.value,
								val->data.scalar.value);
						break;
					case PKG_FLATSIZE:
						pkg_setflatsize(pkg, strtoimax(val->data.scalar.value, NULL, 10));
						break;
					case PKG_LICENSELOGIC:
						if (!strcmp(val->data.scalar.value, "single"))
							pkg_set_licenselogic(pkg, LICENSE_SINGLE);
						else if ( !strcmp(val->data.scalar.value, "and") || !strcmp(val->data.scalar.value, "dual"))
							pkg_set_licenselogic(pkg, LICENSE_AND);
						else if ( !strcmp(val->data.scalar.value, "or") || !strcmp(val->data.scalar.value, "multi"))
							pkg_set_licenselogic(pkg, LICENSE_OR);
						else
							EMIT_PKG_ERROR("Unknown license logic: %s", val->data.scalar.value);
						break;
					default:
						while (val->data.scalar.length > 0 &&
								val->data.scalar.value[val->data.scalar.length - 1] == '\n') {
							val->data.scalar.value[val->data.scalar.length - 1] = '\0';
							val->data.scalar.length--;
						}
						pkg_set(pkg, type, val->data.scalar.value);
						break;
				}
			} else {
				parse_node(pkg, val, document, type);
			}
			break;
	}
}

static void
parse_node(struct pkg *pkg, yaml_node_t *node, yaml_document_t *document, int pkgtype)
{
	yaml_node_pair_t *pair, *p;
	yaml_node_item_t *item;
	yaml_node_t *nd, *pk, *pv, *key, *val;
	char uname[MAXLOGNAME + 1];
	char gname[MAXLOGNAME + 1];
	void *set;
	mode_t perm;

	switch (node->type) {
		case YAML_SCALAR_NODE:
			/* NOT REACHED THERE SHOULD NOT BE ALONE SCALARS */
			printf("%s\n", (char *)node->data.scalar.value);
			break;
		case YAML_SEQUENCE_NODE:
			switch (pkgtype) {
				case PKG_DIRS:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						if (nd->type == YAML_SCALAR_NODE)
							pkg_adddir(pkg, nd->data.scalar.value);
						else if (nd->type == YAML_MAPPING_NODE) {
							uname[0] = '\0';
							gname[0] = '\0';
							perm = 0;
							set = NULL;
							p = nd->data.mapping.pairs.start;
							pk = yaml_document_get_node(document, p->key);
							pv = yaml_document_get_node(document, p->value);
							pair = pv->data.mapping.pairs.start;
							while (pair < pv->data.mapping.pairs.top) {
								key = yaml_document_get_node(document, pair->key);
								val = yaml_document_get_node(document, pair->value);
								if (!strcasecmp(key->data.scalar.value, "uname") && val->data.scalar.length <= MAXLOGNAME)
									strlcpy(uname, val->data.scalar.value,
											sizeof(uname));
								else if (!strcasecmp(key->data.scalar.value, "gname") && val->data.scalar.length <= MAXLOGNAME)
									strlcpy(gname, val->data.scalar.value,
											sizeof(gname));
								else if (!strcasecmp(key->data.scalar.value, "perm") && val->data.scalar.length > 0) {
									if ((set = setmode(val->data.scalar.value)) == NULL)
										EMIT_PKG_ERROR("Not a valide mode: %s", val->data.scalar.value);
									else
										perm = getmode(set, 0);
								}
								++pair;
							}
							pkg_adddir_attr(pkg, pk->data.scalar.value, uname[0] != '\0' ? uname : NULL,
									gname[0] != '\0' ? gname : NULL, perm);
						}
						++item;
					}
					break;
				case PKG_CATEGORIES:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						pkg_addcategory(pkg, nd->data.scalar.value);
						++item;
					}

					break;
				case PKG_CONFLICTS:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						pkg_addconflict(pkg, nd->data.scalar.value);
						++item;
					}
					break;
				case PKG_LICENSES:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						pkg_addlicense(pkg, nd->data.scalar.value);
						++item;
					}
					break;
				case PKG_USERS:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						pkg_adduser(pkg, nd->data.scalar.value);
						++item;
					}
					break;
				case PKG_GROUPS:
					item = node->data.sequence.items.start;
					while (item < node->data.sequence.items.top) {
						nd = yaml_document_get_node(document, *item);
						pkg_adduser(pkg, nd->data.scalar.value);
						++item;
					}
					break;
			}
			break;
		case YAML_MAPPING_NODE:
			pair = node->data.mapping.pairs.start;
			while (pair < node->data.mapping.pairs.top) {
				parse_mapping(pkg, pair, document, pkgtype);
				++pair;
			}
			break;
		case YAML_NO_NODE:
			break;
	}
}

int
pkg_parse_manifest(struct pkg *pkg, char *buf)
{
	yaml_parser_t parser;
	yaml_document_t doc;
	yaml_node_t *node;
	int retcode = EPKG_OK;

	assert(pkg != NULL);
	assert(buf != NULL);

	yaml_parser_initialize(&parser);
	yaml_parser_set_input_string(&parser, buf, strlen(buf));
	yaml_parser_load(&parser, &doc);

	node = yaml_document_get_root_node(&doc);
	if (node != NULL)
		parse_node(pkg, node, &doc, -1);
	else
		retcode = EPKG_FATAL;

	yaml_document_delete(&doc);
	yaml_parser_delete(&parser);

	return retcode;
}

static int
yaml_write_buf(void *data, unsigned char *buffer, size_t size)
{
	struct sbuf *dest = (struct sbuf *)data;
	sbuf_bcat(dest, buffer, size);
	return (1);
}

static void
manifest_append_seqval(yaml_document_t *doc, int parent, int *seq, const char *title, const char *value)
{
	if (*seq == -1) {
		*seq = yaml_document_add_sequence(doc, NULL, YAML_FLOW_SEQUENCE_STYLE);
		yaml_document_append_mapping_pair(doc, parent,
				yaml_document_add_scalar(doc, NULL, __DECONST(yaml_char_t*, title), strlen(title), YAML_PLAIN_SCALAR_STYLE), *seq);
	}
	yaml_document_append_sequence_item(doc, *seq,
			yaml_document_add_scalar(doc, NULL, __DECONST(yaml_char_t*, value), strlen(value), YAML_PLAIN_SCALAR_STYLE));
}

int
pkg_emit_manifest(struct pkg *pkg, char **dest)
{
	yaml_emitter_t emitter;
	yaml_document_t doc;
	char tmpbuf[BUFSIZ];
	struct pkg_dep *dep = NULL;
	struct pkg_conflict *conflict = NULL;
	struct pkg_option *option = NULL;
	struct pkg_file *file = NULL;
	struct pkg_dir *dir = NULL;
	struct pkg_script *script = NULL;
	struct pkg_category *category = NULL;
	struct pkg_license *license = NULL;
	struct pkg_user *user = NULL;
	struct pkg_group *group = NULL;
	int rc = EPKG_OK;
	int mapping;
	int seq = -1;
	int depsmap = -1;
	int depkv;
	int files = -1;
	int options = -1;
	int scripts = -1;
	const char *script_types;
	struct sbuf *destbuf = sbuf_new_auto();

	yaml_emitter_initialize(&emitter);
	yaml_emitter_set_output(&emitter, yaml_write_buf, destbuf);

#define manifest_append_kv(map, key, val) yaml_document_append_mapping_pair(&doc, map, \
		yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, key), strlen(key), YAML_PLAIN_SCALAR_STYLE), \
		yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, val), strlen(val), YAML_PLAIN_SCALAR_STYLE));

#define manifest_append_kv_literal(map, key, val) yaml_document_append_mapping_pair(&doc, map, \
		yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, key), strlen(key), YAML_PLAIN_SCALAR_STYLE), \
		yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, val), strlen(val), YAML_LITERAL_SCALAR_STYLE));

	yaml_document_initialize(&doc, NULL, NULL, NULL, 1, 1);
	mapping = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);

	manifest_append_kv(mapping, "name", pkg_get(pkg, PKG_NAME));
	manifest_append_kv(mapping, "version", pkg_get(pkg, PKG_VERSION));
	manifest_append_kv(mapping, "origin", pkg_get(pkg, PKG_ORIGIN));
	manifest_append_kv(mapping, "comment", pkg_get(pkg, PKG_COMMENT));
	manifest_append_kv(mapping, "arch", pkg_get(pkg, PKG_ARCH));
	manifest_append_kv(mapping, "osversion", pkg_get(pkg, PKG_OSVERSION));
	manifest_append_kv(mapping, "www", pkg_get(pkg, PKG_WWW));
	manifest_append_kv(mapping, "maintainer", pkg_get(pkg, PKG_MAINTAINER));
	manifest_append_kv(mapping, "prefix", pkg_get(pkg, PKG_PREFIX));
	switch (pkg_licenselogic(pkg)) {
		case LICENSE_SINGLE:
			manifest_append_kv(mapping, "licenselogic", "single");
			break;
		case LICENSE_AND:
			manifest_append_kv(mapping, "licenselogic", "and");
			break;
		case LICENSE_OR:
			manifest_append_kv(mapping, "licenselogic", "or");
			break;
	}

	seq = -1;
	while (pkg_licenses(pkg, &license) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "licenses", pkg_license_name(license));

	snprintf(tmpbuf, BUFSIZ, "%" PRId64, pkg_flatsize(pkg));
	manifest_append_kv(mapping, "flatsize", tmpbuf);
	manifest_append_kv_literal(mapping, "desc", pkg_get(pkg, PKG_DESC));

	while (pkg_deps(pkg, &dep) == EPKG_OK) {
		if (depsmap == -1) {
			depsmap = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
			yaml_document_append_mapping_pair(&doc, mapping,
					yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, "deps"), 4, YAML_PLAIN_SCALAR_STYLE),
					depsmap);
		}

		depkv = yaml_document_add_mapping(&doc, NULL, YAML_FLOW_MAPPING_STYLE);
		yaml_document_append_mapping_pair(&doc, depsmap,
				yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, pkg_dep_name(dep)), strlen(pkg_dep_name(dep)), YAML_PLAIN_SCALAR_STYLE),
				depkv);

		manifest_append_kv(depkv, "origin", pkg_dep_origin(dep));
		manifest_append_kv(depkv, "version", pkg_dep_version(dep));
	}

	seq = -1;
	while (pkg_categories(pkg, &category) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "categories", pkg_category_name(category));

	seq = -1;
	while (pkg_users(pkg, &user) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "users", pkg_user_name(user));

	seq = -1;
	while (pkg_groups(pkg, &group) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "groups", pkg_group_name(group));

	seq = -1;
	while (pkg_conflicts(pkg, &conflict) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "conflicts", pkg_conflict_glob(conflict));

	while (pkg_options(pkg, &option) == EPKG_OK) {
		if (options == -1) {
			options = yaml_document_add_mapping(&doc, NULL, YAML_FLOW_MAPPING_STYLE);
			yaml_document_append_mapping_pair(&doc, mapping,
					yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, "options"), 7, YAML_PLAIN_SCALAR_STYLE),
					options);
		}
		manifest_append_kv(files, pkg_option_opt(option), pkg_option_value(option));
	}

	while (pkg_files(pkg, &file) == EPKG_OK) {
		if (files == -1) {
			files = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
			yaml_document_append_mapping_pair(&doc, mapping,
					yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, "files"), 5, YAML_PLAIN_SCALAR_STYLE),
					files);
		}
		manifest_append_kv(files, pkg_file_path(file), pkg_file_sha256(file) && strlen(pkg_file_sha256(file)) > 0 ? pkg_file_sha256(file) : "-");
	}

	seq = -1;
	while (pkg_dirs(pkg, &dir) == EPKG_OK)
		manifest_append_seqval(&doc, mapping, &seq, "dirs", pkg_dir_path(dir));

	while (pkg_scripts(pkg, &script) == EPKG_OK) {
		if (scripts == -1) {
			scripts = yaml_document_add_mapping(&doc, NULL, YAML_BLOCK_MAPPING_STYLE);
			yaml_document_append_mapping_pair(&doc, mapping, 
					yaml_document_add_scalar(&doc, NULL, __DECONST(yaml_char_t*, "scripts"), 7, YAML_PLAIN_SCALAR_STYLE),
					scripts);
		}
		switch (pkg_script_type(script)) {
			case PKG_SCRIPT_PRE_INSTALL:
				script_types = "pre-install";
				break;
			case PKG_SCRIPT_INSTALL:
				script_types = "install";
				break;
			case PKG_SCRIPT_POST_INSTALL:
				script_types = "post-install";
				break;
			case PKG_SCRIPT_PRE_UPGRADE:
				script_types = "pre-upgrade";
				break;
			case PKG_SCRIPT_UPGRADE:
				script_types = "upgrade";
				break;
			case PKG_SCRIPT_POST_UPGRADE:
				script_types = "post-upgrade";
				break;
			case PKG_SCRIPT_PRE_DEINSTALL:
				script_types = "pre-deinstall";
				break;
			case PKG_SCRIPT_DEINSTALL:
				script_types = "deinstall";
				break;
			case PKG_SCRIPT_POST_DEINSTALL:
				script_types = "post-deinstall";
				break;
		}
		manifest_append_kv_literal(scripts, script_types, pkg_script_data(script));
	}
	if (pkg_get(pkg, PKG_MESSAGE) != NULL && pkg_get(pkg, PKG_MESSAGE)[0] != '\0')
		manifest_append_kv_literal(mapping, "message", pkg_get(pkg, PKG_MESSAGE));

	if (!yaml_emitter_dump(&emitter, &doc))
		rc = EPKG_FATAL;

	sbuf_finish(destbuf);
	*dest = strdup(sbuf_data(destbuf));
	sbuf_delete(destbuf);

	yaml_emitter_delete(&emitter);
	return (rc);

}
