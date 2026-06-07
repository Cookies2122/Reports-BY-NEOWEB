#ifndef _INCLUDE_REPORTS_H_
#define _INCLUDE_REPORTS_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include <iserver.h>
#include <entity2/entitysystem.h>
#include <steam/steam_gameserver.h>
#include "igameevents.h"
#include "utlvector.h"
#include "ehandle.h"
#include "vector.h"
#include "CCSPlayerController.h"
#include "module.h"
#include <KeyValues.h>

#include "schemasystem/schemasystem.h"
#include "include/menus.h"
#include "include/sql_mm.h"
#include "include/mysql_mm.h"

#include "nlohmann/json.hpp"
using json = nlohmann::json;

using namespace DynLibUtils;

class Reports final : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;
    void AllPluginsLoaded() override;
    void SetFailState(const char* error);

private:
    void OnGameServerSteamAPIActivated();

public:
    const char* GetAuthor() override;
    const char* GetName() override;
    const char* GetDescription() override;
    const char* GetURL() override;
    const char* GetLicense() override;
    const char* GetVersion() override;
    const char* GetDate() override;
    const char* GetLogTag() override;
};

extern Reports g_Reports;
PLUGIN_GLOBALVARS();

#endif
