#pragma once

#include <functional>
#include <string>

#define SHOP_INTERFACE "IShopApi"

struct Currency
{
	int iID;
	std::string szName;
	std::string szNameRP;
	std::string szIdentity;
	int iDefValue;
};

enum ItemType
{
    Item_None = 0,
    Item_Togglable,
    Item_Usable,
    Item_BuyOnly
};

struct ItemPrice
{
    int iPrice;
    int iSellPrice;
    Currency currency;  
};

enum ItemState
{
	ItemState_None = 0,
	ItemState_Enabled,
	ItemState_Disabled
};

enum CurrencyType
{
	ItemBuy = 0,
	ItemSell,
	ItemProlong,
	Native,
	Transfer,
	Admin
};
	

typedef std::function<void()> OnCoreIsReady;
typedef std::function<void(int iSlot)> FunctionsCallback;
typedef std::function<bool(int iSlot, int& iCount, const char* szCurrency, const char* szReason, CurrencyType type)> GiveCurrencyCallback;
typedef std::function<bool(int iSlot, int& iCount, const char* szCurrency, const char* szReason, CurrencyType type)> TakeCurrencyCallback;
typedef std::function<bool(int iSlot, int& iCount, const char* szCurrency, const char* szReason, CurrencyType type)> SetCurrencyCallback;
typedef std::function<bool(int iSlot, const char* szCategory, const char* szItem, const char* szCurrency, int &iPrice)> BuyItemCallback;
typedef std::function<bool(int iSlot, const char* szCategory, const char* szItem, const char* szCurrency, int &iSellPrice)> SellItemCallback;
typedef std::function<bool(int iSlot, const char* szCategory, const char* szItem)> UseItemCallback;
typedef std::function<bool(int iSlot, const char* szCategory, const char* szItem, ItemState oldStatus, ItemState &newState)> ItemCallbackToggle;
typedef std::function<void(int iSlot, const char* szCategory, const char* szItem)> ItemPreviewCallback;

class IMySQLConnection;

class IShopApi
{
public:
    virtual void HookOnCoreIsReady(SourceMM::PluginId id, OnCoreIsReady fn) = 0;
    
    virtual bool CoreIsLoaded() = 0;
    virtual void RegisterCategory(const char* szIdentity, const char* szName) = 0;
	virtual void RegisterItem(const char* szCategory, const char* szIdentity, const char* szName, std::vector<ItemPrice> vecPrices, ItemType type, int iCount = 1, bool bHide = false) = 0;
	virtual void HookUseItem(const char* szCategory, const char* szIdentity, UseItemCallback fn) = 0;
	virtual void HookToggleItem(const char* szCategory, const char* szIdentity, ItemCallbackToggle fn) = 0;
	virtual void HookBuyItem(const char* szCategory, const char* szIdentity, BuyItemCallback fn) = 0;
	virtual void HookSellItem(const char* szCategory, const char* szIdentity, SellItemCallback fn) = 0;
	virtual std::map<std::string, Currency> GetCurrencies() = 0;
	virtual void RegisterFunction(const char* szIdentity, FunctionsCallback fn) = 0;
	virtual void HookGuveCurrency(SourceMM::PluginId id, GiveCurrencyCallback fn) = 0;
	virtual void HookTakeCurrency(SourceMM::PluginId id, TakeCurrencyCallback fn) = 0;
	virtual void HookSetCurrency(SourceMM::PluginId id, SetCurrencyCallback fn) = 0;
	virtual void GiveClientCurrency(int iClient, int iAmount, const char* szCurrency, const char* szReason, CurrencyType type, bool bNotify = true) = 0;
	virtual void TakeClientCurrency(int iClient, int iAmount, const char* szCurrency, const char* szReason, CurrencyType type, bool bNotify = true) = 0;
	virtual void SetClientCurrency(int iClient, int iAmount, const char* szCurrency, const char* szReason, CurrencyType type, bool bNotify = true) = 0;
	virtual int GetClientCurrency(int iClient, const char* szCurrency) = 0;
	
	virtual void GiveClientItem(int iClient, const char* szItem, const char* szCategory, int iCount) = 0;
	virtual void TakeClientItem(int iClient, const char* szItem, const char* szCategory, int iCount) = 0;
	virtual bool UseClientItem(int iClient, const char* szItem, const char* szCategory) = 0;
	virtual bool IsClientItemToggled(int iClient, const char* szItem, const char* szCategory) = 0;

	virtual int GetItemPrice(const char* szItem, const char* szCategory, const char* szCurrency) = 0;
	virtual int GetItemSellPrice(const char* szItem, const char* szCategory, const char* szCurrency) = 0;
	virtual int GetItemCount(const char* szItem, const char* szCategory) = 0;
	virtual ItemType GetItemType(const char* szItem, const char* szCategory) = 0;

	virtual int GetClientItemCount(int iClient, const char* szItem, const char* szCategory) = 0;
	virtual void SetClientItemCount(int iClient, const char* szItem, const char* szCategory, int iCount) = 0;
	virtual Currency GetCurrencyByID(int iID) = 0;
	virtual Currency GetCurrencyByIdentity(const char* szIdentity) = 0;

	virtual bool IsClientHasItem(int iClient, const char* szItem, const char* szCategory) = 0;
	virtual void ChangeToggleItemState(int iClient, const char* szItem, const char* szCategory, ItemState state) = 0;
	virtual std::string GetItemName(const char* szItem, const char* szCategory) = 0;
	virtual void ShowCategoriesMenu(int iClient) = 0;
	virtual void ShowCategoryMenu(int iClient, const char* szCategory) = 0;
	virtual void ShowItemMenu(int iClient, const char* szCategory, const char* szItem) = 0;
	virtual void ShowFunctionsMenu(int iClient) = 0;
	virtual void ShowInventoryMenu(int iClient) = 0;
	virtual const char* GetTablePrefix() = 0;
	virtual IMySQLConnection* GetDatabase() = 0;
	virtual const char* GetTranslation(const char* szKey) = 0;
	virtual void PrintToChat(int iClient, const char* szMessage, ...) = 0;
	virtual void ToggleClientCategoryOff(int iClient, const char* szCategory, const char* szItemOptional = "") = 0;
	virtual void RegisterPreview(const char* szCategory, const char* szIdentity, ItemPreviewCallback fn) = 0;
};