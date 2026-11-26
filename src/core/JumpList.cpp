#include "JumpList.h"

#include <QCoreApplication>
#include <QDir>
#include <QSet>

#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <shobjidl.h>

#include <cstring>
#include <string>

namespace {

std::wstring g_appId = L"LuridLane.RConnect";

HRESULT setPropString(IPropertyStore *store, const PROPERTYKEY &key, const std::wstring &value)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    auto *buf = static_cast<wchar_t *>(CoTaskMemAlloc(bytes));
    if (!buf)
        return E_OUTOFMEMORY;
    std::memcpy(buf, value.c_str(), bytes);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = buf;
    const HRESULT hr = store->SetValue(key, pv);
    PropVariantClear(&pv);
    return hr;
}

QSet<QString> removedArguments(IObjectArray *removed)
{
    QSet<QString> skip;
    if (!removed)
        return skip;
    UINT count = 0;
    if (FAILED(removed->GetCount(&count)))
        return skip;
    for (UINT i = 0; i < count; ++i) {
        IShellLinkW *link = nullptr;
        if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&link))) && link) {
            wchar_t args[512] = {};
            if (SUCCEEDED(link->GetArguments(args, int(std::size(args)))))
                skip.insert(QString::fromWCharArray(args).trimmed());
            link->Release();
        }
    }
    return skip;
}

IShellLinkW *makeServerLink(const std::wstring &exePath, const JumpList::Item &item, const QString &args)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link)
        return nullptr;

    const std::wstring wArgs = args.toStdWString();
    const std::wstring wDesc = (item.host.isEmpty() ? item.name : QStringLiteral("%1 (%2)").arg(item.name, item.host)).toStdWString();
    link->SetPath(exePath.c_str());
    link->SetArguments(wArgs.c_str());
    link->SetDescription(wDesc.c_str());
    link->SetIconLocation(exePath.c_str(), 0);

    IPropertyStore *store = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && store) {
        setPropString(store, PKEY_Title, item.name.toStdWString());
        store->Commit();
        store->Release();
    }
    return link;
}

} // namespace

namespace JumpList {

void initAppId(bool testMode)
{
    g_appId = testMode ? L"LuridLane.RConnect.Test" : L"LuridLane.RConnect";
    SetCurrentProcessExplicitAppUserModelID(g_appId.c_str());
}

void update(const QString &categoryTitle, const QList<Item> &items)
{
    ICustomDestinationList *cdl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cdl))) || !cdl)
        return;

    cdl->SetAppID(g_appId.c_str());
    if (items.isEmpty()) {
        cdl->DeleteList(g_appId.c_str());
        cdl->Release();
        return;
    }

    UINT maxSlots = 0;
    IObjectArray *removed = nullptr;
    if (FAILED(cdl->BeginList(&maxSlots, IID_PPV_ARGS(&removed)))) {
        cdl->Release();
        return;
    }
    const QSet<QString> skip = removedArguments(removed);
    if (removed)
        removed->Release();

    IObjectCollection *collection = nullptr;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))
        || !collection) {
        cdl->AbortList();
        cdl->Release();
        return;
    }

    const std::wstring exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    UINT added = 0;
    const UINT limit = qMin<UINT>(maxSlots > 0 ? maxSlots : 8u, 8u);
    for (const Item &item : items) {
        if (added >= limit)
            break;
        if (item.id.isEmpty() || item.name.isEmpty())
            continue;
        const QString args = QStringLiteral("--connect %1").arg(item.id);
        if (skip.contains(args))
            continue;
        if (IShellLinkW *link = makeServerLink(exePath, item, args)) {
            if (SUCCEEDED(collection->AddObject(link)))
                ++added;
            link->Release();
        }
    }

    bool committed = false;
    if (added > 0) {
        IObjectArray *array = nullptr;
        if (SUCCEEDED(collection->QueryInterface(IID_PPV_ARGS(&array))) && array) {
            const std::wstring wCategory = categoryTitle.toStdWString();
            if (SUCCEEDED(cdl->AppendCategory(wCategory.c_str(), array)))
                committed = SUCCEEDED(cdl->CommitList());
            array->Release();
        }
    }
    if (!committed)
        cdl->AbortList();

    collection->Release();
    cdl->Release();
}

} // namespace JumpList
