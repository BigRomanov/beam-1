// Copyright 2021 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/threading.h>
#include <emscripten/val.h>
#include <boost/algorithm/string.hpp>
#include <queue>
#include <exception>
#include <filesystem>

#include "common.h"
#include "wallet/client/wallet_client.h"
#include "wallet/core/wallet_db.h"
#include "wallet/api/i_wallet_api.h"
#include "wallet/transactions/lelantus/lelantus_reg_creators.h"
#include "wallet/transactions/lelantus/push_transaction.h"
#include "mnemonic/mnemonic.h"
#include "utility/string_helpers.h"
#include "wasm_beamapi.h"

namespace fs = std::filesystem;
using namespace beam;
using namespace beam::io;
using namespace std;
using namespace emscripten;
using namespace ECC;
using namespace beam::wallet;

#define Assert(x) ((void)((x) || (__assert_fail(#x, __FILE__, __LINE__, __func__),0)))

namespace
{
    void GetWalletSeed(NoLeak<uintBig>& walletSeed, const std::string& s)
    {
        SecString seed;
        WordList phrase;

        auto tempPhrase = s;
        boost::algorithm::trim_if(tempPhrase, [](char ch) { return ch == ';'; });
        phrase = string_helpers::split(tempPhrase, ';');

        auto buf = decodeMnemonic(phrase);
        seed.assign(buf.data(), buf.size());

        walletSeed.V = seed.hash().V;
    }

    void GenerateDefaultAddress(IWalletDB::Ptr db)
    {
        db->generateAndSaveDefaultAddress();
    }

    IWalletDB::Ptr CreateDatabase(const std::string& s, const std::string& dbName, const std::string& pass)
    {
        ECC::NoLeak<ECC::uintBig> seed;
        GetWalletSeed(seed, s);
        auto r = io::Reactor::create();
        io::Reactor::Scope scope(*r);
        auto db = WalletDB::init(dbName, SecString(pass), seed);
        GenerateDefaultAddress(db);
        EM_ASM
        (
            FS.syncfs(false, function()
        {
            console.log("wallet created!");
        });
        );
        return db;
    }
}

class WalletClient2
    : public WalletClient
    , public std::enable_shared_from_this<WalletClient2>
{
public:
    using Callback = std::function<void()>;
    using WeakPtr = std::weak_ptr<WalletClient2>;
    using Ptr = std::shared_ptr<WalletClient2>;
    using WalletClient::postFunctionToClientContext;

    struct ICallbackHandler
    {
        virtual void OnSyncProgress(int done, int total) {}
        virtual void OnResult(const json&) {}
        virtual void OnApproveSend(const std::string&, const string&, WasmAppApi::WeakPtr api) {}
        virtual void OnApproveContractInfo(const std::string& request, const std::string& info, const std::string& amounts, WasmAppApi::WeakPtr api) {}
    };

    using WalletClient::WalletClient;

    virtual ~WalletClient2()
    {
        stopReactor(true);
    }

    void SetHandler(ICallbackHandler* cb)
    {
        m_CbHandler = cb;
    }

    void SendResult(const json& result)
    {
        postFunctionToClientContext([this, sp = shared_from_this(), result]()
        {
            if (m_CbHandler)
            {
                m_CbHandler->OnResult(result);
            }
        });
    }

    void ClientThread_ApproveSend(const std::string& request, const std::string& info, WasmAppApi::Ptr api)
    {
        AssertMainThread();
        if (m_CbHandler)
        {
            m_CbHandler->OnApproveSend(request, info, api);
        }
    }

    void ClientThread_ApproveContractInfo(const std::string& request, const std::string& info, const std::string& amounts, WasmAppApi::Ptr api)
    {
        AssertMainThread();
        if (m_CbHandler)
        {
            m_CbHandler->OnApproveContractInfo(request, info, amounts, api);
        }
    }

    void Stop(Callback&& handler)
    {
        m_StoppedHandler = std::move(handler);
        stopReactor(true);
    }

    IWalletDB::Ptr GetWalletDB()
    {
        return getWalletDB();
    }

private:
    void onSyncProgressUpdated(int done, int total) override
    {
        postFunctionToClientContext([this, done, total]()
        {
            if (m_CbHandler)
            {
                m_CbHandler->OnSyncProgress(done, total);
            }
        });
    }

    void onPostFunctionToClientContext(MessageFunction&& func) override
    {
        {
            std::unique_lock lock(m_Mutex);
            m_Messages.push(std::move(func));
        }
        auto thisWeakPtr = std::make_unique<WeakPtr>(shared_from_this());
        emscripten_async_run_in_main_runtime_thread(
            EM_FUNC_SIG_VI,
            &WalletClient2::ProsessMessageOnMainThread,
            reinterpret_cast<int>(thisWeakPtr.release()));
    }

    void onStopped() override
    {
        WalletClient2::WeakPtr wp = shared_from_this();
        Assert(wp.use_count() == 1);
        postFunctionToClientContext([sp = shared_from_this(), wp]() mutable
        {
            Assert(wp.use_count() == 2);
            if (sp->m_StoppedHandler)
            {
                auto h = std::move(sp->m_StoppedHandler);
                sp.reset(); // handler may hold WalletClient too, but can destroy it, we don't want to prevent this
                Assert(wp.use_count() == 1);
                h();
            }
        });
    }

    static void ProsessMessageOnMainThread(int pThis)
    {
        std::unique_ptr<WeakPtr> wp(reinterpret_cast<WeakPtr*>(pThis));
        assert(wp->use_count() == 1);
        while (true)
        {
            MessageFunction func;
            if (auto sp = wp->lock())
            {
                assert(wp->use_count() == 2);
                {
                    std::unique_lock lock(sp->m_Mutex);
                    if (sp->m_Messages.empty())
                    {
                        return;
                    }
                    func = std::move(sp->m_Messages.front());
                    sp->m_Messages.pop();
                }
            }
            else
            {
                return;
            }
            func();
        }
    }

private:
    std::mutex m_Mutex;
    std::queue<MessageFunction> m_Messages;
    ICallbackHandler* m_CbHandler = nullptr;
    Callback m_StoppedHandler;
    IWalletApi::Ptr m_WalletApi;
};

class AppAPICallback
{
public:
    AppAPICallback(WasmAppApi::WeakPtr sp)
        : m_Api(sp)
    {}

    void SendApproved(const std::string& request)
    {
        AssertMainThread();
        if (auto sp = m_Api.lock())
            sp->AnyThread_callWalletApiDirectly(request);
    }

    void SendRejected(const std::string& request)
    {
        AssertMainThread();
        if (auto sp = m_Api.lock())
            sp-> AnyThread_sendApiError(request, beam::wallet::ApiError::UserRejected, std::string());
    }

    void ContractInfoApproved(const std::string& request)
    {
        AssertMainThread();
        if (auto sp = m_Api.lock())
            sp->AnyThread_callWalletApiDirectly(request);
    }

    void ContractInfoRejected(const std::string& request)
    {
        AssertMainThread();
        if (auto sp = m_Api.lock())
            sp->AnyThread_sendApiError(request, beam::wallet::ApiError::UserRejected, std::string());
    }

private:
    WasmAppApi::WeakPtr m_Api;
};

class WasmWalletClient
    : public IWalletApiHandler
    , private WalletClient2::ICallbackHandler
{
public:
    WasmWalletClient(const std::string& dbName, const std::string& pass, const std::string& node)
        : m_Logger(beam::Logger::create(LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG))
        , m_Reactor(io::Reactor::create())
        , m_DbPath(dbName)
        , m_Pass(pass)
        , m_Node(node)
    {
        wallet::g_AssetsEnabled = true;
    }

    void sendAPIResponse(const json& result) override
    {
        m_Client->SendResult(result);
    }

    uint32_t AddCallback(val&& callback)
    {
        for (uint32_t i = 0; i < m_Callbacks.size(); ++i)
        {
            auto& cb = m_Callbacks[i];
            if (cb.isNull())
            {
                cb = std::move(callback);
                return i;
            }
        }
        m_Callbacks.push_back(std::move(callback));
        return static_cast<uint32_t>(m_Callbacks.size() - 1);
    }

    int Subscribe(val callback)
    {
        for (uint32_t i = 0; i < m_Callbacks.size(); ++i)
        {
            auto& cb = m_Callbacks[i];
            if (cb.isNull())
            {
                cb = std::move(callback);
                return i;
            }
        }
        m_Callbacks.push_back(std::move(callback));
        return static_cast<uint32_t>(m_Callbacks.size() - 1);
    }

    void Unsubscribe(int key)
    {
        if (key == m_Callbacks.size() - 1)
        {
            m_Callbacks.pop_back();
        }
        else if (key < m_Callbacks.size() - 1)
        {
            m_Callbacks[key] = val::null();
        }
    }

    void SetSyncHandler(val handler)
    {
        AssertMainThread();
        m_SyncHandler = std::make_unique<val>(std::move(handler));
    }

    void SetApproveSendHandler(val handler)
    {
        AssertMainThread();
        m_ApproveSendHandler = std::make_unique<val>(std::move(handler));
    }

    void SetApproveContractInfoHandler(val handler)
    {
        AssertMainThread();
        m_ApproveContractInfoHandler = std::make_unique<val>(std::move(handler));
    }

    void ExecuteAPIRequest(const std::string& request)
    {
        AssertMainThread();
        if (!m_Client)
        {
            LOG_ERROR() << "Client is not running";
            return;
        }
        m_Client->getAsync()->makeIWTCall([this, request]()
        {
            if (!m_WalletApi)
            {
                ApiInitData initData;
                initData.walletDB = m_Client->GetWalletDB();
                initData.wallet = m_Client->getWallet();
                m_WalletApi = IWalletApi::CreateInstance(ApiVerCurrent, *this, initData);
            }

            m_WalletApi->executeAPIRequest(request.data(), request.size());
            return boost::none;
        },
            [](const boost::any&) {
        });
    }

    bool StartWallet()
    {
        AssertMainThread();
        EnsureFSMounted();
        if (m_Client)
        {
            LOG_WARNING() << "The client is already running";
            return false;
        }

        try
        {
            auto dbFunc = [path = m_DbPath, pass = m_Pass]() {return OpenWallet(path, pass);};
            m_Client = std::make_shared<WalletClient2>(Rules::get(), dbFunc, m_Node, m_Reactor);
            m_Client->SetHandler(this);
            auto additionalTxCreators = std::make_shared<std::unordered_map<TxType, BaseTransaction::Creator::Ptr>>();
            additionalTxCreators->emplace(TxType::PushTransaction, std::make_shared<lelantus::PushTransaction::Creator>(dbFunc));
            m_Client->getAsync()->enableBodyRequests(true);
            m_Client->start({}, true, additionalTxCreators);
            return true;
        }
        catch (const std::exception& ex)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << ex.what();
        }
        return false;
    }

    void StopWallet(val handler = val::null())
    {
        AssertMainThread();
        if (!m_Client)
        {
            LOG_WARNING() << "The client is stopped";
            return;
        }
        m_Client->getAsync()->makeIWTCall([this]()
        {
            m_WalletApi.reset();
            return boost::none;
        },
            [](const boost::any&) {
        });
        std::weak_ptr<WalletClient2> wp = m_Client;
        Assert(wp.use_count() == 1);
        m_Client->Stop([wp, sp = std::move(m_Client), handler = std::move(handler)]() mutable
        {
            AssertMainThread();
            Assert(wp.use_count() == 1);
            sp.reset(); // release client, at this point destructor should be called and handler code can rely on that client is really stopped and destroyed
            if (!handler.isNull())
            {
                auto handlerPtr = std::make_unique<val>(std::move(handler));
                emscripten_dispatch_to_thread_async(
                    get_thread_id(),
                    EM_FUNC_SIG_VI,
                    &WasmWalletClient::DoCallbackOnMainThread,
                    nullptr,
                    handlerPtr.release());
            }
        });
    }

    void OnSyncProgress(int done, int total) override
    {
        AssertMainThread();
        if (m_SyncHandler && !m_SyncHandler->isNull())
        {
            (*m_SyncHandler)(done, total);
        }
    }

    void OnResult(const json& result) override
    {
        AssertMainThread();
        auto r = result.dump();
        for (auto& cb : m_Callbacks)
        {
            if (!cb.isNull())
            {
                cb(r);
            }
        }
    }

    void OnApproveSend(const std::string& request, const std::string& info, WasmAppApi::WeakPtr api) override
    {
        AssertMainThread();
        if (m_ApproveSendHandler && !m_ApproveSendHandler->isNull())
        {
            (*m_ApproveSendHandler)(request, info, AppAPICallback(api));
        }
    }

    void OnApproveContractInfo(const std::string& request, const std::string& info, const std::string& amounts, WasmAppApi::WeakPtr api) override
    {
        AssertMainThread();
        if (m_ApproveContractInfoHandler && !m_ApproveContractInfoHandler->isNull())
        {
            (*m_ApproveContractInfoHandler)(request, info, amounts, AppAPICallback(api));
        }
    }

    void CreateAppAPI(const std::string& appid, const std::string& appname, val cb)
    {
        AssertMainThread();

        std::weak_ptr<WalletClient2> weak2 = m_Client;
        WasmAppApi::ClientThread_Create(m_Client.get(), "current", appid, appname,
            [cb, weak2](WasmAppApi::Ptr wapi)
            {
                WasmAppApi::WeakPtr weakApi = wapi;
                wapi->SetPostToClientHandler(
                    [weak2](std::function<void (void)> func)
                    {
                        if (auto client2 = weak2.lock())
                        {
                            client2->postFunctionToClientContext([func]() {
                                func();
                            });
                        }
                    }
                );

                wapi->SetContractConsentHandler(
                    [weak2, weakApi](const std::string& req, const std::string& info, const std::string& amoutns)
                    {
                        auto client2 = weak2.lock();
                        auto wapi = weakApi.lock();

                        if(client2 && wapi)
                        {
                            client2->ClientThread_ApproveContractInfo(req, info, amoutns, wapi);
                        }
                    }
                );

                wapi->SetSendConsentHandler(
                    [weak2, weakApi](const std::string& req, const std::string& info)
                    {
                        auto client2 = weak2.lock();
                        auto wapi = weakApi.lock();

                        if(client2 && wapi)
                        {
                            client2->ClientThread_ApproveSend(req, info, wapi);
                        }
                    }
                );

                cb(wapi);
            }
        );
    }

    static void DoCallbackOnMainThread(val* h)
    {
        std::unique_ptr<val> handler(h);
        if (!handler->isNull())
        {
            (*handler)();
        }
    }

    bool IsRunning() const
    {
        AssertMainThread();
        return m_Client && m_Client->isRunning();
    }

    static std::string GeneratePhrase()
    {
        return boost::join(createMnemonic(getEntropy()), " ");
    }

    static bool IsAllowedWord(const std::string& word)
    {
        return isAllowedWord(word);
    }

    static bool IsValidPhrase(const std::string& words)
    {
        return isValidMnemonic(string_helpers::split(words, ' '));
    }

    static std::string ConvertTokenToJson(const std::string& token)
    {
        return wallet::ConvertTokenToJson(token);
    }

    static std::string ConvertJsonToToken(const std::string& jsonParams)
    {
        return wallet::ConvertJsonToToken(jsonParams);
    }

    static void CreateWallet(const std::string& seed, const std::string& dbName, const std::string& pass)
    {
        AssertMainThread();
        CreateDatabase(seed, dbName, pass);
    }

    static void DeleteWallet(const std::string& dbName)
    {
        AssertMainThread();
        fs::remove(dbName);
    }

    static IWalletDB::Ptr OpenWallet(const std::string& dbName, const std::string& pass)
    {
        Rules::get().UpdateChecksum();
        LOG_INFO() << "Rules signature: " << Rules::get().get_SignatureStr();
        return WalletDB::open(dbName, SecString(pass));
    }

    static void MountFS(val cb)
    {
        AssertMainThread();
        m_MountCB = cb;
        EM_ASM
        (
            {
                FS.mkdir("/beam_wallet");
                FS.mount(IDBFS, {}, "/beam_wallet");
                console.log("mounting...");
                FS.syncfs(true, function()
                {
                    console.log("mounted");
                    dynCall('v', $0);
                });

            }, OnMountFS
        );
    }
private:
    static void OnMountFS()
    {
        m_MountCB();
    }

private:
    static val m_MountCB;
    std::shared_ptr<Logger> m_Logger;
    io::Reactor::Ptr m_Reactor;
    std::string m_DbPath;
    std::string m_Pass;
    std::string m_Node;
    std::vector<val> m_Callbacks;
    std::unique_ptr<val> m_SyncHandler;
    std::unique_ptr<val> m_ApproveSendHandler;
    std::unique_ptr<val> m_ApproveContractInfoHandler;
    WalletClient2::Ptr m_Client;
    IWalletApi::Ptr m_WalletApi;
};

val WasmWalletClient::m_MountCB = val::null();

// Binding code
EMSCRIPTEN_BINDINGS()
{
    class_<WasmWalletClient>("WasmWalletClient")
        .constructor<const std::string&, const std::string&, const std::string&>()
        .function("startWallet", &WasmWalletClient::StartWallet)
        .function("stopWallet", &WasmWalletClient::StopWallet)
        .function("isRunning", &WasmWalletClient::IsRunning)
        .function("sendRequest", &WasmWalletClient::ExecuteAPIRequest)
        .function("subscribe", &WasmWalletClient::Subscribe)
        .function("unsubscribe", &WasmWalletClient::Unsubscribe)
        .function("setSyncHandler", &WasmWalletClient::SetSyncHandler)
        .function("setApproveSendHandler", &WasmWalletClient::SetApproveSendHandler)
        .function("setApproveContractInfoHandler", &WasmWalletClient::SetApproveContractInfoHandler)
        .function("createAppAPI", &WasmWalletClient::CreateAppAPI)
        .class_function("GeneratePhrase", &WasmWalletClient::GeneratePhrase)
        .class_function("IsAllowedWord", &WasmWalletClient::IsAllowedWord)
        .class_function("IsValidPhrase", &WasmWalletClient::IsValidPhrase)
        .class_function("ConvertTokenToJson", &WasmWalletClient::ConvertTokenToJson)
        .class_function("ConvertJsonToToken", &WasmWalletClient::ConvertJsonToToken)
        .class_function("CreateWallet", &WasmWalletClient::CreateWallet)
        .class_function("MountFS", &WasmWalletClient::MountFS)
        .class_function("DeleteWallet", &WasmWalletClient::DeleteWallet)
        ;
    class_<WasmAppApi>("AppAPI")
        .smart_ptr<std::shared_ptr<WasmAppApi>>("AppAPI")
        .function("callWalletApi", &WasmAppApi::CallWalletAPI)
        .function("setHandler", &WasmAppApi::SetResultHandler)
        ;
    class_<AppAPICallback>("AppAPICallback")
        .constructor<WasmAppApi::Ptr>()
        .function("sendApproved", &AppAPICallback::SendApproved)
        .function("sendRejected", &AppAPICallback::SendRejected)
        .function("contractInfoApproved", &AppAPICallback::ContractInfoApproved)
        .function("contractInfoRejected", &AppAPICallback::ContractInfoRejected)
        ;
}