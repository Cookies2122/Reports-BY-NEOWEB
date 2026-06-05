#include <stdio.h>
#include "Reports.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <ctime>
#include <cstring>
#include <atomic>
#include <deque>
#include "include/sql_mm.h"
#include "include/mysql_mm.h"
#include "schemasystem/schemasystem.h"

Reports g_Reports;
PLUGIN_EXPOSE(Reports, g_Reports);

IUtilsApi*   g_pUtils   = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IMenusApi*   g_pMenus   = nullptr;

CEntitySystem*      g_pEntitySystem     = nullptr;
CGlobalVars*        gpGlobals           = nullptr;
IVEngineServer2*    engine              = nullptr;
CGameEntitySystem*  g_pGameEntitySystem = nullptr;

ISteamHTTP* g_http = nullptr;

static std::string g_sHostname = "Server";

static std::map<int, std::string> g_ReportMsgIds;

static void LoadReportMessages()
{
    g_ReportMsgIds.clear();
    KeyValues* kv = new KeyValues("ReportMessages");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/data/reports_data.ini")) {
        kv->deleteThis();
        return;
    }
    for (KeyValues* v = kv->GetFirstValue(); v; v = v->GetNextValue()) {
        const char* k = v->GetName();
        const char* s = v->GetString();
        if (!k || !*k || !s || !*s) continue;
        int rid = atoi(k);
        if (rid > 0) g_ReportMsgIds[rid] = s;
    }
    kv->deleteThis();
}

static void SaveReportMessages()
{
    KeyValues* kv = new KeyValues("ReportMessages");
    char buf[16];
    for (auto& it : g_ReportMsgIds) {
        if (it.second.empty()) continue;
        snprintf(buf, sizeof(buf), "%d", it.first);
        kv->SetString(buf, it.second.c_str());
    }
    kv->SaveToFile(g_pFullFileSystem, "addons/data/reports_data.ini");
    kv->deleteThis();
}

static void SetReportMsgId(int reportId, const std::string& mid)
{
    g_ReportMsgIds[reportId] = mid;
    SaveReportMessages();
}

static std::string GetReportMsgId(int reportId)
{
    auto it = g_ReportMsgIds.find(reportId);
    return (it != g_ReportMsgIds.end()) ? it->second : std::string();
}

struct ChatMessage
{
    std::string name;
    uint64_t    steamid;
    std::string message;
    time_t      time;
};
static std::deque<ChatMessage> g_ChatHistory[64];

struct PendingCustom { int victimSlot; time_t deadline; };
static std::map<int , PendingCustom> g_PendingCustom;

struct MyReport
{
    int         id;
    std::string victimName;
    std::string reason;
    time_t      sentTime;
    int         status;
    std::string verdict;
    std::string adminName;
    time_t      verdictTime;
};
static std::map<int , std::vector<MyReport>> g_MyOpenCache;
static std::map<int , std::vector<MyReport>> g_MyClosedCache;

struct Cfg
{

    int         discordType         = 0;
    std::string discordContent;
    std::string discordWebhook;
    std::string discordBotToken;
    std::string discordChannelId;

    std::string vkToken;
    std::string vkPeerId;

    int         pollInterval        = 5;

    std::string steamApiKey;
    int         serverId             = 1;
    std::string serverName;
    bool        iksChatColors        = false;
    std::string website;
    int         countMessages        = 15;
    std::string commandReason        = "!rp";
    std::vector<std::string> commands;
    int         cooldownTime         = 300;
    int         cooldownPlayerTime   = 43200;
    bool        adminInReports       = false;
    bool        testMode             = false;

    std::vector<std::string> reasons;

    std::string mysqlHost     = "127.0.0.1";
    int         mysqlPort     = 3306;
    std::string mysqlUser;
    std::string mysqlPassword;
    std::string mysqlDatabase;
};
static Cfg g_Cfg;

static std::map<std::string, std::string> g_phrases;

static const char* Tr(const char* key, const char* fallback = "")
{
    auto it = g_phrases.find(key);
    return (it != g_phrases.end() && !it->second.empty()) ? it->second.c_str() : fallback;
}

static void LoadPhrases()
{
    g_phrases.clear();
    KeyValues* kv = new KeyValues("Phrases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/translations/reports.phrases.txt"))
    {
        kv->deleteThis();
        return;
    }
    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues* p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
    {
        const char* v = p->GetString(lang, nullptr);
        if (!v || !*v) v = p->GetString("en", nullptr);
        if (v) g_phrases[p->GetName()] = v;
    }
    kv->deleteThis();
}

static void SayRaw(int slot, const std::string& body)
{
    if (!g_pUtils) return;

    std::string out = body;
    const std::string token = "{PREFIX}";
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::string::npos) {
        const char* pfx = Tr("Prefix", "");
        out.replace(pos, token.size(), pfx);
        pos += strlen(pfx);
    }

    g_pUtils->PrintToChat(slot, "%s", out.c_str());
}

static void SayKey(int slot, const char* key, const char* fallback = "")
{
    SayRaw(slot, Tr(key, fallback));
}

static std::map<uint64_t, time_t> g_LastReportBySender;
static std::map<uint64_t, time_t> g_LastReportOnVictim;

static int RemainingSenderCooldown(uint64_t senderId)
{
    auto it = g_LastReportBySender.find(senderId);
    if (it == g_LastReportBySender.end()) return 0;
    int diff = (int)(time(nullptr) - it->second);
    int left = g_Cfg.cooldownTime - diff;
    return (left > 0) ? left : 0;
}

static int RemainingVictimCooldown(uint64_t victimId)
{
    auto it = g_LastReportOnVictim.find(victimId);
    if (it == g_LastReportOnVictim.end()) return 0;
    int diff = (int)(time(nullptr) - it->second);
    int left = g_Cfg.cooldownPlayerTime - diff;
    return (left > 0) ? left : 0;
}

static ISQLInterface*    g_pSQL    = nullptr;
static IMySQLConnection* g_DbConn  = nullptr;
static std::atomic<bool> g_DbReady{false};

static void DbConnect()
{
    if (!g_pSQL) return;
    IMySQLClient* mysqlClient = g_pSQL->GetMySQLClient();
    if (!mysqlClient) {
        ConColorMsg(Color(255,0,0,255), "[Reports] sql_mm: GetMySQLClient returned NULL\n");
        return;
    }
    if (g_Cfg.mysqlHost.empty() || g_Cfg.mysqlUser.empty() || g_Cfg.mysqlDatabase.empty()) {
        ConColorMsg(Color(255,150,0,255),
            "[Reports] MySQL config incomplete — Connect skipped\n");
        return;
    }

    MySQLConnectionInfo info;
    info.host     = g_Cfg.mysqlHost.c_str();
    info.user     = g_Cfg.mysqlUser.c_str();
    info.pass     = g_Cfg.mysqlPassword.c_str();
    info.database = g_Cfg.mysqlDatabase.c_str();
    info.port     = g_Cfg.mysqlPort;
    info.timeout  = 60;

    g_DbConn = mysqlClient->CreateMySQLConnection(info);
    if (!g_DbConn) {
        ConColorMsg(Color(255,0,0,255), "[Reports] sql_mm: CreateMySQLConnection failed\n");
        return;
    }

    g_DbConn->Connect([](bool ok) {
        g_DbReady = ok;
        if (ok) {
            ConColorMsg(Color(0,255,0,255), "[Reports] MySQL connected via sql_mm\n");
        } else {
            ConColorMsg(Color(255,0,0,255), "[Reports] MySQL connect failed via sql_mm\n");
        }
    });
}

static std::string DbEscape(const std::string& in)
{
    if (!g_DbConn) return in;
    return g_DbConn->Escape(in.c_str());
}

static std::string ExtractJsonValue(const std::string& json, const std::string& key);

class DiscordHttpJob
{
public:
    using DoneCb = std::function<void(bool ok, const std::string& body)>;

    DiscordHttpJob(EHTTPMethod method, std::string url, std::string body, DoneCb cb = {})
        : m_method(method), m_url(std::move(url)),
          m_body(std::move(body)), m_cb(std::move(cb)) {}

    void SetHeader(const std::string& k, const std::string& v) {
        m_headers.emplace_back(k, v);
    }

    bool Start()
    {
        if (!g_http) return false;
        HTTPRequestHandle req = g_http->CreateHTTPRequest(m_method, m_url.c_str());
        if (!req) return false;
        m_req = req;

        g_http->SetHTTPRequestHeaderValue(req, "Accept", "application/json");
        g_http->SetHTTPRequestHeaderValue(req, "Content-Type", "application/json");
        for (auto& h : m_headers) {
            g_http->SetHTTPRequestHeaderValue(req, h.first.c_str(), h.second.c_str());
        }
        if (!m_body.empty()) {
            g_http->SetHTTPRequestRawPostBody(req, "application/json",
                (uint8*)m_body.data(), (uint32)m_body.size());
        }

        SteamAPICall_t hCall{};
        if (!g_http->SendHTTPRequest(req, &hCall)) {
            g_http->ReleaseHTTPRequest(req); m_req = 0;
            return false;
        }
        m_call.SetGameserverFlag();
        m_call.Set(hCall, this, &DiscordHttpJob::OnDone);
        return true;
    }

    void OnDone(HTTPRequestCompleted_t* p, bool bFailed)
    {
        std::string body;
        bool ok = !bFailed && p && p->m_eStatusCode >= 200 && p->m_eStatusCode < 300;
        if (g_http && p) {
            uint32 size = 0;
            g_http->GetHTTPResponseBodySize(p->m_hRequest, &size);
            if (size > 0) {
                std::vector<uint8> buf(size + 1, 0);
                g_http->GetHTTPResponseBodyData(p->m_hRequest, buf.data(), size);
                body.assign((char*)buf.data(), size);
            }
        }
        if (g_http && m_req) g_http->ReleaseHTTPRequest(m_req);
        if (m_cb) m_cb(ok, body);
        delete this;
    }

private:
    CCallResult<DiscordHttpJob, HTTPRequestCompleted_t> m_call;
    HTTPRequestHandle m_req = 0;
    EHTTPMethod m_method;
    std::string m_url, m_body;
    DoneCb m_cb;
    std::vector<std::pair<std::string, std::string>> m_headers;
};

static std::string JsonEsc(const std::string& s)
{
    std::string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8]; snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else out += c;
        }
    }
    return out;
}

static void FetchSteamAvatar(uint64_t sid, std::function<void(std::string avatarUrl)> cb)
{
    if (!cb) return;
    if (!g_http || g_Cfg.steamApiKey.empty()) { cb(""); return; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key=%s&steamids=%llu",
        g_Cfg.steamApiKey.c_str(), (unsigned long long)sid);

    auto* job = new DiscordHttpJob(k_EHTTPMethodGET, url, "",
        [cb](bool ok, const std::string& body) {
            if (!ok || body.empty()) { cb(""); return; }
            cb(ExtractJsonValue(body, "avatarfull"));
        });
    if (!job->Start()) { delete job; cb(""); }
}

static std::string ExtractJsonValue(const std::string& json, const std::string& key)
{
    std::string sk = "\"" + key + "\":";
    size_t pos = json.find(sk);
    if (pos == std::string::npos) return "";
    pos += sk.length();
    while (pos < json.length() && (json[pos]==' '||json[pos]=='\t')) pos++;
    if (pos < json.length() && json[pos] == '"') {
        pos++;
        std::string out;
        while (pos < json.length()) {
            char c = json[pos++];
            if (c == '\\' && pos < json.length()) { out += '/'; ++pos; continue; }
            if (c == '"') break;
            out += c;
        }
        return out;
    }
    return "";
}

static KeyValues* FindSubkeyRecursive(KeyValues* root, const char* name)
{
    if (!root) return nullptr;
    for (KeyValues* k = root; k; k = k->GetNextKey()) {
        if (k->GetName() && !V_stricmp(k->GetName(), name)) return k;
    }
    for (KeyValues* sub = root->GetFirstTrueSubKey(); sub; sub = sub->GetNextTrueSubKey()) {
        if (KeyValues* found = FindSubkeyRecursive(sub, name)) return found;
    }
    return nullptr;
}

static void LoadMysqlConfig()
{
    KeyValues* kv = new KeyValues("databases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg")) {
        ConColorMsg(Color(255,0,0,255),
            "[Reports] Failed to load addons/configs/databases.cfg\n");
        kv->deleteThis();
        return;
    }

    KeyValues* reports = nullptr;
    if (kv->GetName() && !V_stricmp(kv->GetName(), "reports")) {
        reports = kv;
    } else {
        reports = kv->FindKey("reports", false);
        if (!reports) {
            for (KeyValues* sub = kv->GetFirstTrueSubKey(); sub; sub = sub->GetNextTrueSubKey()) {
                if (sub->GetName() && !V_stricmp(sub->GetName(), "reports")) {
                    reports = sub;
                    break;
                }
                if (KeyValues* deep = sub->FindKey("reports", false)) {
                    reports = deep;
                    break;
                }
            }
        }
    }

    if (!reports) {
        ConColorMsg(Color(255,150,0,255),
            "[Reports] No 'reports' section in databases.cfg — MySQL disabled\n");
        kv->deleteThis();
        return;
    }

    g_Cfg.mysqlHost     = reports->GetString("host",     "127.0.0.1");
    g_Cfg.mysqlPort     = reports->GetInt   ("port",     3306);
    g_Cfg.mysqlUser     = reports->GetString("user",     "");

    g_Cfg.mysqlPassword = reports->GetString("pass",     reports->GetString("password", ""));
    g_Cfg.mysqlDatabase = reports->GetString("database", "");

    kv->deleteThis();
}

static std::vector<std::string> SplitCSV(const std::string& src)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= src.size(); ++i) {
        char c = (i < src.size()) ? src[i] : ',';
        if (c == ',') {
            while (!cur.empty() && (cur.front()==' '||cur.front()=='\t')) cur.erase(cur.begin());
            while (!cur.empty() && (cur.back() ==' '||cur.back() =='\t')) cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    return out;
}

static void LoadConfig()
{
    g_Cfg = Cfg();
    KeyValues* kv = new KeyValues("Reports");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/reports.ini"))
    {
        ConColorMsg(Color(255,0,0,255),
            "[Reports] Failed to load addons/configs/reports.ini\n");
        kv->deleteThis();
        return;
    }

    g_Cfg.discordType        = kv->GetInt   ("discord_type",       0);
    g_Cfg.discordContent     = kv->GetString("discord_content",    "");
    g_Cfg.discordWebhook     = kv->GetString("discord_webhook",    "");
    g_Cfg.discordBotToken    = kv->GetString("discord_bot_token",  "");
    g_Cfg.discordChannelId   = kv->GetString("discord_channel_id", "");

    g_Cfg.vkToken            = kv->GetString("vk_token",           "");
    g_Cfg.vkPeerId           = kv->GetString("vk_peer_id",         "");

    g_Cfg.pollInterval       = kv->GetInt   ("poll_interval",      5);

    g_Cfg.steamApiKey        = kv->GetString("steam_api_key",      "");
    g_Cfg.serverId           = kv->GetInt   ("server_id",          1);
    g_Cfg.serverName         = kv->GetString("server_name",        "");
    g_Cfg.iksChatColors      = kv->GetBool  ("iks_chatcolors",     false);
    g_Cfg.website            = kv->GetString("website",            "");
    g_Cfg.countMessages      = kv->GetInt   ("count_messages",     15);
    g_Cfg.commandReason      = kv->GetString("command_reason",     "!rp");
    g_Cfg.cooldownTime       = kv->GetInt   ("cooldown_time",      300);
    g_Cfg.cooldownPlayerTime = kv->GetInt   ("cooldown_player_time", 43200);
    g_Cfg.adminInReports     = kv->GetBool  ("admin_in_reports",   false);
    g_Cfg.testMode           = kv->GetBool  ("test_mode",           false);

    {
        std::string raw = kv->GetString("commands", "!report,!reports");
        for (auto& c : raw) if (c == ';') c = ',';
        g_Cfg.commands = SplitCSV(raw);
        if (g_Cfg.commands.empty()) {
            g_Cfg.commands.push_back("!report");
            g_Cfg.commands.push_back("!reports");
        }
    }

    {
        g_Cfg.reasons.clear();
        if (KeyValues* sub = kv->FindKey("reasons", false)) {
            for (KeyValues* v = sub->GetFirstValue(); v; v = v->GetNextValue()) {
                const char* s = v->GetString();
                if (s && *s) g_Cfg.reasons.push_back(s);
            }
        }
    }

    kv->deleteThis();

    LoadMysqlConfig();
}

static std::string GetMapName()
{
    if (!gpGlobals) return "unknown";
    const char* m = gpGlobals->mapname.ToCStr();
    return (m && *m) ? std::string(m) : std::string("unknown");
}

static void RefreshHostname()
{
    if (g_pCVar) {
        ConVarRefAbstract var("hostname");
        if (var.IsValidRef()) {
            const char* s = var.GetString();
            if (s && *s) { g_sHostname = s; return; }
        }
    }
    if (!g_Cfg.serverName.empty()) { g_sHostname = g_Cfg.serverName; return; }
    g_sHostname = "Server";
}

static std::string GetPlayerIP(int slot)
{
    if (!g_pPlayers) return "";
    const char* ip = g_pPlayers->GetIpAddress(slot);
    return (ip && *ip) ? std::string(ip) : std::string("");
}

static bool CheckPrime(uint64_t sid)
{
    if (!SteamGameServer()) return false;

    CSteamID id((uint64)sid);
    return SteamGameServer()->UserHasLicenseForApp(id, 624820) == 0 ||
           SteamGameServer()->UserHasLicenseForApp(id, 54029)  == 0;
}

struct PlayerStats { int kills=0; int deaths=0; };
static PlayerStats GetStats(int slot)
{
    PlayerStats s;
    CCSPlayerController* pc = CCSPlayerController::FromSlot(slot);
    if (!pc) return s;
    if (auto* ats = pc->m_pActionTrackingServices()) {
        s.kills  = ats->m_matchStats().m_iKills();
        s.deaths = ats->m_matchStats().m_iDeaths();
    }
    return s;
}

static std::string FormatDiscordTime(const char* tplKey)
{

    const char* fmt = Tr(tplKey, "%d.%m %H:%M");
    char buf[128];
    time_t now = time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}

static void BuildNewReportPayload(int newReportId,
                                  const std::string& serverName,
                                  const std::string& senderName,  uint64_t senderSid,
                                  const std::string& victimName,  uint64_t victimSid,
                                  const std::string& reason,
                                  int kills, int deaths, bool prime,
                                  const std::string& mapName, const std::string& ip,
                                  bool includeContentPing,
                                  std::function<void(std::string json)> onJson)
{
    FetchSteamAvatar(victimSid, [=](std::string avatarUrl)
    {
        float kd = (deaths > 0) ? (float)kills / (float)deaths : (float)kills;

        const char* tpl = Tr("WebHookSendBody",
            "# 🔔 NEW REPORT #%i\n- Sender: %s\n- Offender: %s\n");
        char body[4096];
        snprintf(body, sizeof(body), tpl,
            newReportId,
            senderName.c_str(), std::to_string(senderSid).c_str(),
            victimName.c_str(), std::to_string(victimSid).c_str(),
            reason.c_str(),
            kd, kills, deaths,
            prime ? "✔" : "✖",
            mapName.c_str());

        std::string urlLine;
        if (!g_Cfg.website.empty()) {
            char urlBuf[512];
            const char* urlTpl = Tr("WebHookReportURL", "[Review](%s/%i/%i/)");
            snprintf(urlBuf, sizeof(urlBuf), urlTpl,
                g_Cfg.website.c_str(), g_Cfg.serverId, newReportId);
            urlLine = urlBuf;
        }

        char snameLine[256];
        char tmpName[200];
        snprintf(tmpName, sizeof(tmpName),
            Tr("WebHookServerName", "Server: %s"), serverName.c_str());
        snprintf(snameLine, sizeof(snameLine), "**%s**", tmpName);

        std::string description = std::string(snameLine) + "\n" + body;

        std::string json = "{";
        if (includeContentPing && !g_Cfg.discordContent.empty())
            json += "\"content\":\"" + JsonEsc(g_Cfg.discordContent) + "\",";
        json += "\"embeds\":[{";
        json += "\"color\":15158332,";
        json += "\"description\":\"" + JsonEsc(description) + "\",";

        char sidStr[32]; snprintf(sidStr, sizeof(sidStr), "%llu", (unsigned long long)victimSid);
        std::string sidBlock = std::string("```") + sidStr + "```";
        json += "\"fields\":[";
        json += "{\"name\":\":id: STEAM ID\",\"value\":\"" + JsonEsc(sidBlock) + "\",\"inline\":true}";
        if (!ip.empty()) {
            std::string ipBlock = std::string("```") + ip + "```";
            json += ",{\"name\":\":earth_americas: IP\",\"value\":\"" + JsonEsc(ipBlock) + "\",\"inline\":true}";
        }

        if (!urlLine.empty()) {
            json += ",{\"name\":\"\\u200b\",\"value\":\"" + JsonEsc(urlLine) + "\",\"inline\":false}";
        }
        json += "]";

        if (!avatarUrl.empty()) {
            json += ",\"thumbnail\":{\"url\":\"" + JsonEsc(avatarUrl) + "\"}";
        }

        std::string sentLine = FormatDiscordTime("TimeWebHook");
        if (!sentLine.empty()) {
            json += ",\"footer\":{\"text\":\"" + JsonEsc(sentLine) + "\"}";
        }

        json += "}]}";

        onJson(std::move(json));
    });
}

static void SendDiscordBotReport(int newReportId,
                                 const std::string& serverName,
                                 const std::string& senderName,  uint64_t senderSid,
                                 const std::string& victimName,  uint64_t victimSid,
                                 const std::string& reason,
                                 int kills, int deaths, bool prime,
                                 const std::string& mapName, const std::string& ip)
{
    if (g_Cfg.discordType != 1) return;
    if (g_Cfg.discordBotToken.empty() || g_Cfg.discordChannelId.empty()) return;
    if (!g_http) return;

    BuildNewReportPayload(newReportId, serverName,
        senderName, senderSid, victimName, victimSid,
        reason, kills, deaths, prime, mapName, ip,
         true,
        [newReportId](std::string json)
        {
            std::string url = "https://discord.com/api/channels/" +
                g_Cfg.discordChannelId + "/messages";
            auto* job = new DiscordHttpJob(k_EHTTPMethodPOST, url, std::move(json),
                [newReportId](bool ok, const std::string& resp) {
                    if (!ok || resp.empty()) return;
                    std::string id = ExtractJsonValue(resp, "id");
                    if (!id.empty()) SetReportMsgId(newReportId, id);
                });
            job->SetHeader("Authorization", "Bot " + g_Cfg.discordBotToken);
            if (!job->Start()) delete job;
        });
}

static void EditDiscordBotMessage(int reportId, const std::string& messageId,
                                  const std::string& description, int color)
{
    if (g_Cfg.discordBotToken.empty() || g_Cfg.discordChannelId.empty()) return;
    if (!g_http || messageId.empty()) return;

    std::string url = "https://discord.com/api/channels/" +
        g_Cfg.discordChannelId + "/messages/" + messageId;

    std::string json = "{\"embeds\":[{";
    json += "\"color\":" + std::to_string(color) + ",";
    json += "\"description\":\"" + JsonEsc(description) + "\"";
    json += "}]}";

    auto* job = new DiscordHttpJob(k_EHTTPMethodPATCH, url, std::move(json), nullptr);
    job->SetHeader("Authorization", "Bot " + g_Cfg.discordBotToken);
    if (!job->Start()) delete job;
}

static void SendDiscordWebhookReport(int newReportId,
                                     const std::string& serverName,
                                     const std::string& senderName,  uint64_t senderSid,
                                     const std::string& victimName,  uint64_t victimSid,
                                     const std::string& reason,
                                     int kills, int deaths,
                                     bool prime,
                                     const std::string& mapName,
                                     const std::string& ip)
{
    if (g_Cfg.discordType != 2 || g_Cfg.discordWebhook.empty()) return;
    if (!g_http) return;

    FetchSteamAvatar(victimSid, [=](std::string avatarUrl)
    {
        float kd = (deaths > 0) ? (float)kills / (float)deaths : (float)kills;

        const char* tpl = Tr("WebHookSendBody",
            "# 🔔 NEW REPORT #%i\n- Sender: %s\n- Offender: %s\n");
        char body[4096];
        snprintf(body, sizeof(body), tpl,
            newReportId,
            senderName.c_str(), std::to_string(senderSid).c_str(),
            victimName.c_str(), std::to_string(victimSid).c_str(),
            reason.c_str(),
            kd, kills, deaths,
            prime ? "✔" : "✖",
            mapName.c_str());

        std::string urlLine;
        if (!g_Cfg.website.empty()) {
            char urlBuf[512];
            const char* urlTpl = Tr("WebHookReportURL", "[Review](%s/%i/%i/)");
            snprintf(urlBuf, sizeof(urlBuf), urlTpl,
                g_Cfg.website.c_str(), g_Cfg.serverId, newReportId);
            urlLine = urlBuf;
        }

        std::string sentLine = FormatDiscordTime("TimeWebHook");

        char snameLine[256];
        char tmpName[200];
        snprintf(tmpName, sizeof(tmpName),
            Tr("WebHookServerName", "Server: %s"), serverName.c_str());
        snprintf(snameLine, sizeof(snameLine), "**%s**", tmpName);

        std::string description = std::string(snameLine) + "\n" + body;
        if (!urlLine.empty()) description += "\n" + urlLine;
        description += "\n" + sentLine;

        std::string json = "{";
        if (!g_Cfg.discordContent.empty())
            json += "\"content\":\"" + JsonEsc(g_Cfg.discordContent) + "\",";
        json += "\"embeds\":[{";
        json += "\"color\":15158332,";
        json += "\"description\":\"" + JsonEsc(description) + "\",";

        char sidStr[32]; snprintf(sidStr, sizeof(sidStr), "%llu", (unsigned long long)victimSid);
        std::string sidBlock = std::string("```") + sidStr + "```";
        json += "\"fields\":[";
        json += "{\"name\":\":id: STEAM ID\",\"value\":\"" + JsonEsc(sidBlock) + "\",\"inline\":true}";
        if (!ip.empty()) {
            std::string ipBlock = std::string("```") + ip + "```";
            json += ",{\"name\":\":earth_americas: IP\",\"value\":\"" + JsonEsc(ipBlock) + "\",\"inline\":true}";
        }
        json += "]";

        if (!avatarUrl.empty()) {
            json += ",\"thumbnail\":{\"url\":\"" + JsonEsc(avatarUrl) + "\"}";
        }
        json += "}]}";

        auto* job = new DiscordHttpJob(k_EHTTPMethodPOST,
            g_Cfg.discordWebhook, std::move(json), nullptr);
        if (!job->Start()) delete job;
    });
}

static std::string StrReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::string UrlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out; out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static void SendVkReport(int newReportId,
                         const std::string& serverName,
                         const std::string& senderName,  uint64_t senderSid,
                         const std::string& victimName,  uint64_t victimSid,
                         const std::string& reason,
                         int kills, int deaths,
                         bool prime,
                         const std::string& mapName,
                         const std::string& ip)
{
    if (g_Cfg.discordType != 3) return;
    if (g_Cfg.vkToken.empty() || g_Cfg.vkPeerId.empty()) return;
    if (!g_http) return;

    float kd = (deaths > 0) ? (float)kills / (float)deaths : (float)kills;
    char kdBuf[16]; snprintf(kdBuf, sizeof(kdBuf), "%.1f", kd);
    char sSidBuf[32]; snprintf(sSidBuf, sizeof(sSidBuf), "%llu", (unsigned long long)senderSid);
    char vSidBuf[32]; snprintf(vSidBuf, sizeof(vSidBuf), "%llu", (unsigned long long)victimSid);

    std::string text = Tr("VKMessageContent", "New report #{report_id}: {intruder_name}");
    text = StrReplaceAll(text, "{report_id}",        std::to_string(newReportId));
    text = StrReplaceAll(text, "{server_name}",      serverName);
    text = StrReplaceAll(text, "{sender_name}",      senderName);
    text = StrReplaceAll(text, "{sender_steamid}",   sSidBuf);
    text = StrReplaceAll(text, "{intruder_name}",    victimName);
    text = StrReplaceAll(text, "{intruder_steamid}", vSidBuf);
    text = StrReplaceAll(text, "{intruder_ip}",      ip);
    text = StrReplaceAll(text, "{reason}",           reason);
    text = StrReplaceAll(text, "{kd_ratio}",         kdBuf);
    text = StrReplaceAll(text, "{kills}",            std::to_string(kills));
    text = StrReplaceAll(text, "{deaths}",           std::to_string(deaths));
    text = StrReplaceAll(text, "{prime}",            prime ? "✔" : "✖");
    text = StrReplaceAll(text, "{map}",              mapName);

    uint64_t randomId = (uint64_t)time(nullptr) * 1000ull + (uint64_t)newReportId;

    char url[2048];
    snprintf(url, sizeof(url),
        "https://api.vk.com/method/messages.send?access_token=%s&peer_id=%s"
        "&random_id=%llu&message=%s&v=5.131",
        g_Cfg.vkToken.c_str(), g_Cfg.vkPeerId.c_str(),
        (unsigned long long)randomId, UrlEncode(text).c_str());

    auto* job = new DiscordHttpJob(k_EHTTPMethodGET, url, "", nullptr);
    if (!job->Start()) delete job;
}

static std::string FormatTime(const char* tplKey)
{
    const char* fmt = Tr(tplKey, "%d.%m %H:%M");
    char buf[128];
    time_t now = time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}

static void SendStatusMessage(int reportId, const char* tplKey,
                              const std::string& authorName,
                              const std::string& authorIconUrl,
                              const std::string& thumbnailUrl,
                              const std::string& timeArg,
                              const std::string& secondArg,
                              int discordColor)
{

    auto buildDescription = [&]() -> std::string {
        char body[1024];
        char idStr[16]; snprintf(idStr, sizeof(idStr), "%d", reportId);
        snprintf(body, sizeof(body),
            Tr(tplKey, "# %s #%s\n- %s\n"),
            idStr, timeArg.c_str(), secondArg.c_str());
        char snameLine[256];
        char tmpName[200];
        snprintf(tmpName, sizeof(tmpName),
            Tr("WebHookServerName", "Server: %s"), g_sHostname.c_str());
        snprintf(snameLine, sizeof(snameLine), "**%s**", tmpName);
        return std::string(snameLine) + "\n" + body;
    };

    auto buildEmbedJson = [&]() -> std::string {
        std::string json = "{\"content\":\"\",\"embeds\":[{";
        json += "\"color\":" + std::to_string(discordColor) + ",";
        if (!authorName.empty()) {
            json += "\"author\":{\"name\":\"" + JsonEsc(authorName) + "\"";
            if (!authorIconUrl.empty())
                json += ",\"icon_url\":\"" + JsonEsc(authorIconUrl) + "\"";
            json += "},";
        }
        json += "\"description\":\"" + JsonEsc(buildDescription()) + "\"";
        if (!thumbnailUrl.empty()) {
            json += ",\"thumbnail\":{\"url\":\"" + JsonEsc(thumbnailUrl) + "\"}";
        }
        json += "}]}";
        return json;
    };

    if (g_Cfg.discordType == 1) {

        std::string mid = GetReportMsgId(reportId);
        if (mid.empty()) return;

        std::string url = "https://discord.com/api/channels/" +
            g_Cfg.discordChannelId + "/messages/" + mid;
        auto* job = new DiscordHttpJob(k_EHTTPMethodPATCH, url,
            buildEmbedJson(), nullptr);
        job->SetHeader("Authorization", "Bot " + g_Cfg.discordBotToken);
        if (!job->Start()) delete job;

        if (!strcmp(tplKey, "WebHookUpdateVerdict")) {
            g_ReportMsgIds.erase(reportId);
            SaveReportMessages();
        }
    }
    else if (g_Cfg.discordType == 2) {
        if (g_Cfg.discordWebhook.empty() || !g_http) return;
        auto* job = new DiscordHttpJob(k_EHTTPMethodPOST,
            g_Cfg.discordWebhook, buildEmbedJson(), nullptr);
        if (!job->Start()) delete job;
    }
    else if (g_Cfg.discordType == 3) {

        if (g_Cfg.vkToken.empty() || g_Cfg.vkPeerId.empty() || !g_http) return;
        char body[1024];
        char idStr[16]; snprintf(idStr, sizeof(idStr), "%d", reportId);
        snprintf(body, sizeof(body),
            Tr(tplKey, "%s #%s\n%s\n"),
            idStr, timeArg.c_str(), secondArg.c_str());

        std::string text;
        if (!authorName.empty()) text = authorName + "\n";
        text += body;

        uint64_t randomId = (uint64_t)time(nullptr) * 1000ull + (uint64_t)reportId;
        char url[2048];
        snprintf(url, sizeof(url),
            "https://api.vk.com/method/messages.send?access_token=%s&peer_id=%s"
            "&random_id=%llu&message=%s&v=5.131",
            g_Cfg.vkToken.c_str(), g_Cfg.vkPeerId.c_str(),
            (unsigned long long)randomId, UrlEncode(text).c_str());
        auto* job = new DiscordHttpJob(k_EHTTPMethodGET, url, "", nullptr);
        if (!job->Start()) delete job;
    }
}

static void PollReportStages()
{
    if (!g_DbConn || !g_DbReady.load()) return;
    if (g_Cfg.discordType != 1 && g_Cfg.discordType != 2 && g_Cfg.discordType != 3) return;
    int sid = g_Cfg.serverId;

    auto fmtTime = [](time_t t) -> std::string {
        if (t <= 0) return "";
        char buf[64];
        struct tm lt; localtime_r(&t, &lt);
        strftime(buf, sizeof(buf), Tr("TimeWebHook2", "%d.%m %H:%M"), &lt);
        return buf;
    };

    {
        char q[512];
        snprintf(q, sizeof(q),
            "SELECT id, IFNULL(name_admin_verdict,''), IFNULL(reason,''), "
            " IFNULL(time_take,0), IFNULL(steamid_admin_verdict,0), "
            " IFNULL(steamid_intruder,0) "
            "FROM rs_reports "
            "WHERE sid=%d AND status=0 AND noty<1 "
            "  AND time_take IS NOT NULL AND time_take<>0 "
            "LIMIT 20", sid);
        g_DbConn->Query(q, [fmtTime](ISQLQuery* qr) {
            if (!qr) return;
            ISQLResult* res = qr->GetResultSet();
            if (!res) return;
            while (res->MoreRows()) {
                res->FetchRow();
                int rid           = res->GetInt(0);
                const char* admin = res->GetString(1);
                const char* reas  = res->GetString(2);
                const char* tStr  = res->GetString(3);
                const char* aSid  = res->GetString(4);
                const char* iSid  = res->GetString(5);
                time_t   tTake     = tStr ? (time_t)atoll(tStr) : 0;
                uint64_t adminSid  = aSid ? (uint64_t)atoll(aSid) : 0;
                uint64_t intrSid   = iSid ? (uint64_t)atoll(iSid) : 0;

                char authorName[256];
                snprintf(authorName, sizeof(authorName),
                    Tr("WebHookAdminTake", "Taken by admin %s"), admin ? admin : "");
                std::string ts   = fmtTime(tTake);
                std::string reas2 = reas ? std::string(reas) : "";
                std::string aname = authorName;

                FetchSteamAvatar(adminSid, [rid, aname, ts, reas2, intrSid](std::string adminAv) {
                    FetchSteamAvatar(intrSid, [rid, aname, ts, reas2, adminAv](std::string intrAv) {
                        SendStatusMessage(rid, "WebHookUpdateTake",
                            aname, adminAv, intrAv, ts, reas2,
                            16753920 );
                    });
                });

                char uq[128];
                snprintf(uq, sizeof(uq),
                    "UPDATE rs_reports SET noty=1 WHERE id=%d", rid);
                g_DbConn->Query(uq, [](ISQLQuery*){});
            }
        });
    }

    {
        char q[512];
        snprintf(q, sizeof(q),
            "SELECT id, IFNULL(verdict,''), IFNULL(name_admin_verdict,''), "
            " IFNULL(time_verdict,0), IFNULL(steamid_admin_verdict,0), "
            " IFNULL(steamid_intruder,0) "
            "FROM rs_reports "
            "WHERE sid=%d AND status=1 AND noty<2 "
            "LIMIT 20", sid);
        g_DbConn->Query(q, [fmtTime](ISQLQuery* qr) {
            if (!qr) return;
            ISQLResult* res = qr->GetResultSet();
            if (!res) return;
            while (res->MoreRows()) {
                res->FetchRow();
                int rid           = res->GetInt(0);
                const char* verd  = res->GetString(1);
                const char* admin = res->GetString(2);
                const char* tStr  = res->GetString(3);
                const char* aSid  = res->GetString(4);
                const char* iSid  = res->GetString(5);
                time_t   tVerdict  = tStr ? (time_t)atoll(tStr) : 0;
                uint64_t adminSid  = aSid ? (uint64_t)atoll(aSid) : 0;
                uint64_t intrSid   = iSid ? (uint64_t)atoll(iSid) : 0;
                if (!verd || !*verd) continue;

                char authorName[256];
                snprintf(authorName, sizeof(authorName),
                    Tr("WebHookAdminReview", "Reviewed by admin %s"), admin ? admin : "");
                std::string ts    = fmtTime(tVerdict);
                std::string verd2 = verd;
                std::string aname = authorName;

                FetchSteamAvatar(adminSid, [rid, aname, ts, verd2, intrSid](std::string adminAv) {
                    FetchSteamAvatar(intrSid, [rid, aname, ts, verd2, adminAv](std::string intrAv) {
                        SendStatusMessage(rid, "WebHookUpdateVerdict",
                            aname, adminAv, intrAv, ts, verd2,
                            3066993 );
                    });
                });

                char uq[128];
                snprintf(uq, sizeof(uq),
                    "UPDATE rs_reports SET noty=2 WHERE id=%d", rid);
                g_DbConn->Query(uq, [](ISQLQuery*){});
            }
        });
    }
}

static void NotifyOnlineAdminsOfNewReport(int reportId, const std::string& victimName)
{
    if (!g_DbReady.load() || !g_DbConn) return;

    int sid = g_Cfg.serverId;
    std::string sName = victimName;
    std::string website = g_Cfg.website;

    char q[256];
    snprintf(q, sizeof(q),
        "SELECT steamid FROM rs_admins WHERE working=1 AND sid=%d", sid);

    g_DbConn->Query(q, [reportId, sid, sName, website](ISQLQuery* qr)
    {
        if (!qr) return;
        ISQLResult* res = qr->GetResultSet();
        if (!res) return;

        std::set<uint64_t> adminSids;
        while (res->MoreRows()) {
            res->FetchRow();
            const char* s = res->GetString(0);
            if (s && *s) {
                try { adminSids.insert(std::stoull(s)); } catch (...) {}
            }
        }

        if (!g_pPlayers) return;
        for (int i = 0; i < 64; ++i) {
            if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
            uint64_t pSid = g_pPlayers->GetSteamID64(i);
            if (!adminSids.count(pSid)) continue;

            char msg[512];
            snprintf(msg, sizeof(msg),
                Tr("AdminInfo", "{PREFIX}New report on %s"), sName.c_str());
            SayRaw(i, msg);

            if (!website.empty()) {
                char url[512];
                snprintf(url, sizeof(url),
                    Tr("AdminInfoUrl", "{PREFIX}Report link: %s/%i/%i"),
                    website.c_str(), sid, reportId);
                SayRaw(i, url);
            }
        }
    });
}

static void SaveChatLogForReport(int victimSlot, int reportId);

static void SubmitReport(int senderSlot, int victimSlot, const std::string& reason)
{
    if (!g_pPlayers) return;
    if (senderSlot < 0 || victimSlot < 0) return;

    uint64_t senderSid = g_pPlayers->GetSteamID64(senderSlot);
    uint64_t victimSid = g_pPlayers->GetSteamID64(victimSlot);
    if (!senderSid || !victimSid) return;

    const char* senderNameP = g_pPlayers->GetPlayerName(senderSlot);
    const char* victimNameP = g_pPlayers->GetPlayerName(victimSlot);
    std::string senderName = (senderNameP && *senderNameP) ? senderNameP : "Unknown";
    std::string victimName = (victimNameP && *victimNameP) ? victimNameP : "Unknown";

    PlayerStats st = GetStats(victimSlot);
    bool primeSender = CheckPrime(senderSid);
    bool primeVictim = CheckPrime(victimSid);
    std::string mapName = GetMapName();
    std::string ip = GetPlayerIP(victimSlot);
    int sid = g_Cfg.serverId;
    time_t now = time(nullptr);

    if (!g_Cfg.testMode) {
        g_LastReportBySender[senderSid]  = now;
        g_LastReportOnVictim[victimSid]  = now;
    }

    if (!g_DbReady.load() || !g_DbConn) {
        SayKey(senderSlot, "SuccessReportSend", "{PREFIX}Report sent (DB offline)");
        return;
    }

    std::string sName = DbEscape(senderName);
    std::string vName = DbEscape(victimName);
    std::string vIp   = DbEscape(ip);
    std::string rsn   = DbEscape(reason);
    std::string mp    = DbEscape(mapName);

    char q[2048];
    snprintf(q, sizeof(q),
        "INSERT INTO rs_reports "
        "(name_intruder, steamid_intruder, ip_intruder, prime_intruder, kills, deaths, "
        " reason, name_sender, steamid_sender, prime_sender, smap, time, status, noty, sid) "
        "VALUES ('%s', %llu, '%s', %d, %d, %d, '%s', '%s', %llu, %d, '%s', %lld, 0, 0, %d)",
        vName.c_str(), (unsigned long long)victimSid, vIp.c_str(), primeVictim ? 1 : 0,
        st.kills, st.deaths,
        rsn.c_str(), sName.c_str(), (unsigned long long)senderSid, primeSender ? 1 : 0,
        mp.c_str(), (long long)now, sid);

    g_DbConn->Query(q, [=](ISQLQuery* qr)
    {
        if (!qr) {
            ConColorMsg(Color(255,0,0,255), "[Reports] INSERT failed (query result null)\n");
            return;
        }
        int newId = (int)qr->GetInsertId();
        if (newId <= 0) {
            ConColorMsg(Color(255,0,0,255), "[Reports] INSERT failed (no insert_id)\n");
            return;
        }

        if (g_Cfg.discordType == 1) {
            SendDiscordBotReport(newId, g_sHostname,
                senderName, senderSid,
                victimName, victimSid,
                reason, st.kills, st.deaths, primeSender,
                mapName, ip);
        } else if (g_Cfg.discordType == 2) {
            SendDiscordWebhookReport(newId, g_sHostname,
                senderName, senderSid,
                victimName, victimSid,
                reason, st.kills, st.deaths, primeSender,
                mapName, ip);
        } else if (g_Cfg.discordType == 3) {
            SendVkReport(newId, g_sHostname,
                senderName, senderSid,
                victimName, victimSid,
                reason, st.kills, st.deaths, primeSender,
                mapName, ip);
        }

        NotifyOnlineAdminsOfNewReport(newId, victimName);

        SaveChatLogForReport(victimSlot, newId);

        SayKey(senderSlot, "SuccessReportSend", "{PREFIX}Report sent successfully");

        if (!g_Cfg.website.empty()) {
            char idBuf[16];
            snprintf(idBuf, sizeof(idBuf), "%d", newId);
            char url[512];
            snprintf(url, sizeof(url),
                Tr("ReportUrl", "{PREFIX}Report link: %s/%i/%s/"),
                g_Cfg.website.c_str(), sid, idBuf);
            SayRaw(senderSlot, url);
        }
    });
}

static void ShowMainMenu(int slot);
static void ShowPlayerSelectMenu(int slot);
static void ShowReasonsMenu(int slot, int victimSlot);
static void ShowMyReportsMenu(int slot);
static void ShowMyReportsList(int slot, int status );
static void ShowMyReportDetail(int slot, int reportId, int status);

static void BuildMainMenu(int slot, int warnsCount);

static void ShowMainMenu(int slot)
{
    if (!g_pMenus || !g_pPlayers) return;

    uint64_t sid = g_pPlayers->GetSteamID64(slot);
    if (!sid || !g_DbConn || !g_DbReady.load()) {

        BuildMainMenu(slot, 0);
        return;
    }

    int serverId = g_Cfg.serverId;
    char q[256];
    snprintf(q, sizeof(q),
        "SELECT COUNT(*) FROM rs_warns WHERE steamid=%llu AND sid=%d",
        (unsigned long long)sid, serverId);

    g_DbConn->Query(q, [slot](ISQLQuery* qr) {
        int warns = 0;
        if (qr) {
            if (ISQLResult* res = qr->GetResultSet()) {
                if (res->MoreRows()) {
                    res->FetchRow();
                    warns = res->GetInt(0);
                }
            }
        }
        BuildMainMenu(slot, warns);
    });
}

static void BuildMainMenu(int slot, int warnsCount)
{
    if (!g_pMenus) return;

    Menu hMenu;
    char title[128];
    snprintf(title, sizeof(title),
        Tr("MenuReportTitle", "Reports<br>Warns: %i"), warnsCount);
    hMenu.szTitle = title;

    g_pMenus->AddItemMenu(hMenu, "send", Tr("MenuReportSend", "Send report"));
    g_pMenus->AddItemMenu(hMenu, "my",   Tr("MenuReportMy",   "My reports"));

    g_pMenus->SetExitMenu(hMenu, true);

    g_pMenus->SetCallback(hMenu,
        [](const char* szBack, const char* szText, int iItem, int iSlot)
        {
            if (!szBack || !*szBack) return;
            if (!strcmp(szBack, "send")) {

                if (g_pPlayers && !g_Cfg.testMode) {
                    uint64_t sid = g_pPlayers->GetSteamID64(iSlot);
                    int left = RemainingSenderCooldown(sid);
                    if (left > 0) {
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            Tr("Cooldown", "{PREFIX}Wait %i sec"), left);
                        SayRaw(iSlot, buf);
                        return;
                    }
                }
                ShowPlayerSelectMenu(iSlot);
            } else if (!strcmp(szBack, "my")) {
                ShowMyReportsMenu(iSlot);
            }
        });

    g_pMenus->DisplayPlayerMenu(hMenu, slot, true, true);
}

static void ShowPlayerSelectMenu(int senderSlot)
{
    if (!g_pMenus || !g_pPlayers) return;

    Menu hMenu;
    hMenu.szTitle = Tr("PlayerListTitle", "Select player");

    bool any = false;
    for (int i = 0; i < 64; ++i) {

        if (!g_Cfg.testMode && i == senderSlot) continue;
        if (!g_pPlayers->IsConnected(i) || g_pPlayers->IsFakeClient(i)) continue;
        if (!g_pPlayers->IsInGame(i)) continue;

        const char* nm = g_pPlayers->GetPlayerName(i);
        if (!nm || !*nm) nm = "Unknown";

        char key[16]; snprintf(key, sizeof(key), "p%d", i);

        if (g_Cfg.testMode && i == senderSlot) {
            char label[128];
            snprintf(label, sizeof(label), "[TEST] %s", nm);
            g_pMenus->AddItemMenu(hMenu, key, label);
        } else {
            g_pMenus->AddItemMenu(hMenu, key, nm);
        }
        any = true;
    }

    if (!any) {
        SayKey(senderSlot, "PlayerListEmpty", "{PREFIX}No available players");
        return;
    }

    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, true);

    g_pMenus->SetCallback(hMenu,
        [](const char* szBack, const char* szText, int iItem, int iSlot)
        {
            if (!szBack || !szBack[0] || !strcmp(szBack, "back")) {

                ShowMainMenu(iSlot);
                return;
            }
            if (szBack[0] != 'p') return;
            int victim = atoi(szBack + 1);
            if (victim < 0 || victim >= 64) return;

            if (g_pPlayers) {
                if (!g_Cfg.testMode) {
                    uint64_t vsid = g_pPlayers->GetSteamID64(victim);
                    int left = RemainingVictimCooldown(vsid);
                    if (left > 0) {
                        char b[256];
                        snprintf(b, sizeof(b),
                            Tr("CooldownPlayer", "{PREFIX}Player report cooldown: %i sec"), left);
                        SayRaw(iSlot, b);
                        return;
                    }
                }

                if (!g_pPlayers->IsConnected(victim)) {
                    SayKey(iSlot, "PlayerLeave", "{PREFIX}Player left server");
                    return;
                }
            }
            ShowReasonsMenu(iSlot, victim);
        });

    g_pMenus->DisplayPlayerMenu(hMenu, senderSlot, true, true);
}

static void ShowReasonsMenu(int senderSlot, int victimSlot)
{
    if (!g_pMenus) return;
    if (g_Cfg.reasons.empty()) {
        SayKey(senderSlot, "ReasonsEmpty", "{PREFIX}No reasons configured");
        return;
    }

    Menu hMenu;
    hMenu.szTitle = Tr("ReasonsTitle", "Select reason");

    for (size_t i = 0; i < g_Cfg.reasons.size(); ++i) {
        char key[16]; snprintf(key, sizeof(key), "r%zu_%d", i, victimSlot);
        g_pMenus->AddItemMenu(hMenu, key, g_Cfg.reasons[i].c_str());
    }

    {
        char key[16]; snprintf(key, sizeof(key), "own_%d", victimSlot);
        g_pMenus->AddItemMenu(hMenu, key, Tr("ReasonsOwn", "Custom reason"));
    }

    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, true);

    g_pMenus->SetCallback(hMenu,
        [](const char* szBack, const char* szText, int iItem, int iSlot)
        {
            if (!szBack || !szBack[0] || !strcmp(szBack, "back")) {

                ShowPlayerSelectMenu(iSlot);
                return;
            }

            if (!strncmp(szBack, "own_", 4)) {
                int victim = atoi(szBack + 4);
                if (victim < 0 || victim >= 64) return;
                if (!g_pPlayers || !g_pPlayers->IsConnected(victim)) {
                    SayKey(iSlot, "PlayerLeave", "{PREFIX}Player left server");
                    return;
                }

                g_PendingCustom[iSlot] = { victim, time(nullptr) + 60 };
                char buf[256];
                snprintf(buf, sizeof(buf),
                    Tr("ReasonsOwnChat", "{PREFIX}Type your reason using %s <text>"),
                    g_Cfg.commandReason.c_str());
                SayRaw(iSlot, buf);
                if (g_pMenus) g_pMenus->ClosePlayerMenu(iSlot);
                return;
            }

            if (szBack[0] != 'r') return;
            int reasonIdx = -1, victim = -1;
            if (sscanf(szBack + 1, "%d_%d", &reasonIdx, &victim) != 2) return;
            if (reasonIdx < 0 || reasonIdx >= (int)g_Cfg.reasons.size()) return;
            if (victim < 0 || victim >= 64) return;
            if (!g_pPlayers || !g_pPlayers->IsConnected(victim)) {
                SayKey(iSlot, "PlayerLeave", "{PREFIX}Player left server");
                return;
            }
            SubmitReport(iSlot, victim, g_Cfg.reasons[reasonIdx]);

            if (g_pMenus) g_pMenus->ClosePlayerMenu(iSlot);
        });

    g_pMenus->DisplayPlayerMenu(hMenu, senderSlot, true, true);
}

static bool OnCustomReasonCommand(int slot, const char* args)
{
    if (slot < 0) return true;
    if (!g_pPlayers || !g_pPlayers->IsConnected(slot)) return true;

    auto it = g_PendingCustom.find(slot);
    if (it == g_PendingCustom.end()) {

        return true;
    }
    if (time(nullptr) > it->second.deadline) {
        g_PendingCustom.erase(it);
        return true;
    }
    int victim = it->second.victimSlot;
    g_PendingCustom.erase(it);

    if (!args) {
        SayKey(slot, "ReasonsEmpty", "{PREFIX}You cannot send report without reason");
        return true;
    }

    std::string s(args);

    while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='"'))
        s.erase(s.begin());

    while (!s.empty() && (s.back() ==' '||s.back() =='\t'||s.back() =='"'||s.back()=='\r'||s.back()=='\n'))
        s.pop_back();

    if (!g_Cfg.commandReason.empty()) {
        const std::string& cmd = g_Cfg.commandReason;

        std::string cmdNoBang = (!cmd.empty() && cmd.front()=='!') ? cmd.substr(1) : cmd;
        auto stripPrefix = [&](const std::string& pfx) {
            if (s.size() >= pfx.size() && !strncasecmp(s.c_str(), pfx.c_str(), pfx.size())) {
                size_t cut = pfx.size();
                while (cut < s.size() && (s[cut]==' '||s[cut]=='\t')) ++cut;
                s.erase(0, cut);
            }
        };
        stripPrefix(cmd);
        stripPrefix(cmdNoBang);

        while (!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='"'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() ==' '||s.back() =='\t'||s.back() =='"'))
            s.pop_back();
    }

    if (s.empty()) {
        SayKey(slot, "ReasonsEmpty", "{PREFIX}You cannot send report without reason");
        return true;
    }
    if (!g_pPlayers->IsConnected(victim)) {
        SayKey(slot, "PlayerLeave", "{PREFIX}Player left server");
        return true;
    }
    SubmitReport(slot, victim, s);
    return true;
}

static void ShowMyReportsMenu(int slot)
{
    if (!g_pMenus) return;

    Menu hMenu;

    hMenu.szTitle = Tr("MenuReportMy", "My reports");

    g_pMenus->AddItemMenu(hMenu, "open",   Tr("MyReportsOpen",   "Open"));
    g_pMenus->AddItemMenu(hMenu, "closed", Tr("MyReportsClosed", "Closed"));
    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, true);

    g_pMenus->SetCallback(hMenu,
        [](const char* szBack, const char* szText, int iItem, int iSlot)
        {
            if (szBack) {
                if (!strcmp(szBack, "open"))         { ShowMyReportsList(iSlot, 0); return; }
                if (!strcmp(szBack, "closed"))       { ShowMyReportsList(iSlot, 1); return; }
                if (!strcmp(szBack, "back") || !szBack[0]) { ShowMainMenu(iSlot); return; }
            }
            ShowMainMenu(iSlot);
        });

    g_pMenus->DisplayPlayerMenu(hMenu, slot, true, true);
}

static void BuildMyReportsListMenu(int slot, int status)
{
    if (!g_pMenus) return;

    auto& cache = (status == 0) ? g_MyOpenCache[slot] : g_MyClosedCache[slot];

    Menu hMenu;
    hMenu.szTitle = (status == 0)
        ? Tr("MenuMyReportsOpenTitle",   "My open reports")
        : Tr("MenuMyReportsClosedTitle", "My closed reports");

    if (cache.empty()) {
        const char* emptyText = (status == 0)
            ? Tr("MenuReportAdminItemEmpty",        "No open reports")
            : Tr("MenuReportAdminItemEmptyClosed",  "No closed reports");
        g_pMenus->AddItemMenu(hMenu, "empty", emptyText);
    } else {
        for (const auto& r : cache) {
            char label[256];
            snprintf(label, sizeof(label),
                Tr("MenuReportAdminItem", "On %s"), r.victimName.c_str());
            char key[24];
            snprintf(key, sizeof(key), "rep_%d_%d", r.id, status);
            g_pMenus->AddItemMenu(hMenu, key, label);
        }
    }

    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, true);

    g_pMenus->SetCallback(hMenu,
        [](const char* szBack, const char* szText, int iItem, int iSlot)
        {
            if (!szBack) { ShowMyReportsMenu(iSlot); return; }
            if (!strcmp(szBack, "back") || !szBack[0]) {
                ShowMyReportsMenu(iSlot);
                return;
            }
            if (!strncmp(szBack, "rep_", 4)) {
                int reportId = -1, st = 0;
                if (sscanf(szBack + 4, "%d_%d", &reportId, &st) != 2) return;
                ShowMyReportDetail(iSlot, reportId, st);
                return;
            }

            ShowMyReportsMenu(iSlot);
        });

    g_pMenus->DisplayPlayerMenu(hMenu, slot, true, true);
}

static void ShowMyReportsList(int slot, int status)
{
    if (!g_pPlayers) return;
    uint64_t sid = g_pPlayers->GetSteamID64(slot);
    if (!sid) return;

    if (!g_DbReady.load() || !g_DbConn) {
        SayRaw(slot, "{PREFIX}Database is offline");
        return;
    }

    int serverId = g_Cfg.serverId;

    char q[512];
    snprintf(q, sizeof(q),
        "SELECT id, name_intruder, reason, time, status, "
        " IFNULL(verdict,''), IFNULL(name_admin_verdict,''), IFNULL(time_verdict,0) "
        "FROM rs_reports "
        "WHERE steamid_sender=%llu AND sid=%d AND status=%d "
        "ORDER BY time DESC LIMIT 30",
        (unsigned long long)sid, serverId, status);

    g_DbConn->Query(q, [slot, status](ISQLQuery* qr)
    {
        if (!qr) return;
        ISQLResult* res = qr->GetResultSet();
        if (!res) return;

        std::vector<MyReport> list;
        while (res->MoreRows()) {
            res->FetchRow();
            MyReport r{};
            r.id          = res->GetInt(0);
            const char* s = res->GetString(1); r.victimName  = s ? s : "";
            s = res->GetString(2);             r.reason      = s ? s : "";
            s = res->GetString(3);             r.sentTime    = s ? (time_t)atoll(s) : 0;
            r.status      = res->GetInt(4);
            s = res->GetString(5);             r.verdict     = s ? s : "";
            s = res->GetString(6);             r.adminName   = s ? s : "";
            s = res->GetString(7);             r.verdictTime = s ? (time_t)atoll(s) : 0;
            list.push_back(std::move(r));
        }

        if (status == 0) g_MyOpenCache[slot]   = std::move(list);
        else             g_MyClosedCache[slot] = std::move(list);
        BuildMyReportsListMenu(slot, status);
    });
}

static void ShowMyReportDetail(int slot, int reportId, int status)
{
    if (!g_pMenus) return;

    const auto& cache = (status == 0) ? g_MyOpenCache[slot] : g_MyClosedCache[slot];
    const MyReport* r = nullptr;
    for (const auto& it : cache) if (it.id == reportId) { r = &it; break; }
    if (!r) {
        SayRaw(slot, "{PREFIX}Report not found");
        return;
    }

    Menu hMenu;

    if (status == 0)
        hMenu.szTitle = Tr("MenuReportShowTitle", "Open report");
    else
        hMenu.szTitle = (g_pUtils && !strcmp(g_pUtils->GetLanguage(), "ru"))
            ? "Закрытый репорт"
            : "Closed report";

    char buf[256];
    snprintf(buf, sizeof(buf),
        Tr("MenuReportShowItemTarget", "Offender: %s"), r->victimName.c_str());
    g_pMenus->AddItemMenu(hMenu, "i1", buf);

    snprintf(buf, sizeof(buf),
        Tr("MenuReportShowItemReason", "Reason: %s"), r->reason.c_str());
    g_pMenus->AddItemMenu(hMenu, "i2", buf);

    if (status == 0) {

        g_pMenus->AddItemMenu(hMenu, "i3",
            Tr("MenuMyReportsOpenStatus", "Status: waiting for response"));
    } else {

        snprintf(buf, sizeof(buf),
            Tr("MenuMyReportsVerdict", "Verdict: %s"), r->verdict.c_str());
        g_pMenus->AddItemMenu(hMenu, "i3", buf);

        if (!r->adminName.empty()) {
            snprintf(buf, sizeof(buf),
                Tr("MenuMyReportsAdmin", "Admin: %s"), r->adminName.c_str());
            g_pMenus->AddItemMenu(hMenu, "i4", buf);
        }

        if (r->verdictTime > 0) {
            struct tm lt; time_t t = r->verdictTime; localtime_r(&t, &lt);
            snprintf(buf, sizeof(buf),
                Tr("MenuMyReportsClosed", "Closed: %02d-%02d, %02d:%02d"),
                lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min);
            g_pMenus->AddItemMenu(hMenu, "i5", buf);
        }
    }

    g_pMenus->SetExitMenu(hMenu, true);
    g_pMenus->SetBackMenu(hMenu, true);

    int capturedStatus = status;
    g_pMenus->SetCallback(hMenu,
        [capturedStatus](const char* szBack, const char* szText, int iItem, int iSlot)
        {

            ShowMyReportsList(iSlot, capturedStatus);
        });

    g_pMenus->DisplayPlayerMenu(hMenu, slot, true, true);
}

static bool OnReportCommand(int slot, const char* args)
{
    if (slot < 0) return true;
    if (!g_pPlayers || !g_pPlayers->IsConnected(slot)) return true;
    ShowMainMenu(slot);
    return true;
}

SH_DECL_HOOK0_void(IServerGameDLL, GameServerSteamAPIActivated, SH_NOATTRIB, 0);

void Reports::OnGameServerSteamAPIActivated()
{
    g_http = SteamGameServerHTTP();
    RETURN_META(MRES_IGNORED);
}

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr;
}

static bool g_bCommandsRegistered = false;

static void OnPlayerDisconnectEvt(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
    int slot = pEvent ? pEvent->GetInt("userid") : -1;
    if (slot < 0 || slot >= 64) return;
    g_PendingCustom.erase(slot);
    g_MyOpenCache.erase(slot);
    g_MyClosedCache.erase(slot);
    g_ChatHistory[slot].clear();
}

static bool OnChatListener(int slot, const char* content, bool bMute, bool bTeam)
{
    if (slot < 0 || slot >= 64 || !content || !*content) return true;
    if (!g_pPlayers) return true;
    if (!g_pPlayers->IsConnected(slot) || g_pPlayers->IsFakeClient(slot)) return true;

    ChatMessage m;
    const char* nm = g_pPlayers->GetPlayerName(slot);
    m.name    = (nm && *nm) ? nm : "Unknown";
    m.steamid = g_pPlayers->GetSteamID64(slot);
    m.message = content;
    m.time    = time(nullptr);

    auto& dq = g_ChatHistory[slot];
    dq.push_back(std::move(m));
    int cap = g_Cfg.countMessages > 0 ? g_Cfg.countMessages : 15;
    while ((int)dq.size() > cap) dq.pop_front();
    return true;
}

static void SaveChatLogForReport(int victimSlot, int reportId)
{
    if (victimSlot < 0 || victimSlot >= 64) return;
    if (!g_DbConn || !g_DbReady.load()) return;
    if (reportId <= 0) return;

    const auto& dq = g_ChatHistory[victimSlot];
    if (dq.empty()) return;

    std::string sql = "INSERT INTO rs_chatlogging (name, steamid, message, time, rid) VALUES ";
    bool first = true;
    for (const auto& m : dq)
    {

        std::string msg = m.message;
        if (msg.size() > 250) msg.resize(250);

        std::string en = DbEscape(m.name);
        std::string em = DbEscape(msg);

        char chunk[1024];
        snprintf(chunk, sizeof(chunk),
            "%s('%s', %llu, '%s', %lld, %d)",
            first ? "" : ",",
            en.c_str(),
            (unsigned long long)m.steamid,
            em.c_str(),
            (long long)m.time,
            reportId);
        sql += chunk;
        first = false;
    }
    g_DbConn->Query(sql.c_str(), [](ISQLQuery*){});
}

void OnStartupServer()
{
    g_pGameEntitySystem = GameEntitySystem();
    gpGlobals           = g_pUtils->GetCGlobalVars();
    g_pEntitySystem     = g_pUtils->GetCEntitySystem();

    LoadConfig();
    LoadPhrases();
    RefreshHostname();
    LoadReportMessages();

    if (!g_DbConn) DbConnect();

    if (g_pUtils && g_Cfg.pollInterval > 0) {
        float iv = (float)g_Cfg.pollInterval;
        g_pUtils->CreateTimer(iv, [iv]() -> float {
            PollReportStages();
            return iv;
        });
    }

    if (!g_bCommandsRegistered) {

        g_pUtils->RegCommand(g_PLID,
            { "mm_reports" }, g_Cfg.commands, OnReportCommand);

        if (!g_Cfg.commandReason.empty()) {
            g_pUtils->RegCommand(g_PLID,
                std::vector<std::string>{},
                std::vector<std::string>{ g_Cfg.commandReason },
                OnCustomReasonCommand);
        }
        g_bCommandsRegistered = true;
    }
}

bool Reports::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY    (GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

    SH_ADD_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server,
        SH_MEMBER(this, &Reports::OnGameServerSteamAPIActivated), false);

    g_SMAPI->AddListener(this, this);

    if (!g_http) g_http = SteamGameServerHTTP();

    return true;
}

bool Reports::Unload(char* error, size_t maxlen)
{
    SH_REMOVE_HOOK(IServerGameDLL, GameServerSteamAPIActivated, g_pSource2Server,
        SH_MEMBER(this, &Reports::OnGameServerSteamAPIActivated), false);

    if (g_pUtils) g_pUtils->ClearAllHooks(g_PLID);

    if (g_DbConn) { g_DbConn->Destroy(); g_DbConn = nullptr; }
    g_DbReady = false;

    ConVar_Unregister();
    return true;
}

void Reports::AllPluginsLoaded()
{
    int ret;
    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) { SetFailState("Missing Utils plugin"); return; }

    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) { SetFailState("Missing Players plugin"); return; }

    g_pMenus   = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) { SetFailState("Missing Menus plugin"); return; }

    g_pSQL = (ISQLInterface*)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, NULL);
    if (ret == META_IFACE_FAILED) { SetFailState("Missing sql_mm plugin"); return; }

    g_pUtils->StartupServer(g_PLID, OnStartupServer);

    g_pUtils->HookEvent(g_PLID, "player_disconnect", OnPlayerDisconnectEvt);

    g_pUtils->AddChatListenerPost(g_PLID, OnChatListener);
}

void Reports::SetFailState(const char* error)
{
    if (g_pUtils) g_pUtils->ErrorLog("%s %s\n", GetLogTag(), error);
    else ConColorMsg(Color(255,0,0,255), "[%s] %s\n", GetLogTag(), error);
    std::string cmd = "meta unload " + std::to_string(g_PLID);
    engine->ServerCommand(cmd.c_str());
}

const char *Reports::GetLicense()
{
    return "Public";
}

const char *Reports::GetVersion()
{
    return "1.0.0";
}

const char *Reports::GetDate()
{
    return __DATE__;
}

const char *Reports::GetLogTag()
{
    return "[Reports]";
}

const char *Reports::GetAuthor()
{
    return "_ded_cookies";
}

const char *Reports::GetDescription()
{
    return "Reports";
}

const char *Reports::GetName()
{
    return "Reports";
}

const char *Reports::GetURL()
{
    return "https://api.onlypublic.net/";
}