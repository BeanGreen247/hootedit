/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_ASKPASS_H
#define HE_ASKPASS_H
#include <string>

/* Password and passphrase prompts for git and ssh, shown inside the editor the way VS Code does it.
 * git (GIT_ASKPASS) and ssh (SSH_ASKPASS) start a helper that asks for each secret. The helper is this same
 * executable run as `hootedit --askpass "<prompt>"`; it forwards the prompt over a unix socket to the editor,
 * which shows a dialog and sends the answer back. The secret is never written to disk or put on a command line. */

int askpass_client(const char *prompt);                 /* the helper's main(): prints the answer, returns the exit status */
bool askpass_start();                                   /* socket and listener thread; call once; false if it cannot be set up */
const std::string &askpass_env();                       /* "GIT_ASKPASS=... SSH_ASKPASS=... " to put in front of a command line; empty if not started */

struct AskReq { int fd = -1; std::string prompt; };
bool askpass_take(AskReq &r);                           /* the next waiting request, from the main thread */
void askpass_reply(int fd, bool ok, const std::string &text);
void askpass_stop();
#endif
