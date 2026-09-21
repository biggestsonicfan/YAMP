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
    }

    std::wstring SharedLoginPath()
    {
        wchar_t appData[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return {};
        return std::wstring(appData) + L"\\m2hle2\\netplay.cfg";
    }

    std::string SharedTwitchToken(const char* server, const char* npid)
    {
        if (server == nullptr || *server == '\0' || npid == nullptr || *npid == '\0'
            || SharedLoginPath().empty())
            return {};

        std::string fileServer, fileNpid, token, owner;
        unsigned port = 0;
        for (const std::string& line : ReadLines())
        {
            const char* v = nullptr;
            if ((v = ValueOf(line, "server")) != nullptr)            fileServer = v;
            else if ((v = ValueOf(line, "port")) != nullptr)         port = unsigned(strtoul(v, nullptr, 10));
            else if ((v = ValueOf(line, "npid")) != nullptr)         fileNpid = v;
            else if ((v = ValueOf(line, "twitch_token")) != nullptr) token = v;
            else if ((v = ValueOf(line, "twitch_npid")) != nullptr)  owner = v;
        }
        // A file written before tokens had owners: the token was whoever was stored beside it.
        // m2-hle2 reads it the same way.
        if (owner.empty())
            owner = fileNpid;

        // A token is only good on the server that issued it, and only as its own account.
        if (token.empty() || !SameName(fileServer, server) || !SameName(owner, npid))
            return {};
        if (port != 0 && port != kDefaultPort)
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

        // Every line YAMP does not own is kept as it was. The server moves only when it has to:
        // it is also m2-hle2's default server, and a token belongs with the server it came from.
        std::vector<std::string> lines = ReadLines();
        std::string fileServer;
        bool haveNpid = false;
        for (const std::string& line : lines)
        {
            if (const char* v = ValueOf(line, "server")) fileServer = v;
            if (const char* v = ValueOf(line, "npid"))   haveNpid = *v != '\0';
        }
        const bool moveServer = !SameName(fileServer, server);

        std::ostringstream out;
        if (lines.empty())
            out << "# m2-hle2 netplay settings. Delete this file to forget them.\n";
        for (const std::string& line : lines)
        {
            if (ValueOf(line, "twitch_token") || ValueOf(line, "twitch_npid"))
                continue;
            if (moveServer && (ValueOf(line, "server") || ValueOf(line, "port")))
                continue;
            if (!haveNpid && ValueOf(line, "npid"))
                continue;
            out << line << "\n";
        }
        if (moveServer)
            out << "server=" << server << "\nport=" << kDefaultPort << "\n";
        if (!haveNpid)
            out << "npid=" << npid << "\n";
        out << "twitch_token=" << token << "\ntwitch_npid=" << npid << "\n";

        // Written whole and renamed into place, as m2-hle2 does: a stream and its training runs
        // read this file too, and none of them may see half of it.
        const std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
                return false;
            const std::string text = out.str();
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
