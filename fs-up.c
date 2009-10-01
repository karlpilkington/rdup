/* 
 * Copyright (c) 2009 Miek Gieben
 * See LICENSE for the license
 * rdup-up -- update an directory tree with 
 * and rdup archive
 *
 * File related functions
 *
 */

#include "rdup-up.h"
#include "protocol.h"

extern sig_atomic_t sig;
extern gboolean opt_dry;
extern guint opt_strip;
extern gint opt_verbose;
extern GSList *hlink_list;

/* signal.c */
void got_sig(int signal);

static gboolean
mk_time(struct rdup *e)
{
	struct utimbuf ut;
	/* don't carry the a_time, how cares anyway with noatime? */
	ut.actime = ut.modtime = e->f_mtime;

	if (utime(e->f_name, &ut) == -1) { /* todo */ }
	return TRUE;
}

/* set time also */
static gboolean
mk_mode(struct rdup *e) 
{
	/* todo: error checking */
	chmod(e->f_name, e->f_mode);
	if (getuid() == 0)
		if (chown(e->f_name, e->f_uid, e->f_gid) == -1) { } /* todo */

	mk_time(e);
	return TRUE;
}

static gboolean
mk_dev(struct rdup *e) 
{
	gchar *parent;
	struct stat *st;

	if (opt_dry)
		return TRUE;

	if (!rm(e->f_name)) {
		msg(_("Failed to remove existing entry: '%s\'"), e->f_name);
		return FALSE;
	}

	if (mknod(e->f_name, e->f_mode, e->f_rdev) == -1) {
		if (errno == EACCES) {
			parent = dir_parent(e->f_name);
			st = dir_write(parent);
			if (mknod(e->f_name, e->f_mode, e->f_rdev) == -1) {
				msg(_("Failed to make device: `%s\': %s"), e->f_name, strerror(errno));
				dir_restore(parent, st);
				g_free(parent);
				return FALSE;
			}
			dir_restore(parent, st);
			g_free(parent);
		} else {
			msg(_("Failed to make device: `%s\': %s"), e->f_name, strerror(errno));
			return FALSE;
		}
	}
	mk_mode(e);
	return TRUE;
}

static gboolean
mk_sock(struct rdup *e) 
{
	gchar *parent;
	struct stat *st;

	if (opt_dry)
		return TRUE;

	if (!rm(e->f_name)) {
		msg(_("Failed to remove existing entry: '%s\'"), e->f_name);
		return FALSE;
	}

	if (mkfifo(e->f_name, e->f_mode) == -1) {
		if (errno == EACCES) {
			parent = dir_parent(e->f_name);
			st = dir_write(parent);
			if (mkfifo(e->f_name, e->f_mode) == -1) {
				msg(_("Failed to make socket: `%s\': %s"), e->f_name, strerror(errno));
				dir_restore(parent, st);
				g_free(parent);
				return FALSE;
			}
			dir_restore(parent, st);
			g_free(parent);
		} else {
			msg(_("Failed to make socket: `%s\': %s"), e->f_name, strerror(errno));
			return FALSE;
		}
	}
	mk_mode(e);
	return TRUE;
}

static gboolean
mk_link(struct rdup *e, char *p)
{
	struct stat *st;
	gchar *t;
	gchar *parent;

	if (opt_dry)
		return TRUE;

	if (!rm(e->f_name)) {
		msg(_("Failed to remove existing entry: '%s\'"), e->f_name);
		return FALSE;
	}

	/* symlink */
	if (S_ISLNK(e->f_mode)) {
		if (symlink(e->f_target, e->f_name) == -1) {
			if (errno == EACCES) {
				parent = dir_parent(e->f_name);
				st = dir_write(parent);
				if (symlink(e->f_target, e->f_name) == -1) {
					msg(_("Failed to make symlink: `%s -> %s\': %s"), e->f_name, e->f_target, strerror(errno));
					dir_restore(parent, st);
					g_free(parent);
					return FALSE;
				} 
				dir_restore(parent, st);
				g_free(parent);
			} else {
				msg(_("Failed to make symlink: `%s -> %s\': %s"), e->f_name, e->f_target, strerror(errno));
				return FALSE;
			}
		}
		if (getuid() == 0)
			if (lchown(e->f_name, e->f_uid, e->f_gid) == -1) { /* todo */ }
		return TRUE;
	}

	/* hardlink */
	/* target must also fall in backup dir */
	t = g_strdup_printf("%s%s", p, e->f_target);
	e->f_target = t;
	hlink_list = g_slist_append(hlink_list, e);
	return TRUE;
}

static gboolean
mk_reg(FILE *in, struct rdup *e)
{
	FILE *out = NULL;
	char *buf;
	size_t  bytes;
	gboolean ok = TRUE;
	gboolean old_dry = opt_dry;
	struct stat *st;

	/* with opt_dry we can't just return TRUE; as we may 
	 * need to suck in the file's content - which is thrown
	 * away in that case */

	if (! e->f_name) {
		/* fake an opt_dry */
		opt_dry = TRUE;
	}

	if (!opt_dry)  {
		if (!rm(e->f_name)) {
			msg(_("Failed to remove existing entry: '%s\'"), e->f_name);
			opt_dry = old_dry;
			return FALSE;
		}
	}
	if (!opt_dry && !(out = fopen(e->f_name, "w"))) {
		if (errno == EACCES) {
			st = dir_write(dir_parent(e->f_name));
			if (!(out = fopen(e->f_name, "w"))) {
				msg(_("Failed to open file `%s\': %s"), e->f_name, strerror(errno));
				ok = FALSE;
			} else {
				ok = TRUE;
			}
			dir_restore(dir_parent(e->f_name), st);
		} else {
			msg(_("Failed to open file `%s\': %s"), e->f_name, strerror(errno));
			ok = FALSE;
		}
	} 
	if (ok && !opt_dry) {
		chmod(e->f_name, e->f_mode);
		if (getuid() == 0)
			if (fchown(fileno(out), e->f_uid, e->f_gid) == -1) { } /* todo */
	}

	/* we need to read the input to not upset
	 * the flow into rdup-up, but we are not
	 * creating anything when opt_dry is active
	 */
	buf   = g_malloc(BUFSIZE + 1);
	while ((bytes = block_in_header(in)) > 0) {
		if (block_in(in, bytes, buf) == -1) {
			if (out)
				fclose(out);
			opt_dry = old_dry;
			return FALSE;
		}
		if (ok && !opt_dry) {
			if (fwrite(buf, sizeof(char), bytes, out) != bytes) {
				msg(_("Write failure `%s\': %s"), e->f_name, strerror(errno));
				if (out)
					fclose(out);
				opt_dry = old_dry;
				return FALSE;
			}
		}
	}
	g_free(buf);
	if (ok && out)
		fclose(out); 
	if (!opt_dry)
		mk_mode(e);

	opt_dry = old_dry;
	return TRUE;
}

static gboolean
mk_dir(struct rdup *e) 
{
	struct stat *s;
	struct stat st;
	gchar *parent;

	if (opt_dry)
		return TRUE;

	lstat(e->f_name, &st);
	if (S_ISDIR(st.st_mode)) {
		/* something dir is here - update the perms and ownership */
		mk_mode(e);
		return TRUE;
	}

	if (mkdir(e->f_name, e->f_mode) == -1) {
		if (errno == EACCES) {
			/* make parent dir writable, and try again */
			parent = dir_parent(e->f_name);
			s = dir_write(parent);
			if (mkdir(e->f_name, e->f_mode) == -1) {
				msg(_("Failed to create directory `%s\': %s"), e->f_name, strerror(errno));
				dir_restore(parent, s);
				g_free(parent);
				return FALSE;
			}
			dir_restore(parent, s);
			g_free(parent);
		} else {
			msg(_("Failed to create directory `%s\': %s"), e->f_name, strerror(errno));
			return FALSE;
		}
	}
	mk_mode(e);
	return TRUE;
}


/* make an object in the filesystem */
gboolean
mk_obj(FILE *in, char *p, struct rdup *e) 
{
	/* -v */
	if (opt_verbose == 1 && e->f_name)
		fprintf(stdout, "%s\n", e->f_name);

	/* -vv */
	if (opt_verbose == 2 && e->f_name)
		fprintf(stdout, "%c %d %d %s\n", 
				e->plusmin == PLUS ? '+' : '-',
				e->f_uid, e->f_gid, e->f_name);

	/* split here - or above - return when path is zero lenght
	 * for links check that the f_size is zero */
	switch(e->plusmin) {
		case MINUS:
			if (opt_dry || ! e->f_name)
				return TRUE;

			return rm(e->f_name);
		case PLUS:
			/* opt_dry handled within the subfunctions */

			/* only files, no hardlinks! */
			if (S_ISREG(e->f_mode) && ! e->f_lnk )
				return mk_reg(in, e);

			/* no name, we can exit here - for files this is handled
			 * in mk_reg, because we may need to suck in data */
			if (e->f_name == NULL)
				return TRUE;

			if (S_ISDIR(e->f_mode))
				return mk_dir(e);	

			/* First sym and hardlinks and then regular files */
			if (S_ISLNK(e->f_mode) || e->f_lnk) 
				return mk_link(e, p);

			if (S_ISBLK(e->f_mode) || S_ISCHR(e->f_mode))
				return mk_dev(e);

			if (S_ISSOCK(e->f_mode))
				return mk_sock(e);
	}
	/* huh still alive? */
	return TRUE;
}

/* Create the remaining hardlinks in the target directory */
gboolean
mk_hlink(GSList *h)
{
	struct rdup *e;
	GSList *p;
	struct stat *st;
	gchar *parent;


	if (opt_dry)
		return TRUE;

	for (p = g_slist_nth(h, 0); p; p = p->next) { 
		e = (struct rdup *)p->data;
		if (link(e->f_target, e->f_name) == -1) {
			if (errno == EACCES) {
				parent = dir_parent(e->f_name);
				st = dir_write(parent);
				if (link(e->f_target, e->f_name) == -1) {
					msg(_("Failed to create hardlink `%s -> %s\': %s"),
							e->f_name, e->f_target, strerror(errno));
					dir_restore(parent, st);
					g_free(parent);
					return FALSE;
				}
				dir_restore(parent, st);
				g_free(parent);
				return TRUE;
			} else {
				msg(_("Failed to create hardlink `%s -> %s\': %s"),
						e->f_name, e->f_target, strerror(errno));
				return FALSE;
			}
		}
	}
	return TRUE;
}
