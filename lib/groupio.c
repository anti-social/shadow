
#include <config.h>

#ident "$Id$"

#include <stdio.h>

#include "prototypes.h"
#include "defines.h"
#include "commonio.h"
#include "getdef.h"
#include "groupio.h"

static struct commonio_entry *merge_group_entries (struct commonio_entry *gr1,
                                                   struct commonio_entry *gr2);
static int split_groups (unsigned int max_members);
static int group_open_hook (void);

static void *group_dup (const void *ent)
{
	const struct group *gr = ent;

	return __gr_dup (gr);
}

static void group_free (void *ent)
{
	struct group *gr = ent;

	free (gr->gr_name);
	free (gr->gr_passwd);
	while (*(gr->gr_mem)) {
		free (*(gr->gr_mem));
		gr->gr_mem++;
	}
	free (gr);
}

static const char *group_getname (const void *ent)
{
	const struct group *gr = ent;

	return gr->gr_name;
}

static void *group_parse (const char *line)
{
	return (void *) sgetgrent (line);
}

static int group_put (const void *ent, FILE * file)
{
	const struct group *gr = ent;

	return (putgrent (gr, file) == -1) ? -1 : 0;
}

static int group_close_hook (void)
{
	unsigned int max_members = getdef_unum("MAX_MEMBERS_PER_GROUP", 0);

	if (0 == max_members)
		return 1;

	return split_groups (max_members);
}

static struct commonio_ops group_ops = {
	group_dup,
	group_free,
	group_getname,
	group_parse,
	group_put,
	fgetsx,
	fputsx,
	group_open_hook,
	group_close_hook
};

static struct commonio_db group_db = {
	GROUP_FILE,		/* filename */
	&group_ops,		/* ops */
	NULL,			/* fp */
#ifdef WITH_SELINUX
	NULL,			/* scontext */
#endif
	NULL,			/* head */
	NULL,			/* tail */
	NULL,			/* cursor */
	0,			/* changed */
	0,			/* isopen */
	0,			/* locked */
	0			/* readonly */
};

int gr_name (const char *filename)
{
	return commonio_setname (&group_db, filename);
}

int gr_lock (void)
{
	return commonio_lock (&group_db);
}

int gr_open (int mode)
{
	return commonio_open (&group_db, mode);
}

const struct group *gr_locate (const char *name)
{
	return commonio_locate (&group_db, name);
}

const struct group *gr_locate_gid (gid_t gid)
{
	const struct group *grp;

	gr_rewind ();
	while (   ((grp = gr_next ()) != NULL)
	       && (grp->gr_gid != gid)) {
	}

	return grp;
}

int gr_update (const struct group *gr)
{
	return commonio_update (&group_db, (const void *) gr);
}

int gr_remove (const char *name)
{
	return commonio_remove (&group_db, name);
}

int gr_rewind (void)
{
	return commonio_rewind (&group_db);
}

const struct group *gr_next (void)
{
	return commonio_next (&group_db);
}

int gr_close (void)
{
	return commonio_close (&group_db);
}

int gr_unlock (void)
{
	return commonio_unlock (&group_db);
}

void __gr_set_changed (void)
{
	group_db.changed = 1;
}

struct commonio_entry *__gr_get_head (void)
{
	return group_db.head;
}

struct commonio_db *__gr_get_db (void)
{
	return &group_db;
}

void __gr_del_entry (const struct commonio_entry *ent)
{
	commonio_del_entry (&group_db, ent);
}

static int gr_cmp (const void *p1, const void *p2)
{
	gid_t u1, u2;

	if ((*(struct commonio_entry **) p1)->eptr == NULL)
		return 1;
	if ((*(struct commonio_entry **) p2)->eptr == NULL)
		return -1;

	u1 = ((struct group *) (*(struct commonio_entry **) p1)->eptr)->gr_gid;
	u2 = ((struct group *) (*(struct commonio_entry **) p2)->eptr)->gr_gid;

	if (u1 < u2)
		return -1;
	else if (u1 > u2)
		return 1;
	else
		return 0;
}

/* Sort entries by GID */
int gr_sort ()
{
	return commonio_sort (&group_db, gr_cmp);
}

static int group_open_hook (void)
{
	unsigned int max_members = getdef_unum("MAX_MEMBERS_PER_GROUP", 0);
	struct commonio_entry *gr1, *gr2;

	if (0 == max_members)
		return 1;

	for (gr1 = group_db.head; gr1; gr1 = gr1->next) {
		for (gr2 = gr1->next; gr2; gr2 = gr2->next) {
			struct group *g1 = (struct group *)gr1->eptr;
			struct group *g2 = (struct group *)gr2->eptr;
			if (NULL != g1 &&
			    NULL != g2 &&
			    0 == strcmp (g1->gr_name, g2->gr_name) &&
			    0 == strcmp (g1->gr_passwd, g2->gr_passwd) &&
			    g1->gr_gid == g2->gr_gid) {
				/* Both group entries refer to the same
				 * group. It is a split group. Merge the
				 * members. */
				gr1 = merge_group_entries (gr1, gr2);
				if (NULL == gr1)
					return 0;
				/* Unlink gr2 */
				if (NULL != gr2->next)
					gr2->next->prev = gr2->prev;
				gr2->prev->next = gr2->next;
			}
		}
	}

	return 1;
}

/*
 * Merge the list of members of the two group entries.
 *
 * The commonio_entry arguments shall be group entries.
 *
 * You should not merge the members of two groups if they don't have the
 * same name, password and gid.
 *
 * It merge the members of the second entry in the first one, and return
 * the modified first entry on success, or NULL on failure (with errno
 * set).
 */
static struct commonio_entry *merge_group_entries (struct commonio_entry *gr1,
                                                   struct commonio_entry *gr2)
{
	struct group *gptr1;
	struct group *gptr2;
	char **new_members;
	int members = 0;
	char *new_line;
	int new_line_len, i;
	if (NULL == gr2 || NULL == gr1) {
		errno = EINVAL;
		return NULL;
	}

	gptr1 = (struct group *)gr1->eptr;
	gptr2 = (struct group *)gr2->eptr;
	if (NULL == gptr2 || NULL == gptr1) {
		errno = EINVAL;
		return NULL;
	}

	/* Concatenate the 2 lines */
	new_line_len = strlen (gr1->line) + strlen (gr2->line) +1;
	new_line = (char *)malloc ((new_line_len + 1) * sizeof(char*));
	if (NULL == new_line) {
		errno = ENOMEM;
		return NULL;
	}
	snprintf(new_line, new_line_len, "%s\n%s", gr1->line, gr2->line);
	new_line[new_line_len] = '\0';

	/* Concatenate the 2 list of members */
	for (i=0; NULL != gptr1->gr_mem[i]; i++);
	members += i;
	for (i=0; NULL != gptr2->gr_mem[i]; i++) {
		char **pmember = gptr1->gr_mem;
		while (NULL != *pmember) {
			if (0 == strcmp(*pmember, gptr2->gr_mem[i]))
				break;
			pmember++;
		}
		if (NULL == *pmember)
			members++;
	}
	new_members = (char **)malloc ( (members+1) * sizeof(char*) );
	if (NULL == new_members) {
		errno = ENOMEM;
		return NULL;
	}
	for (i=0; NULL != gptr1->gr_mem[i]; i++)
		new_members[i] = gptr1->gr_mem[i];
	members = i;
	for (i=0; NULL != gptr2->gr_mem[i]; i++) {
		char **pmember = new_members;
		while (NULL != *pmember) {
			if (0 == strcmp(*pmember, gptr2->gr_mem[i]))
				break;
			pmember++;
		}
		if (NULL == *pmember) {
			new_members[members++] = gptr2->gr_mem[i];
			new_members[members] = NULL;
		}
	}

	gr1->line = new_line;
	gptr1->gr_mem = new_members;

	return gr1;
}

/*
 * Scan the group database and split the groups which have more members
 * than specified, if this is the result from a current change.
 *
 * Return 0 on failure (errno set) and 1 on success.
 */
static int split_groups (unsigned int max_members)
{
	struct commonio_entry *gr;

	for (gr = group_db.head; gr; gr = gr->next) {
		struct group *gptr = (struct group *)gr->eptr;
		struct commonio_entry *new;
		struct group *new_gptr;
		unsigned int members = 0;

		/* Check if this group must be split */
		if (!gr->changed)
			continue;
		if (NULL == gptr)
			continue;
		for (members = 0; NULL != gptr->gr_mem[members]; members++);
		if (members <= max_members)
			continue;

		new = (struct commonio_entry *) malloc (sizeof *new);
		if (NULL == new) {
			errno = ENOMEM;
			return 0;
		}
		new->eptr = group_dup(gr->eptr);
		if (NULL == new->eptr) {
			errno = ENOMEM;
			return 0;
		}
		new_gptr = (struct group *)new->eptr;
		new->line = NULL;
		new->changed = 1;

		/* Enforce the maximum number of members on gptr */
		gptr->gr_mem[max_members] = NULL;
		/* The number of members in new_gptr will be check later */
		new_gptr->gr_mem = &new_gptr->gr_mem[max_members];

		/* insert the new entry in the list */
		new->prev = gr;
		new->next = gr->next;
		gr->next = new;
	}

	return 1;
}
