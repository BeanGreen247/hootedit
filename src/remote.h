/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef HE_REMOTE_H
#define HE_REMOTE_H
#include <string>
#include <vector>

/* Remote workspaces over ssh, sftp, ftp/ftps and smb, driven by the ordinary command line clients (ssh, sftp, curl, smbclient).
 * ssh and sftp keep one authenticated connection open (ControlMaster) so a password or passphrase is asked once, through the
 * editor's own prompt (see askpass.h). ftp and smb take a password the editor asks for and hands over in the environment or a
 * private temporary file, never on a command line. Every call blocks: run it on a worker thread. */
struct RemoteProfile {
    std::string name, proto;      /* proto: ssh, sftp, ftp, ftps, smb */
    std::string host, user, path, key;   /* key: an identity file for ssh and sftp, empty for the defaults */
    int port = 0;                 /* 0 = the protocol's default */
};
struct RemoteEntry { std::string name; bool dir = false; };

std::vector<RemoteProfile> remote_load();
void remote_save(const std::vector<RemoteProfile> &v);
bool remote_tools(const RemoteProfile &p, std::string *missing);          /* the command line client for this protocol is installed */
bool remote_needs_password(const RemoteProfile &p);                       /* ftp and smb: the editor asks and passes it in */

int remote_connect(const RemoteProfile &p, const std::string &pw, std::string *err);
void remote_disconnect(const RemoteProfile &p);
int remote_list(const RemoteProfile &p, const std::string &pw, const std::string &dir, std::vector<RemoteEntry> &out, std::string *err);
int remote_get(const RemoteProfile &p, const std::string &pw, const std::string &remote, const std::string &local, std::string *err);
int remote_put(const RemoteProfile &p, const std::string &pw, const std::string &local, const std::string &remote, std::string *err);
std::string remote_ssh_line(const RemoteProfile &p);                      /* "ssh -p 22 user@host" for a terminal */
std::string remote_label(const RemoteProfile &p);                         /* "user@host:22" */
#endif
