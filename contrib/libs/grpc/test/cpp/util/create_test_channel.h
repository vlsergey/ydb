/*
 *
 * Copyright 2015-2016 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_TEST_CPP_UTIL_CREATE_TEST_CHANNEL_H
#define GRPC_TEST_CPP_UTIL_CREATE_TEST_CHANNEL_H

#include <memory>

#include <grpcpp/channel.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>

namespace grpc {
class Channel;

namespace testing {

typedef enum { INSECURE = 0, TLS, ALTS } transport_security;

}  // namespace testing

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, testing::transport_security security_type);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& override_hostname,
    testing::transport_security security_type, bool use_prod_roots);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& override_hostname,
    testing::transport_security security_type, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& override_hostname,
    testing::transport_security security_type, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds,
    const ChannelArguments& args);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& cred_type,
    const TString& override_hostname, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds,
    const ChannelArguments& args);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& credential_type,
    const std::shared_ptr<CallCredentials>& creds);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& override_hostname,
    testing::transport_security security_type, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& override_hostname,
    testing::transport_security security_type, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds, const ChannelArguments& args,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& cred_type,
    const TString& override_hostname, bool use_prod_roots,
    const std::shared_ptr<CallCredentials>& creds, const ChannelArguments& args,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);

std::shared_ptr<Channel> CreateTestChannel(
    const TString& server, const TString& credential_type,
    const std::shared_ptr<CallCredentials>& creds,
    std::vector<
        std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
        interceptor_creators);

}  // namespace grpc

#endif  // GRPC_TEST_CPP_UTIL_CREATE_TEST_CHANNEL_H
