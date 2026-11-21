/* What the agent-control display shows: a plain struct built from the hub's
 * /api/fleet, shared by the firmware and the desktop simulator. It is ~7 KB:
 * keep it static or on the heap, never on a task stack. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AC_MAX_SESSIONS 24
#define AC_MAX_HOSTS 6
#define AC_MAX_USAGE 3

typedef enum {
    AC_WORKING,  /* mid-turn */
    AC_WAITING,  /* blocked on the person: a permission prompt, a question, a dialog */
    AC_IDLE,     /* a session nobody has used yet */
    AC_RUNNING,  /* a headless run: alive, reports no finer state */
    AC_DONE,     /* it has finished what it was asked: a result to go and look at */
    AC_FAILED,
    AC_STOPPED,
    AC_STATUS_COUNT,
} ac_status_t;

typedef struct {
    char key[32];     /* "<host>:<pid>": the row's identity across updates */
    char host[12];
    char title[80];   /* what the session is about */
    char detail[40];  /* what it is waiting for, when waiting */
    char todo[72];    /* when it has stopped: what is left for the person to do, in a line the hub had written */
    ac_status_t status;
    bool quiet;       /* finished with nothing asked of the person: shown dimmed */
    int64_t since;    /* epoch seconds the status began; 0 = not known */
    char tokens[16];  /* what the session has spent, as the hub writes it: "2.1M TOK"; empty = not known */
} ac_session_t;

typedef struct {
    char name[12];
    bool up;          /* the hub can reach it */
    int sessions;
    int waiting;
} ac_host_t;

typedef struct {
    char label[12];   /* which allowance window, as the hub labels it: "WK", "5H" ... */
    int percent;      /* used */
} ac_usage_t;

typedef struct {
    char rev[20];     /* hub's digest of this state; echoed back so the next poll can wait for a change */
    int64_t now;      /* the hub's clock when it answered: the display's only clock */
    int working, waiting, done;
    int n_usage;      /* the Claude plan's allowance windows; none when the hub has no fresh figure */
    ac_usage_t usage[AC_MAX_USAGE];
    int n_hosts;
    ac_host_t hosts[AC_MAX_HOSTS];
    int n_sessions;
    ac_session_t sessions[AC_MAX_SESSIONS];
} ac_model_t;

/* DONE sessions with something left for the person to check: those not shown dimmed. */
static inline int ac_done_to_check(const ac_model_t *m) {
    int n = 0;
    for (int i = 0; m && i < m->n_sessions; i++) n += m->sessions[i].status == AC_DONE && !m->sessions[i].quiet;
    return n;
}
