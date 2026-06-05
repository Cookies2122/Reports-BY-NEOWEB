#pragma once

#define DISCORD_WEBHOOK_INTERFACE "IDiscordWebhookApi"
#define DISCORD_BOT_INTERFACE "IDiscordBotApi"

class Embed
{
public:
    void SetAuthor(const char* szName, const char* szURL = nullptr, const char* szIcon = nullptr) {
        m_szAuthorName = szName;
        m_szAuthorURL = szURL;
        m_szAuthorIcon = szIcon;
    }
    void SetTitle(const char* szTitle) {
        m_szTitle = szTitle;
    }
    void SetDescription(const char* szDescription) {
        m_szDescription = szDescription;
    }
    void SetURL(const char* szURL) {
        m_szURL = szURL;
    }
    void SetColor(int iColor) {
        m_iColor = iColor;
    }

    void AddField(std::string szTitle, std::string szValue, bool bInline = false) {
        m_hFields.push_back(std::make_tuple(szTitle, szValue, bInline));
    }

    void SetImage(const char* szURL) {
        m_szImage = szURL;
    }
    void SetThumbnail(const char* szURL) {
        m_szThumbnail = szURL;
    }

    void SetFooter(const char* szText, const char* szIcon = nullptr) {
        m_szFooterText = szText;
        m_szFooterIcon = szIcon;
    }

    const char* GetAuthorName() const {
        if (m_szAuthorName.empty()) {
            return nullptr;
        }
        return m_szAuthorName.c_str();
    }
    const char* GetAuthorURL() const {
        if (m_szAuthorURL.empty()) {
            return nullptr;
        }
        return m_szAuthorURL.c_str();
    }
    const char* GetAuthorIcon() const {
        if (m_szAuthorIcon.empty()) {
            return nullptr;
        }
        return m_szAuthorIcon.c_str();
    }

    const char* GetTitle() const {
        if (m_szTitle.empty()) {
            return nullptr;
        }
        return m_szTitle.c_str();
    }
    const char* GetDescription() const {
        if (m_szDescription.empty()) {
            return nullptr;
        }
        return m_szDescription.c_str();
    }
    const char* GetURL() const {
        if (m_szURL.empty()) {
            return nullptr;
        }
        return m_szURL.c_str();
    }
    int GetColor() const {
        return m_iColor;
    }

    const std::vector<std::tuple<std::string, std::string, bool>>& GetFields() const {
        return m_hFields;
    }

    const char* GetImage() const {
        if (m_szImage.empty()) {
            return nullptr;
        }
        return m_szImage.c_str();
    }
    const char* GetThumbnail() const {
        if (m_szThumbnail.empty()) {
            return nullptr;
        }
        return m_szThumbnail.c_str();
    }

    const char* GetFooterText() const {
        if (m_szFooterText.empty()) {
            return nullptr;
        }
        return m_szFooterText.c_str();
    }
    const char* GetFooterIcon() const {
        if (m_szFooterIcon.empty()) {
            return nullptr;
        }
        return m_szFooterIcon.c_str();
    }
private:
    std::string m_szAuthorName;
    std::string m_szAuthorURL;
    std::string m_szAuthorIcon;

    std::string m_szTitle;
    std::string m_szDescription;
    std::string m_szURL;
    int m_iColor = 0xFFFFFF;

    std::vector<std::tuple<std::string, std::string, bool>> m_hFields;

    std::string m_szImage;
    std::string m_szThumbnail;

    std::string m_szFooterText;
    std::string m_szFooterIcon;
};

class IDiscordWebhookApi
{
public:
    virtual void SendWebHook(const char* szWebHook, const char* szContent, std::vector<Embed*> hEmbeds) = 0;
};

class DiscordBot
{
public:
    explicit DiscordBot(const std::string& token)
        : m_token(token) {}

    const std::string& GetToken() const { return m_token; }
private:
    std::string m_token;
};

typedef std::function<void(int iStatusCode, const char* szResponse)> DiscordCallback;

class IDiscordBotApi
{
public:
    virtual void SendMessage(DiscordBot* pBot, const char* szChannelID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback) = 0;
    virtual void DeleteMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback) = 0;
    virtual void EditMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* szContent, std::vector<Embed*> hEmbeds, DiscordCallback callback) = 0;
    virtual void PinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback) = 0;
    virtual void UnpinMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback) = 0;
    
    virtual void GetMessage(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, DiscordCallback callback) = 0;
    virtual void GetMessages(DiscordBot* pBot, const char* szChannelID, int iLimit, const char* szBefore, const char* szAfter, DiscordCallback callback) = 0;
    virtual void GetPinnedMessages(DiscordBot* pBot, const char* szChannelID, DiscordCallback callback) = 0;

    virtual void AddReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback) = 0;
    virtual void RemoveReaction(DiscordBot* pBot, const char* szChannelID, const char* szMessageID, const char* emoji, DiscordCallback callback) = 0;

    virtual void AddRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback) = 0;
    virtual void RemoveRole(DiscordBot* pBot, const char* szGuildID, const char* szUserID, const char* szRoleID, DiscordCallback callback) = 0;

    virtual void GetGuildMember(DiscordBot* pBot, const char* szGuildID, const char* szUserID, DiscordCallback callback) = 0;
    virtual void GetGuildMembers(DiscordBot* pBot, const char* szGuildID, int iLimit, const char* szAfter, DiscordCallback callback) = 0;
    virtual void GetGuildRoles(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback) = 0;
    virtual void GetGuildChannels(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback) = 0;
    virtual void GetGuildEmojis(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback) = 0;
    virtual void GetGuildInvites(DiscordBot* pBot, const char* szGuildID, DiscordCallback callback) = 0;
};