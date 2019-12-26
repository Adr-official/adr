
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/SHAMapStore.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/beast/core/LexicalCast.h>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/format.hpp>
namespace ripple {
Json::Value doCanDelete (RPC::Context& context)
{
    if (! context.app.getSHAMapStore().advisoryDelete())
        return RPC::make_error(rpcNOT_ENABLED);
    Json::Value ret (Json::objectValue);
    if (context.params.isMember(jss::can_delete))
    {
        Json::Value canDelete  = context.params.get(jss::can_delete, 0);
        std::uint32_t canDeleteSeq = 0;
        if (canDelete.isUInt())
        {
            canDeleteSeq = canDelete.asUInt();
        }
        else
        {
            std::string canDeleteStr = canDelete.asString();
            boost::to_lower (canDeleteStr);
            if (canDeleteStr.find_first_not_of ("0123456789") ==
                std::string::npos)
            {
                canDeleteSeq =
                        beast::lexicalCast <std::uint32_t>(canDeleteStr);
            }
            else if (canDeleteStr == "never")
            {
                canDeleteSeq = 0;
            }
            else if (canDeleteStr == "always")
            {
                canDeleteSeq = std::numeric_limits <std::uint32_t>::max();
            }
            else if (canDeleteStr == "now")
            {
                canDeleteSeq = context.app.getSHAMapStore().getLastRotated();
                if (!canDeleteSeq)
                    return RPC::make_error (rpcNOT_READY);
            }
            else if (canDeleteStr.size() == 64 &&
                canDeleteStr.find_first_not_of("0123456789abcdef") ==
                std::string::npos)
            {
                auto ledger = context.ledgerMaster.getLedgerByHash (
                    from_hex_text<uint256>(canDeleteStr));
                if (!ledger)
                    return RPC::make_error(rpcLGR_NOT_FOUND, "ledgerNotFound");
                canDeleteSeq = ledger->info().seq;
            }
            else
            {
                return RPC::make_error (rpcINVALID_PARAMS);
            }
        }
        ret[jss::can_delete] =
                context.app.getSHAMapStore().setCanDelete (canDeleteSeq);
    }
    else
    {
        ret[jss::can_delete] = context.app.getSHAMapStore().getCanDelete();
    }
    return ret;
}
} 
