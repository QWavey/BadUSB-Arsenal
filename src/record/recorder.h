#ifndef GHOST_RECORDER_H
#define GHOST_RECORDER_H

int recorder_start(void);
int recorder_stop(void);

/* Called from net worker when a rule fires; attaches a verdict to the frame
 * number that is currently being written. Thread-safe. */
void recorder_verdict(const char *rule, const char *verdict, const char *detail);

#endif
