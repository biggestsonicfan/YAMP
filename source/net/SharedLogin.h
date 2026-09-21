#pragma once

// The Twitch login token, shared with the other RPCN client on this machine.
//
// RPCN KEEPS ONE TWITCH LOGIN TOKEN PER ACCOUNT. Every device flow issues a fresh one and the
// token before it stops working (the server overwrites its hash), so two clients that each
// keep their own copy take turns signing each other out: YAMP signs in with Twitch and m2-hle2's
// stored token is refused the next time it logs in, and the other way round. On a machine where
// one of them is a headless stream there is nobody to approve a new code, and its lobby simply
// stops opening.
//
// m2-hle2 keeps its sign-in in ONE per-user file, %APPDATA%\m2hle2\netplay.cfg, which every copy
// of it reads. YAMP joins in: a Twitch sign-in here is written into that file too, and a login
// looks there for a newer token before it trusts its own. Only the Twitch fields are touched -
// the file's other settings belong to m2-hle2, and YAMP rewrites them exactly as it found them.
//
// Format, as m2-hle2 writes it: one key=value per line, '#' starts a comment. The keys used here
// are `server`, `port`, `npid`, `twitch_token` and `twitch_npid` (the account the token logs in
// as). Account names compare without case, as RPCN's do.

#include <string>

namespace net
{
    // The token stored for `npid` on `server`, or "" when there is none (or no file at all).
    std::string SharedTwitchToken(const char* server, const char* npid);

    // Records a token the Twitch flow just issued. Returns false if the file could not be written.
    bool StoreSharedTwitchToken(const char* server, const char* npid, const char* token);

    // Where the file is, for log lines.
    std::wstring SharedLoginPath();
}
