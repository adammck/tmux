/* $Id: session.c,v 1.74 2009-12-26 23:45:21 tcunha Exp $ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "tmux.h"

/* Global session list. */
struct sessions	sessions;
struct sessions dead_sessions;
struct session_groups session_groups;

struct winlink *session_next_activity(struct session *, struct winlink *);
struct winlink *session_previous_activity(struct session *, struct winlink *);

void
session_alert_cancel(struct session *s, struct winlink *wl)
{
	struct session_alert	*sa, *sb;

	sa = SLIST_FIRST(&s->alerts);
	while (sa != NULL) {
		sb = sa;
		sa = SLIST_NEXT(sa, entry);

		if (wl == NULL || sb->wl == wl) {
			SLIST_REMOVE(&s->alerts, sb, session_alert, entry);
			xfree(sb);
		}
	}
}

void
session_alert_add(struct session *s, struct window *w, int type)
{
	struct session_alert	*sa;
	struct winlink		*wl;

	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl == s->curw)
			continue;

		if (wl->window == w &&
		    !session_alert_has(s, wl, type)) {
			sa = xmalloc(sizeof *sa);
			sa->wl = wl;
			sa->type = type;
			SLIST_INSERT_HEAD(&s->alerts, sa, entry);
		}
	}
}

int
session_alert_has(struct session *s, struct winlink *wl, int type)
{
	struct session_alert	*sa;

	SLIST_FOREACH(sa, &s->alerts, entry) {
		if (sa->wl == wl && sa->type == type)
			return (1);
	}

	return (0);
}

int
session_alert_has_window(struct session *s, struct window *w, int type)
{
	struct session_alert	*sa;

	SLIST_FOREACH(sa, &s->alerts, entry) {
		if (sa->wl->window == w && sa->type == type)
			return (1);
	}

	return (0);
}

/* Find session by name. */
struct session *
session_find(const char *name)
{
	struct session	*s;
	u_int		 i;

	for (i = 0; i < ARRAY_LENGTH(&sessions); i++) {
		s = ARRAY_ITEM(&sessions, i);
		if (s != NULL && strcmp(s->name, name) == 0)
			return (s);
	}

	return (NULL);
}

/* Create a new session. */
struct session *
session_create(const char *name, const char *cmd, const char *cwd,
    struct environ *env, struct termios *tio, int idx, u_int sx, u_int sy,
    char **cause)
{
	struct session	*s;
	u_int		 i;

	s = xmalloc(sizeof *s);
	s->references = 0;
	s->flags = 0;

	if (gettimeofday(&s->creation_time, NULL) != 0)
		fatal("gettimeofday failed");
	memcpy(&s->activity_time, &s->creation_time, sizeof s->activity_time);

	s->curw = NULL;
	TAILQ_INIT(&s->lastw);
	RB_INIT(&s->windows);
	SLIST_INIT(&s->alerts);

	paste_init_stack(&s->buffers);

	options_init(&s->options, &global_s_options);
	environ_init(&s->environ);
	if (env != NULL)
		environ_copy(env, &s->environ);

	s->tio = NULL;
	if (tio != NULL) {
		s->tio = xmalloc(sizeof *s->tio);
		memcpy(s->tio, tio, sizeof *s->tio);
	}

	s->sx = sx;
	s->sy = sy;

	for (i = 0; i < ARRAY_LENGTH(&sessions); i++) {
		if (ARRAY_ITEM(&sessions, i) == NULL) {
			ARRAY_SET(&sessions, i, s);
			break;
		}
	}
	if (i == ARRAY_LENGTH(&sessions))
		ARRAY_ADD(&sessions, s);

	if (name != NULL)
		s->name = xstrdup(name);
	else
		xasprintf(&s->name, "%u", i);

	if (cmd != NULL) {
		if (session_new(s, NULL, cmd, cwd, idx, cause) == NULL) {
			session_destroy(s);
			return (NULL);
		}
		session_select(s, RB_ROOT(&s->windows)->idx);
	}

	log_debug("session %s created", s->name);

	return (s);
}

/* Destroy a session. */
void
session_destroy(struct session *s)
{
	u_int	i;

	log_debug("session %s destroyed", s->name);

	if (session_index(s, &i) != 0)
		fatalx("session not found");
	ARRAY_SET(&sessions, i, NULL);
	while (!ARRAY_EMPTY(&sessions) && ARRAY_LAST(&sessions) == NULL)
		ARRAY_TRUNC(&sessions, 1);

	if (s->tio != NULL)
		xfree(s->tio);

	session_group_remove(s);
	session_alert_cancel(s, NULL);
	environ_free(&s->environ);
	options_free(&s->options);
	paste_free_stack(&s->buffers);

	while (!TAILQ_EMPTY(&s->lastw))
		winlink_stack_remove(&s->lastw, TAILQ_FIRST(&s->lastw));
	while (!RB_EMPTY(&s->windows))
		winlink_remove(&s->windows, RB_ROOT(&s->windows));

	xfree(s->name);

	for (i = 0; i < ARRAY_LENGTH(&dead_sessions); i++) {
		if (ARRAY_ITEM(&dead_sessions, i) == NULL) {
			ARRAY_SET(&dead_sessions, i, s);
			break;
		}
	}
	if (i == ARRAY_LENGTH(&dead_sessions))
		ARRAY_ADD(&dead_sessions, s);
	s->flags |= SESSION_DEAD;
}

/* Find session index. */
int
session_index(struct session *s, u_int *i)
{
	for (*i = 0; *i < ARRAY_LENGTH(&sessions); (*i)++) {
		if (s == ARRAY_ITEM(&sessions, *i))
			return (0);
	}
	return (-1);
}

/* Create a new window on a session. */
struct winlink *
session_new(struct session *s,
    const char *name, const char *cmd, const char *cwd, int idx, char **cause)
{
	struct window	*w;
	struct environ	 env;
	const char	*shell;
	u_int		 hlimit;

	environ_init(&env);
	environ_copy(&global_environ, &env);
	environ_copy(&s->environ, &env);
	server_fill_environ(s, &env);

	shell = options_get_string(&s->options, "default-shell");
	if (*shell == '\0' || areshell(shell))
		shell = _PATH_BSHELL;

	hlimit = options_get_number(&s->options, "history-limit");
	w = window_create(
	    name, cmd, shell, cwd, &env, s->tio, s->sx, s->sy, hlimit, cause);
	if (w == NULL) {
		environ_free(&env);
		return (NULL);
	}
	environ_free(&env);

	if (options_get_number(&s->options, "set-remain-on-exit"))
		options_set_number(&w->options, "remain-on-exit", 1);

	return (session_attach(s, w, idx, cause));
}

/* Attach a window to a session. */
struct winlink *
session_attach(struct session *s, struct window *w, int idx, char **cause)
{
	struct winlink	*wl;

	if ((wl = winlink_add(&s->windows, w, idx)) == NULL)
		xasprintf(cause, "index in use: %d", idx);
	session_group_synchronize_from(s);
	return (wl);
}

/* Detach a window from a session. */
int
session_detach(struct session *s, struct winlink *wl)
{
	if (s->curw == wl &&
	    session_last(s) != 0 && session_previous(s, 0) != 0)
		session_next(s, 0);

	session_alert_cancel(s, wl);
	winlink_stack_remove(&s->lastw, wl);
	winlink_remove(&s->windows, wl);
	session_group_synchronize_from(s);
	if (RB_EMPTY(&s->windows)) {
		session_destroy(s);
		return (1);
	}
	return (0);
}

/* Return if session has window. */
int
session_has(struct session *s, struct window *w)
{
	struct winlink	*wl;

	RB_FOREACH(wl, winlinks, &s->windows) {
		if (wl->window == w)
			return (1);
	}
	return (0);
}

struct winlink *
session_next_activity(struct session *s, struct winlink *wl)
{
	while (wl != NULL) {
		if (session_alert_has(s, wl, WINDOW_BELL))
			break;
		if (session_alert_has(s, wl, WINDOW_ACTIVITY))
			break;
		if (session_alert_has(s, wl, WINDOW_CONTENT))
			break;
		wl = winlink_next(wl);
	}
	return (wl);
}

/* Move session to next window. */
int
session_next(struct session *s, int activity)
{
	struct winlink	*wl;

	if (s->curw == NULL)
		return (-1);

	wl = winlink_next(s->curw);
	if (activity)
		wl = session_next_activity(s, wl);
	if (wl == NULL) {
		wl = RB_MIN(winlinks, &s->windows);
		if (activity && ((wl = session_next_activity(s, wl)) == NULL))
			return (-1);
	}
	if (wl == s->curw)
		return (1);
	winlink_stack_remove(&s->lastw, wl);
	winlink_stack_push(&s->lastw, s->curw);
	s->curw = wl;
	session_alert_cancel(s, wl);
	return (0);
}

struct winlink *
session_previous_activity(struct session *s, struct winlink *wl)
{
	while (wl != NULL) {
		if (session_alert_has(s, wl, WINDOW_BELL))
			break;
		if (session_alert_has(s, wl, WINDOW_ACTIVITY))
			break;
		if (session_alert_has(s, wl, WINDOW_CONTENT))
			break;
		wl = winlink_previous(wl);
	}
	return (wl);
}

/* Move session to previous window. */
int
session_previous(struct session *s, int activity)
{
	struct winlink	*wl;

	if (s->curw == NULL)
		return (-1);

	wl = winlink_previous(s->curw);
	if (activity)
		wl = session_previous_activity(s, wl);
	if (wl == NULL) {
		wl = RB_MAX(winlinks, &s->windows);
		if (activity && (wl = session_previous_activity(s, wl)) == NULL)
			return (-1);
	}
	if (wl == s->curw)
		return (1);
	winlink_stack_remove(&s->lastw, wl);
	winlink_stack_push(&s->lastw, s->curw);
	s->curw = wl;
	session_alert_cancel(s, wl);
	return (0);
}

/* Move session to specific window. */
int
session_select(struct session *s, int idx)
{
	struct winlink	*wl;

	wl = winlink_find_by_index(&s->windows, idx);
	if (wl == NULL)
		return (-1);
	if (wl == s->curw)
		return (1);
	winlink_stack_remove(&s->lastw, wl);
	winlink_stack_push(&s->lastw, s->curw);
	s->curw = wl;
	session_alert_cancel(s, wl);
	return (0);
}

/* Move session to last used window. */
int
session_last(struct session *s)
{
	struct winlink	*wl;

	wl = TAILQ_FIRST(&s->lastw);
	if (wl == NULL)
		return (-1);
	if (wl == s->curw)
		return (1);

	winlink_stack_remove(&s->lastw, wl);
	winlink_stack_push(&s->lastw, s->curw);
	s->curw = wl;
	session_alert_cancel(s, wl);
	return (0);
}

/* Find the session group containing a session. */
struct session_group *
session_group_find(struct session *target)
{
	struct session_group	*sg;
	struct session		*s;

	TAILQ_FOREACH(sg, &session_groups, entry) {
		TAILQ_FOREACH(s, &sg->sessions, gentry) {
			if (s == target)
				return (sg);
		}
	}
	return (NULL);
}

/* Find session group index. */
u_int
session_group_index(struct session_group *sg)
{
	struct session_group   *sg2;
	u_int			i;

	i = 0;
	TAILQ_FOREACH(sg2, &session_groups, entry) {
		if (sg == sg2)
			return (i);
		i++;
	}

	fatalx("session group not found");
}

/*
 * Add a session to the session group containing target, creating it if
 * necessary.
 */
void
session_group_add(struct session *target, struct session *s)
{
	struct session_group	*sg;

	if ((sg = session_group_find(target)) == NULL) {
		sg = xmalloc(sizeof *sg);
		TAILQ_INSERT_TAIL(&session_groups, sg, entry);
		TAILQ_INIT(&sg->sessions);
		TAILQ_INSERT_TAIL(&sg->sessions, target, gentry);
	}
	TAILQ_INSERT_TAIL(&sg->sessions, s, gentry);
}

/* Remove a session from its group and destroy the group if empty. */
void
session_group_remove(struct session *s)
{
	struct session_group	*sg;

	if ((sg = session_group_find(s)) == NULL)
		return;
	TAILQ_REMOVE(&sg->sessions, s, gentry);
	if (TAILQ_NEXT(TAILQ_FIRST(&sg->sessions), gentry) == NULL)
		TAILQ_REMOVE(&sg->sessions, TAILQ_FIRST(&sg->sessions), gentry);
	if (TAILQ_EMPTY(&sg->sessions)) {
		TAILQ_REMOVE(&session_groups, sg, entry);
		xfree(sg);
	}
}

/* Synchronize a session to its session group. */
void
session_group_synchronize_to(struct session *s)
{
	struct session_group	*sg;
	struct session		*target;

	if ((sg = session_group_find(s)) == NULL)
		return;

	target = NULL;
	TAILQ_FOREACH(target, &sg->sessions, gentry) {
		if (target != s)
			break;
	}
	session_group_synchronize1(target, s);
}

/* Synchronize a session group to a session. */
void
session_group_synchronize_from(struct session *target)
{
	struct session_group	*sg;
	struct session		*s;

	if ((sg = session_group_find(target)) == NULL)
		return;

	TAILQ_FOREACH(s, &sg->sessions, gentry) {
		if (s != target)
			session_group_synchronize1(target, s);
	}
}

/*
 * Synchronize a session with a target session. This means destroying all
 * winlinks then recreating them, then updating the current window, last window
 * stack and alerts.
 */
void
session_group_synchronize1(struct session *target, struct session *s)
{
	struct winlinks		 old_windows, *ww;
	struct winlink_stack	 old_lastw;
	struct winlink		*wl, *wl2;
	struct session_alert	*sa;

	/* Don't do anything if the session is empty (it'll be destroyed). */
	ww = &target->windows;
	if (RB_EMPTY(ww))
		return;

	/* If the current window has vanished, move to the next now. */
	if (s->curw != NULL &&
	    winlink_find_by_index(ww, s->curw->idx) == NULL &&
	    session_last(s) != 0 && session_previous(s, 0) != 0)
		session_next(s, 0);

	/* Save the old pointer and reset it. */
	memcpy(&old_windows, &s->windows, sizeof old_windows);
	RB_INIT(&s->windows);

	/* Link all the windows from the target. */
	RB_FOREACH(wl, winlinks, ww)
		winlink_add(&s->windows, wl->window, wl->idx);

	/* Fix up the current window. */
	if (s->curw != NULL)
		s->curw = winlink_find_by_index(&s->windows, s->curw->idx);
	else
		s->curw = winlink_find_by_index(&s->windows, target->curw->idx);

	/* Fix up the last window stack. */
	memcpy(&old_lastw, &s->lastw, sizeof old_lastw);
	TAILQ_INIT(&s->lastw);
	TAILQ_FOREACH(wl, &old_lastw, sentry) {
		wl2 = winlink_find_by_index(&s->windows, wl->idx);
		if (wl2 != NULL)
			TAILQ_INSERT_TAIL(&s->lastw, wl2, sentry);
	}

	/* And update the alerts list. */
	SLIST_FOREACH(sa, &s->alerts, entry) {
		wl = winlink_find_by_index(&s->windows, sa->wl->idx);
		if (wl == NULL)
			session_alert_cancel(s, sa->wl);
		else
			sa->wl = wl;
	}

	/* Then free the old winlinks list. */
	while (!RB_EMPTY(&old_windows)) {
		wl = RB_ROOT(&old_windows);
		winlink_remove(&old_windows, wl);
	}
}
