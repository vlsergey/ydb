#pragma once
#include <library/cpp/containers/stack_vector/stack_vec.h>
#include <ydb/core/base/defs.h>
#include <ydb/core/base/events.h>
#include <ydb/core/protos/config.pb.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/login/login.h>
#include <util/string/builder.h>

namespace NKikimr {
    struct TEvTicketParser {
        enum EEv {
            // requests
            EvAuthorizeTicket = EventSpaceBegin(TKikimrEvents::ES_TICKET_PARSER),
            EvRefreshTicket,
            EvDiscardTicket,
            EvUpdateLoginSecurityState,

            // replies
            EvAuthorizeTicketResult = EvAuthorizeTicket + 512,

            EvEnd
        };

        static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_TICKET_PARSER), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_TICKET_PARSER)");

        struct TEvAuthorizeTicket : TEventLocal<TEvAuthorizeTicket, EvAuthorizeTicket> {
            struct TEntry {
                TStackVec<TString> Permissions;
                TStackVec<std::pair<TString, TString>> Attributes;
            };

            const TString Database;
            const TString Ticket;
            const TString PeerName;

            // if two identical permissions with different attributies are specified,
            // only one of them will be processed. Which one is not guaranteed
            const std::vector<TEntry> Entries;

            struct TInitializationFields {
                TString Database;
                TString Ticket;
                TString PeerName;
                std::vector<TEntry> Entries;
            };

            TEvAuthorizeTicket(TInitializationFields&& init)
                : Database(std::move(init.Database))
                , Ticket(std::move(init.Ticket))
                , PeerName(std::move(init.PeerName))
                , Entries(std::move(init.Entries))
            {
            }

            TEvAuthorizeTicket(const TString& ticket)
                : Ticket(ticket)
            {}

            TEvAuthorizeTicket(const TString& ticket, const TString& peerName)
                : Ticket(ticket)
                , PeerName(peerName)
            {}

            TEvAuthorizeTicket(const TString& ticket, const TVector<std::pair<TString, TString>>& attributes, const TVector<TString>& permissions)
                : Ticket(ticket)
                , Entries({{permissions, attributes}})
            {}

            TEvAuthorizeTicket(const TString& ticket, const TString& peerName, const TVector<std::pair<TString, TString>>& attributes, const TVector<TString>& permissions)
                : Ticket(ticket)
                , PeerName(peerName)
                , Entries({{permissions, attributes}})
            {}

            TEvAuthorizeTicket(const TString& ticket, const TVector<TEntry>& entries)
                : Ticket(ticket)
                , Entries(entries)
            {}

            TEvAuthorizeTicket(const TString& ticket, const TString& peerName, const TVector<TEntry>& entries)
                : Ticket(ticket)
                , PeerName(peerName)
                , Entries(entries)
            {}
        };

        struct TError {
            TString Message;
            bool Retryable = true;

            bool empty() const {
                return Message.empty();
            }

            void clear() {
                Message.clear();
                Retryable = true;
            }

            operator bool() const {
                return !empty();
            }

            TString ToString() const {
                return TStringBuilder()
                    << "{message:\"" << Message << "\",retryable:" << Retryable << "}";
            }
        };

        struct TEvAuthorizeTicketResult : TEventLocal<TEvAuthorizeTicketResult, EvAuthorizeTicketResult> {
            TString Ticket;
            TError Error;
            TIntrusivePtr<NACLib::TUserToken> Token;
            TString SerializedToken;

            TEvAuthorizeTicketResult(const TString& ticket, const TIntrusivePtr<NACLib::TUserToken>& token, const TString& serializedToken)
                : Ticket(ticket)
                , Token(token)
                , SerializedToken(serializedToken)
            {}

            TEvAuthorizeTicketResult(const TString& ticket, const TError& error)
                : Ticket(ticket)
                , Error(error)
            {}
        };

        struct TEvRefreshTicket : TEventLocal<TEvRefreshTicket, EvRefreshTicket> {
            const TString Ticket;

            TEvRefreshTicket(const TString& ticket)
                : Ticket(ticket)
            {}
        };

        struct TEvDiscardTicket : TEventLocal<TEvDiscardTicket, EvDiscardTicket> {
            const TString Ticket;

            TEvDiscardTicket(const TString& ticket)
                : Ticket(ticket)
            {}
        };

        struct TEvUpdateLoginSecurityState : TEventLocal<TEvUpdateLoginSecurityState, EvUpdateLoginSecurityState> {
            NLoginProto::TSecurityState SecurityState;

            TEvUpdateLoginSecurityState(NLoginProto::TSecurityState securityState)
                : SecurityState(std::move(securityState))
            {
            }
        };
    };

    inline NActors::TActorId MakeTicketParserID() {
        const char name[12] = "ticketparse";
        return NActors::TActorId(0, TStringBuf(name, 12));
    }
}

template <>
inline void Out<NKikimr::TEvTicketParser::TError>(IOutputStream& str, const NKikimr::TEvTicketParser::TError& error) {
    str << error.Message;
}

namespace NKikimr {
namespace NGRpcService {

class ICheckerIface {
public:
    virtual void SetEntries(const TVector<TEvTicketParser::TEvAuthorizeTicket::TEntry>& entries) = 0;
};

}
}
