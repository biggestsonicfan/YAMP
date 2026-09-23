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
// are `npid`, `twitch_token`, `twitch_npid` (the account the token logs in as) and
// `twitch_server` / `twitch_port` (the server that issued it). Account names compare without
// case, as RPCN's do.
//
// TWO SERVERS. m2-hle2 signs in to the official RPCN (np.rpcs3.net) or ours, and `server` /
// `port` are the one its player picked - which says nothing about where the token came from. A
// token is a password and only ever goes to its issuer, so it is matched against twitch_server,
// and YAMP never touches `server`. A file from before twitch_server existed (m2-hle2 before
// 6b8bb30) meant `server`, and is read that way.

#include <string>

namespace net
{
    // The token stored for `npid` on `server`, or "" when there is none (or no file at all).
    std::string SharedTwitchToken(const char* server, const char* npid);

    // Records a token the Twitch flow just issued. Returns false if the file could not be written.
    bool StoreSharedTwitchToken(const char* server, const char* npid, const char* token);

    // Removes the file's token if it is the one `server` would be offered for `npid`, leaving
    // every other line alone. m2-hle2 on this machine then has to sign in again. Returns false
    // only if the file could not be rewritten.
    bool ForgetSharedTwitchToken(const char* server, const char* npid);

    // Where the file is, for log lines.
    std::wstring SharedLoginPath();

    // May a Twitch token issued by `issuer` for `owner` be offered to `server` as `npid`? The one
    // test every use of a token goes through - YAMP's own saved one as well as the shared one -
    // so a token never reaches a server that did not issue it (m2-hle2's netplay_twitch_here).
    bool TwitchTokenIsFor(const char* issuer, const char* owner, const char* server,
                          const char* npid);
}
