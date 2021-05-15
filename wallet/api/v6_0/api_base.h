// Copyright 2018 The Beam Team
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
#pragma once

#include <boost/serialization/strong_typedef.hpp>
#include "api_errors_imp.h"
#include "wallet/core/common.h"
#include "utility/common.h"
#include "../i_wallet_api.h"

namespace beam::wallet
{
    using json = nlohmann::json;
    using JsonRpcId = json;

    #define API_WRITE_ACCESS true
    #define API_READ_ACCESS false

    class ApiBase
        : public IWalletApi
    {
    public:
        static inline const char JsonRpcHeader[] = "jsonrpc";
        static inline const char JsonRpcVersion[] = "2.0";

        // user api key and read/write access
        ApiBase(IWalletApiHandler& handler, ACL acl, std::string appid, std::string appname);

        void sendError(const JsonRpcId& id, ApiError code, const std::string& data = "");

        boost::optional<ParseResult> parseAPIRequest(const char* data, size_t size) override;
        ApiSyncMode executeAPIRequest(const char *data, size_t size) override;
        std::string fromError(const std::string& request, ApiError code, const std::string& errorText) override;

        //
        // getMandatory....
        // return param if it exists and is of the requested type & constraints or throw otherwise
        //
        template<typename T>
        static T getMandatoryParam(const json &params, const std::string &name)
        {
            if (auto param = getOptionalParam<T>(params, name))
            {
                return *param;
            }
            throw jsonrpc_exception(ApiError::InvalidParamsJsonRpc, "Parameter '" + name + "' doesn't exist.");
        }

        //
        // getOptional... throws only if type constraints are violated
        //
        template<typename T>
        static boost::optional<T> getOptionalParam(const json& params, const std::string& name)
        {
            if(auto raw = getOptionalParam<const json&>(params, name))
            {
                return (*raw).get<T>();
            }
            return boost::none;
        }

        static bool hasParam(const json &params, const std::string &name);

    protected:
        struct Method
        {
            std::function<void(const JsonRpcId &id, const json &msg)> execFunc;
            std::function<IWalletApi::MethodInfo (const JsonRpcId &id, const json &msg)> parseFunc;

            bool writeAccess;
            bool isAsync;
            bool appsAllowed;
        };

        std::unordered_map <std::string, Method> _methods;
        ACL _acl;
        std::string _appId;
        std::string _appName;
        IWalletApiHandler& _handler;

    private:
        static json formError(const JsonRpcId& id, ApiError code, const std::string& data = "");
        boost::optional<ApiCallInfo> parseCallInfo(const char* data, size_t size);

        template<typename TRes>
        boost::optional<TRes> callGuarded(const JsonRpcId& rpcid, std::function<TRes (void)> func)
        {
            try
            {
                return func();
            }
            catch (const nlohmann::detail::type_error& e)
            {
                auto error = formError(rpcid, ApiError::InvalidParamsJsonRpc, e.what());
                _handler.onParseError(error);
            }
            catch (const nlohmann::detail::exception& e)
            {
                auto error = formError(rpcid, ApiError::InvalidJsonRpc, e.what());
                _handler.onParseError(error);
            }
            catch (const jsonrpc_exception& e)
            {
                auto error = formError(rpcid, e.code(), e.whatstr());
                switch(e.code())
                {
                case ApiError::InvalidJsonRpc:
                case ApiError::InvalidParamsJsonRpc:
                    _handler.onParseError(error);
                    break;
                default:
                    _handler.sendAPIResponse(error);
                }
            }
            catch (const std::runtime_error& e)
            {
                auto error = formError(rpcid, ApiError::InternalErrorJsonRpc, e.what());
                _handler.sendAPIResponse(error);
            }
            catch (const std::exception& e)
            {
                auto error = formError(rpcid, ApiError::InternalErrorJsonRpc, e.what());
                _handler.sendAPIResponse(error);
            }
            catch (...)
            {
                auto error = formError(rpcid, ApiError::InternalErrorJsonRpc, "API call failed, please take a look at logs");
                _handler.sendAPIResponse(error);
            }

            return boost::none;
        }
    };

    // boost::optional<json> is not defined intentionally, use const json& instead
    template<>
    boost::optional<json> ApiBase::getOptionalParam<json>(const json &params, const std::string &name);

    template<>
    boost::optional<const json&> ApiBase::getOptionalParam<const json&>(const json &params, const std::string &name);

    template<> // can be empty but must be a string
    boost::optional<std::string> ApiBase::getOptionalParam<std::string>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(std::string, NonEmptyString)
    template<>
    boost::optional<NonEmptyString> ApiBase::getOptionalParam<NonEmptyString>(const json &params, const std::string &name);

    template<>
    boost::optional<bool> ApiBase::getOptionalParam<bool>(const json &params, const std::string &name);

    template<>
    boost::optional<uint32_t> ApiBase::getOptionalParam<uint32_t>(const json &params, const std::string &name);

    template<>
    boost::optional<uint64_t> ApiBase::getOptionalParam<uint64_t>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(json, JsonArray)
    inline void to_json(json& j, const JsonArray& p) {
        j = p.t;
    }

    template<>
    boost::optional<JsonArray> ApiBase::getOptionalParam<JsonArray>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(json, NonEmptyJsonArray)
    inline void to_json(json& j, const NonEmptyJsonArray& p) {
        j = p.t;
    }

    template<>
    boost::optional<NonEmptyJsonArray> ApiBase::getOptionalParam<NonEmptyJsonArray>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(uint32_t, PositiveUint32)
    template<>
    boost::optional<PositiveUint32> ApiBase::getOptionalParam<PositiveUint32>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(uint64_t, PositiveUnit64)
    template<>
    boost::optional<PositiveUnit64> ApiBase::getOptionalParam<PositiveUnit64>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(Amount, PositiveAmount)
    template<>
    boost::optional<PositiveAmount> ApiBase::getOptionalParam<PositiveAmount>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(Height, PositiveHeight)
    template<>
    boost::optional<PositiveHeight> ApiBase::getOptionalParam<PositiveHeight>(const json &params, const std::string &name);

    BOOST_STRONG_TYPEDEF(TxID, ValidTxID)
    template<>
    boost::optional<ValidTxID> ApiBase::getOptionalParam<ValidTxID>(const json &params, const std::string &name);
}
