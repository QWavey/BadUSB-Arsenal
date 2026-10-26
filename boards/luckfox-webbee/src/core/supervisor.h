#ifndef GHOST_SUPERVISOR_H
#define GHOST_SUPERVISOR_H

typedef int (*svc_fn)(void);  /* 0 = started OK */
typedef int (*svc_stop_fn)(void);

typedef struct {
    const char   *name;
    svc_fn        start;
    svc_stop_fn   stop;
    int           needs;        /* 1 if dependent on cfg flag */
    int          *gate;         /* pointer to g_cfg.xxx */
    int           running;
} svc_t;

int  supervisor_init(void);
int  supervisor_register(svc_t *s);
int  supervisor_start_all(void);
int  supervisor_stop_all(void);
int  supervisor_reconcile(void);  /* called on cfg change */

#endif
