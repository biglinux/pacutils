#define _GNU_SOURCE /* strcasestr */

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <regex.h>

#include <pacutils.h>

const char *myname = "pacsift", *myver = "0.1";

pu_config_t *config = NULL;
alpm_handle_t *handle = NULL;
alpm_loglevel_t log_level = ALPM_LOG_ERROR | ALPM_LOG_WARNING;

int srch_cache = 0, srch_local = 0, srch_sync = 0;
int invert = 0, re = 0, exact = 0, or = 0;
int osep = '\n', isep = '\n';
alpm_list_t *search_dbs = NULL;
alpm_list_t *repo = NULL, *name = NULL, *description = NULL, *packager = NULL;
alpm_list_t *group = NULL, *license = NULL;
alpm_list_t *ownsfile = NULL;
alpm_list_t *requiredby = NULL;
alpm_list_t *provides = NULL, *depends = NULL, *conflicts = NULL, *replaces = NULL;

typedef const char* (str_accessor) (alpm_pkg_t* pkg);
typedef alpm_list_t* (strlist_accessor) (alpm_pkg_t* pkg);
typedef alpm_list_t* (deplist_accessor) (alpm_pkg_t* pkg);

enum longopt_flags {
	FLAG_CONFIG = 1000,
	FLAG_DBPATH,
	FLAG_DEBUG,
	FLAG_HELP,
	FLAG_NULL,
	FLAG_ROOT,
	FLAG_VERSION,

	FLAG_CACHE,
	FLAG_NAME,
	FLAG_DESCRIPTION,
	FLAG_GROUP,
	FLAG_OWNSFILE,
	FLAG_PACKAGER,
	FLAG_PROVIDES,
	FLAG_DEPENDS,
	FLAG_CONFLICTS,
	FLAG_REPLACES,
	FLAG_REPO,
};

void cleanup(int ret)
{
	alpm_list_free(search_dbs);
	alpm_release(handle);
	pu_config_free(config);

	FREELIST(repo);
	FREELIST(name);
	FREELIST(description);
	FREELIST(packager);

	FREELIST(group);
	FREELIST(license);

	exit(ret);
}

const char *get_dbname(alpm_pkg_t *pkg)
{
	return alpm_db_get_name(alpm_pkg_get_db(pkg));
}

int ptr_cmp(const void *p1, const void *p2)
{
	return p2 - p1;
}

/* regcmp wrapper with error handling */
void _regcomp(regex_t *preg, const char *regex, int cflags)
{
	int err;
	if((err = regcomp(preg, regex, REG_EXTENDED | REG_ICASE | REG_NOSUB)) != 0) {
		char errstr[100];
		regerror(err, preg, errstr, 100);
		fprintf(stderr, "error: invalid regex '%s' (%s)\n", regex,  errstr);
		cleanup(1);
	}
}

alpm_list_t *filter_filelist(alpm_list_t **pkgs, const char *str,
		const char *root, const size_t rootlen)
{
	alpm_list_t *p, *matches = NULL;
	if(strncmp(str, root, rootlen) == 0) { str += rootlen; }
	if(re) {
		regex_t preg;
		_regcomp(&preg, str, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		for(p = *pkgs; p; p = p->next) {
			alpm_filelist_t *files = alpm_pkg_get_files(p->data);
			int i;
			for(i = 0; i < files->count; ++i) {
				if(regexec(&preg, files->files[i].name, 0, NULL, 0) == 0) {
					matches = alpm_list_add(matches, p->data);
					break;
				}
			}
		}
		regfree(&preg);
	} else if (exact) {
		for(p = *pkgs; p; p = p->next) {
			if(alpm_filelist_contains(alpm_pkg_get_files(p->data), str)) {
				matches = alpm_list_add(matches, p->data);
			}
		}
	} else {
		for(p = *pkgs; p; p = p->next) {
			alpm_filelist_t *files = alpm_pkg_get_files(p->data);
			int i;
			for(i = 0; i < files->count; ++i) {
				if(strcasestr(files->files[i].name, str)) {
					matches = alpm_list_add(matches, p->data);
					break;
				}
			}
		}
	}
	for(p = matches; p; p = p->next) {
		*pkgs = alpm_list_remove(*pkgs, p->data, ptr_cmp, NULL);
	}
	return matches;
}

alpm_list_t *filter_str(alpm_list_t **pkgs, const char *str, str_accessor *func)
{
	alpm_list_t *p, *matches = NULL;
	if(re) {
		regex_t preg;
		_regcomp(&preg, str, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		for(p = *pkgs; p; p = p->next) {
			const char *s = func(p->data);
			if(s && regexec(&preg, s, 0, NULL, 0) == 0) {
				matches = alpm_list_add(matches, p->data);
			}
		}
		regfree(&preg);
	} else if(exact) {
		for(p = *pkgs; p; p = p->next) {
			const char *s = func(p->data);
			if(s && strcasecmp(s, str) == 0) {
				matches = alpm_list_add(matches, p->data);
			}
		}
	} else {
		for(p = *pkgs; p; p = p->next) {
			const char *s = func(p->data);
			if(s && strcasestr(s, str)) {
				matches = alpm_list_add(matches, p->data);
			}
		}
	}
	for(p = matches; p; p = p->next) {
		*pkgs = alpm_list_remove(*pkgs, p->data, ptr_cmp, NULL);
	}
	return matches;
}

int depcmp(alpm_depend_t *d, alpm_depend_t *needle)
{
	if(needle->name_hash != d->name_hash || strcmp(needle->name, d->name) != 0) {
		return 1;
	}

	if(!exact && !needle->version) { return 0; }

	if(needle->mod == d->mod
			&& alpm_pkg_vercmp(needle->version, d->version) == 0) {
		return 0;
	}

	return 1;
}

alpm_list_t *filter_deplist(alpm_list_t **pkgs, const char *str, deplist_accessor *func)
{
	alpm_list_t *p, *matches = NULL;
	alpm_depend_t *needle = alpm_dep_from_string(str);
	if(needle == NULL) {
		fprintf(stderr, "error: invalid dependency '%s'\n", str);
		cleanup(1);
	}
	for(p = *pkgs; p; p = p->next) {
		alpm_list_t *deps = func(p->data);
		if(alpm_list_find(deps, needle, (alpm_list_fn_cmp) depcmp)) {
			matches = alpm_list_add(matches, p->data);
		}
	}
	for(p = matches; p; p = p->next) {
		*pkgs = alpm_list_remove(*pkgs, p->data, ptr_cmp, NULL);
	}
	return matches;
}

alpm_list_t *filter_strlist(alpm_list_t **pkgs, const char *str, strlist_accessor *func)
{
	alpm_list_t *p, *matches = NULL;
	if(re) {
		regex_t preg;
		_regcomp(&preg, str, REG_EXTENDED | REG_ICASE | REG_NOSUB);
		for(p = *pkgs; p; p = p->next) {
			alpm_list_t *h = func(p->data);
			for(; h; h = h->next ) {
				if(regexec(&preg, h->data, 0, NULL, 0) == 0) {
					matches = alpm_list_add(matches, p->data);
					break;
				}
			}
		}
		regfree(&preg);
	} else if (exact) {
		for(p = *pkgs; p; p = p->next) {
			if(alpm_list_find_str(func(p->data), str)) {
				matches = alpm_list_add(matches, p->data);
			}
		}
	} else {
		for(p = *pkgs; p; p = p->next) {
			alpm_list_t *h = func(p->data);
			for(; h; h = h->next) {
				if(strcasestr(h->data, str)) {
					matches = alpm_list_add(matches, p->data);
					break;
				}
			}
		}
	}
	for(p = matches; p; p = p->next) {
		*pkgs = alpm_list_remove(*pkgs, p->data, ptr_cmp, NULL);
	}
	return matches;
}

#define match(list, filter) \
	if(list) { \
		alpm_list_t *lp; \
		for(lp = list; lp; lp = alpm_list_next(lp)) { \
			void *i = lp->data; \
			matches = alpm_list_join(matches, filter); \
		} \
		if(!or) { \
			alpm_list_free(haystack); \
			haystack = matches; \
			matches = NULL; \
		} \
	}

alpm_list_t *filter_pkgs(alpm_handle_t *handle, alpm_list_t *pkgs)
{
	alpm_list_t *matches = NULL, *haystack = alpm_list_copy(pkgs);
	const char *root = alpm_option_get_root(handle);
	const size_t rootlen = strlen(root);

	match(name, filter_str(&haystack, i, alpm_pkg_get_name));
	match(description, filter_str(&haystack, i, alpm_pkg_get_desc));
	match(packager, filter_str(&haystack, i, alpm_pkg_get_desc));
	match(repo, filter_str(&haystack, i, get_dbname));
	match(group, filter_strlist(&haystack, i, alpm_pkg_get_groups));
	match(ownsfile, filter_filelist(&haystack, i, root, rootlen));

	match(provides, filter_deplist(&haystack, i, alpm_pkg_get_provides));
	match(depends, filter_deplist(&haystack, i, alpm_pkg_get_depends));
	match(conflicts, filter_deplist(&haystack, i, alpm_pkg_get_conflicts));
	match(replaces, filter_deplist(&haystack, i, alpm_pkg_get_replaces));

	if(invert) {
		matches = alpm_list_diff(pkgs, haystack, ptr_cmp);
		alpm_list_free(haystack);
		return matches;
	} else {
		return haystack;
	}
}

#undef match

void usage(int ret)
{
	FILE *stream = (ret ? stderr : stdout);
#define hputs(str) fputs(str"\n", stream);
	hputs("pacsift - query packages");
	hputs("usage:  pacsift [options] (<field> <term>)...");
	hputs("        pacsift (--help|--version)");
	hputs("options:");
	hputs("   --config=<path>     set an alternate configuration file");
	hputs("   --dbpath=<path>     set an alternate database location");
	hputs("   --null=[sep]        use <sep> to separate values (default NUL)");
	hputs("   --help              display this help information");
	hputs("   --version           display version information");

	hputs("   --invert            display packages which DO NOT match search criteria");
	/*hputs("   --or                OR search terms instead of AND");*/

	hputs("   --exact");
	hputs("   --regex");

	hputs(" Filters:");
	hputs("   Note: filters are unaffected by --invert and --or");
	hputs("   --cache             search packages in cache (EXPERIMENTAL)");
	hputs("   --local             search installed packages");
	hputs("   --sync              search packages in all sync repositories");
	/*hputs("   --depends           limit to packages installed as dependencies");*/
	/*hputs("   --explicit          limit to packages installed explicitly");*/
	/*hputs("   --unrequired        limit to unrequired packages");*/
	/*hputs("   --required          limit to required packages");*/
	/*hputs("   --foreign           limit to packages not in a sync repo");*/
	/*hputs("   --native            limit to packages in a sync repo");*/

	hputs(" Package Fields:");
	hputs("   Note: options specified multiple times will be OR'd");
	hputs("   --repo=<name>       search packages in repo <name>");
	hputs("   --name=<name>");
	hputs("   --description=<desc>");
	hputs("   --packager=<name>");
	hputs("   --group=<name>      search packages in group <name>");
	hputs("   --owns-file=<path>  search packages that own <path>");
	/*hputs("   --license");*/
	hputs("   --provides          search package provides");
	hputs("   --depends           search package dependencies");
	hputs("   --conflicts         search package conflicts");
	hputs("   --replaces          search package replaces");
#undef hputs

	cleanup(ret);
}

pu_config_t *parse_opts(int argc, char **argv)
{
	char *config_file = "/etc/pacman.conf";
	pu_config_t *config = NULL;
	int c;

	char *short_opts = "QS";
	struct option long_opts[] = {
		{ "config"        , required_argument , NULL    , FLAG_CONFIG        } ,
		{ "dbpath"        , required_argument , NULL    , FLAG_DBPATH        } ,
		{ "debug"         , no_argument       , NULL    , FLAG_DEBUG         } ,
		{ "help"          , no_argument       , NULL    , FLAG_HELP          } ,
		{ "version"       , no_argument       , NULL    , FLAG_VERSION       } ,

		{ "cache"         , no_argument       , NULL    , FLAG_CACHE         } ,
		{ "local"         , no_argument       , NULL    , 'Q'                } ,
		{ "sync"          , no_argument       , NULL    , 'S'                } ,

		{ "invert"        , no_argument       , &invert , 1                  } ,
		{ "regex"         , no_argument       , &re     , 1                  } ,
		{ "exact"         , no_argument       , &exact  , 1                  } ,

		{ "null"          , optional_argument , NULL    , FLAG_NULL          } ,

		{ "repo"          , required_argument , NULL    , FLAG_REPO          } ,
		{ "packager"      , required_argument , NULL    , FLAG_PACKAGER      } ,
		{ "name"          , required_argument , NULL    , FLAG_NAME          } ,
		{ "description"   , required_argument , NULL    , FLAG_DESCRIPTION   } ,
		{ "owns-file"     , required_argument , NULL    , FLAG_OWNSFILE      } ,
		{ "group"         , required_argument , NULL    , FLAG_GROUP         } ,

		{ "provides"      , required_argument , NULL    , FLAG_PROVIDES      } ,
		{ "depends"       , required_argument , NULL    , FLAG_DEPENDS       } ,
		{ "conflicts"     , required_argument , NULL    , FLAG_CONFLICTS     } ,
		{ "replaces"      , required_argument , NULL    , FLAG_REPLACES      } ,

		{ 0, 0, 0, 0 },
	};

	if((config = pu_config_new()) == NULL) {
		perror("malloc");
		return NULL;
	}

	while((c = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
		switch(c) {
			case 0:
				/* already handled */
				break;
			case FLAG_CONFIG:
				config_file = optarg;
				break;
			case FLAG_DBPATH:
				free(config->dbpath);
				config->dbpath = strdup(optarg);
				break;
			case FLAG_DEBUG:
				log_level |= ALPM_LOG_DEBUG;
				log_level |= ALPM_LOG_FUNCTION;
				break;
			case FLAG_HELP:
				usage(0);
				break;
			case FLAG_NULL:
				osep = optarg ? optarg[0] : '\0';
				isep = osep;
				break;
			case FLAG_VERSION:
				pu_print_version(myname, myver);
				cleanup(0);
				break;

			case 'Q':
				srch_local = 1;
				break;
			case 'S':
				srch_sync = 1;
				break;
			case FLAG_CACHE:
				srch_cache = 1;
				break;

			case FLAG_REPO:
				repo = alpm_list_add(repo, strdup(optarg));
				break;
			case FLAG_NAME:
				name = alpm_list_add(name, strdup(optarg));
				break;
			case FLAG_PACKAGER:
				packager = alpm_list_add(packager, strdup(optarg));
				break;
			case FLAG_DESCRIPTION:
				description = alpm_list_add(description, strdup(optarg));
				break;
			case FLAG_OWNSFILE:
				ownsfile = alpm_list_add(ownsfile, strdup(optarg));
				break;
			case FLAG_GROUP:
				group = alpm_list_add(group, strdup(optarg));
				break;

			case FLAG_PROVIDES:
				provides = alpm_list_add(provides, strdup(optarg));
				break;
			case FLAG_DEPENDS:
				depends = alpm_list_add(depends, strdup(optarg));
				break;
			case FLAG_REPLACES:
				replaces = alpm_list_add(replaces, strdup(optarg));
				break;
			case FLAG_CONFLICTS:
				conflicts = alpm_list_add(conflicts, strdup(optarg));
				break;

			case '?':
				usage(1);
				break;
		}
	}

	if(!pu_ui_config_load(config, config_file)) {
		fprintf(stderr, "error: could not parse '%s'\n", config_file);
		return NULL;
	}

	return config;
}

void parse_pkg_spec(char *spec, char **pkgname, char **dbname)
{
	char *c;
	if((c = strchr(spec, '/'))) {
		*c = '\0';
		*pkgname = c + 1;
		*dbname = spec;
	} else {
		*dbname = NULL;
		*pkgname = spec;
	}
}

int main(int argc, char **argv)
{
	alpm_list_t *haystack = NULL, *matches = NULL, *i;
	int ret = 0;

	if(!(config = parse_opts(argc, argv))) {
		goto cleanup;
	}

	if(!(handle = pu_initialize_handle_from_config(config))) {
		fprintf(stderr, "error: failed to initialize alpm.\n");
		ret = 1;
		goto cleanup;
	}

	if(!pu_register_syncdbs(handle, config->repos)) {
		fprintf(stderr, "error: no valid sync dbs configured.\n");
		ret = 1;
		goto cleanup;
	}

	if(!isatty(fileno(stdin)) && errno != EBADF) {
		char *buf = NULL;
		size_t len = 0;
		ssize_t read;

		if(srch_local || srch_sync || srch_cache) {
			fprintf(stderr, "error: --local, --sync, and --cache cannot be used as filters\n");
			ret = 1;
			goto cleanup;
		}

		while((read = getdelim(&buf, &len, isep, stdin)) != -1) {
			alpm_pkg_t *pkg;
			if(buf[read - 1] == isep) { buf[read - 1] = '\0'; }
			if((pkg = pu_find_pkgspec(handle, buf))) {
				haystack = alpm_list_add(haystack, pkg);
			} else {
				fprintf(stderr, "warning: could not locate pkg '%s'\n", buf);
			}
		}

		free(buf);
	} else {
		alpm_list_t *p, *s;

		if(!srch_local && !srch_sync && !srch_cache) {
			srch_local = 1;
			srch_sync = 1;
		}

		if(srch_local) {
			for(p = alpm_db_get_pkgcache(alpm_get_localdb(handle)); p; p = p->next) {
				haystack = alpm_list_add(haystack, p->data);
			}
		}
		if(srch_sync) {
			for(s = alpm_get_syncdbs(handle); s; s = s->next) {
				for(p = alpm_db_get_pkgcache(s->data); p; p = p->next) {
					haystack = alpm_list_add(haystack, p->data);
				}
			}
		}
		if(srch_cache) {
			for(i = alpm_option_get_cachedirs(handle); i; i = i->next) {
				const char *path = i->data;
				DIR *dir = opendir(path);
				struct dirent entry, *result;
				if(!dir) {
					fprintf(stderr, "warning: could not open cache dir '%s' (%s)\n",
							path, strerror(errno));
					continue;
				}
				while(readdir_r(dir, &entry, &result) == 0 && result != NULL) {
					if(strcmp(".", entry.d_name) == 0 || strcmp("..", entry.d_name) == 0) {
						continue;
					}
					size_t path_len = strlen(path) + strlen(entry.d_name);
					char *filename = malloc(path_len + 1);
					int needfiles = ownsfile ? 1 : 0;
					alpm_pkg_t *pkg = NULL;
					sprintf(filename, "%s%s", path, entry.d_name);
					if(alpm_pkg_load(handle, filename, needfiles, 0, &pkg) == 0) {
						haystack = alpm_list_add(haystack, pkg);
					} else {
						fprintf(stderr, "warning: could not load package '%s' (%s)\n",
								filename, alpm_strerror(alpm_errno(handle)));
					}
					free(filename);
				}
				closedir(dir);
			}
		}
	}

	matches = filter_pkgs(handle, haystack);
	for(i = matches; i; i = i->next) {
		pu_fprint_pkgspec(stdout, i->data);
		fputc(osep, stdout);
	}

cleanup:
	alpm_list_free(matches);
	alpm_list_free_inner(haystack, (alpm_list_fn_free) alpm_pkg_free);
	alpm_list_free(haystack);

	cleanup(ret);

	return 0;
}

/* vim: set ts=2 sw=2 noet: */
