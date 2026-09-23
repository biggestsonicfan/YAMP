#include "SharedLogin.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace net
{
    namespace
    {
        // RPCN's default port. YAMP always connects on it (yampnet_rpcn_config::port = 0), and
        // m2-hle2 reads a stored 0 the same way, so the two only disagree about a server named
        // with a different port - in which case the token is not ours to share.
        constexpr unsigned kDefaultPort = 31313;

        bool SameName(const std::string& a, const char* b)
        {
            return b != nullptr && _stricmp(a.c_str(), b) == 0;
        }

        std::vector<std::string> ReadLines()
        {
            std::vector<std::string> lines;
            std::ifstream in(SharedLoginPath());
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
            return lines;
        }

        // The value of `key`, or nullptr when the line is not that key.
        const char* ValueOf(const std::string& line, const char* key)
        {
            const size_t n = strlen(key);
            if (line.size() <= n || line.compare(0, n, key) != 0 || line[n] != '=')
                return nullptr;
            return line.c_str() + n + 1;
        }

        // Written whole and renamed into place, as m2-hle2 does: a stream and its training runs
        // read this file too, and none of them may see half of it.
        bool WriteWhole(const std::wstring& path, const std::string& text)
        {
            const std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f)
                    return false;
                f.write(text.data(), std::streamsize(text.size()));
                if (!f)
                {
                    f.close();
                    DeleteFileW(tmp.c_str());
                    return false;
                }
            }
            // Windows will not replace a file another process has open, and every copy of m2-hle2
            // reads this one. A read is over in a moment.
            bool moved = false;
            for (int tries = 0; !moved && tries < 20; ++tries)
            {
                moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
                if (!moved)
                    Sleep(10);
            }
            if (!moved)
            {
                DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }
    }

    std::wstring SharedLoginPath()
    {
        wchar_t appData[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return {};
        return std::wstring(appData) + L"\\m2hle2\\netplay.cfg";
    }

    bool TwitchTokenIsFor(const char* issuer, const char* owner, const char* server,
                          const char* npid)
    {
        // Both ends connect on RPCN's default port (see kDefaultPort), so the host name is the
        // whole of the server's identity here.
        return issuer != nullptr && *issuer != '\0' && owner != nullptr && *owner != '\0'
            && server != nullptr && *server != '\0'
            && SameName(issuer, server) && SameName(owner, npid);
    }

    std::string SharedTwitchToken(const char* server, const char* npid)
    {
        if (server == nullptr || *server == '\0' || npid == nullptr || *npid == '\0'
            || SharedLoginPath().empty())
            return {};

        std::string fileServer, fileNpid, token, owner, issuer;
        unsigned port = 0, issuerPort = 0;
        bool haveIssuer = false;
        for (const std::string& line : ReadLines())
        {
            const char* v = nullptr;
            if ((v = ValueOf(line, "server")) != nullptr)             fileServer = v;
            else if ((v = ValueOf(line, "port")) != nullptr)          port = unsigned(strtoul(v, nullptr, 10));
            else if ((v = ValueOf(line, "npid")) != nullptr)          fileNpid = v;
            else if ((v = ValueOf(line, "twitch_token")) != nullptr)  token = v;
            else if ((v = ValueOf(line, "twitch_npid")) != nullptr)   owner = v;
            else if ((v = ValueOf(line, "twitch_server")) != nullptr) { issuer = v; haveIssuer = true; }
            else if ((v = ValueOf(line, "twitch_port")) != nullptr)   issuerPort = unsigned(strtoul(v, nullptr, 10));
        }
        // A file written before tokens had owners, or issuers: the token was whoever, and
        // wherever, was stored beside it. m2-hle2 reads it the same way.
        if (owner.empty())
            owner = fileNpid;
        if (!haveIssuer || issuer.empty())
        {
            issuer = fileServer;
            issuerPort = port;
        }

        // A token is only good on the server that issued it, and only as its own account. NOT
        // `server`: that is the one m2-hle2's player picked, which can be np.rpcs3.net while the
        // token in the file is still good on ours.
        if (token.empty() || !SameName(issuer, server) || !SameName(owner, npid))
            return {};
        if (issuerPort != 0 && issuerPort != kDefaultPort)
            return {};
        return token;
    }

    bool StoreSharedTwitchToken(const char* server, const char* npid, const char* token)
    {
        const std::wstring path = SharedLoginPath();
        if (path.empty() || server == nullptr || *server == '\0' || npid == nullptr
            || *npid == '\0' || token == nullptr || *token == '\0')
            return false;

        CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);   // may exist

        // Every line YAMP does not own is kept as it was - `server` and `port` included. Those are
        // the server m2-hle2's player PICKED, which is theirs to choose and may well be
        // np.rpcs3.net; where the token came from is said by twitch_server / twitch_port instead.
        std::vector<std::string> lines = ReadLines();
        bool haveNpid = false;
        for (const std::string& line : lines)
        {
            if (const char* v = ValueOf(line, "npid"))
                haveNpid = *v != '\0';
        }

        std::ostringstream out;
        if (lines.empty())
            out << "# m2-hle2 netplay settings. Delete this file to forget them.\n";
        for (const std::string& line : lines)
        {
            if (ValueOf(line, "twitch_token") || ValueOf(line, "twitch_npid")
                || ValueOf(line, "twitch_server") || ValueOf(line, "twitch_port"))
                continue;
            if (!haveNpid && ValueOf(line, "npid"))
                continue;
            out << line << "\n";
        }
        if (!haveNpid)
            out << "npid=" << npid << "\n";
        // Never one without the others, as m2-hle2 writes them: a token whose server was
        // forgotten would be offered to whichever server the file names next.
        out << "twitch_token=" << token << "\ntwitch_npid=" << npid
            << "\ntwitch_server=" << server << "\ntwitch_port=" << kDefaultPort << "\n";
        return WriteWhole(path, out.str());
    }

    bool ForgetSharedTwitchToken(const char* server, const char* npid)
    {
        const std::wstring path = SharedLoginPath();
        // Only a token this server and account would have been offered - the same test a login
        // makes. A token for some other server or account is somebody else's business.
        if (path.empty() || SharedTwitchToken(server, npid).empty())
            return true;

        std::ostringstream out;
        for (const std::string& line : ReadLines())
        {
            if (ValueOf(line, "twitch_token") || ValueOf(line, "twitch_npid")
                || ValueOf(line, "twitch_server") || ValueOf(line, "twitch_port"))
                continue;
            out << line << "\n";
        }
        return WriteWhole(path, out.str());
    }
}
