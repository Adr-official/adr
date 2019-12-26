
#include <ripple/app/consensus/RCLConsensus.h>
#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/BuildLedger.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/InboundTransactions.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LocalTxs.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/misc/ValidatorKeys.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/basics/make_lock.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/consensus/LedgerTiming.h>
#include <ripple/nodestore/DatabaseShard.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/overlay/predicates.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/digest.h>
#include <algorithm>
namespace ripple {
RCLConsensus::RCLConsensus(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    Consensus<Adaptor>::clock_type const& clock,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : adaptor_(
          app,
          std::move(feeVote),
          ledgerMaster,
          localTxs,
          inboundTransactions,
          validatorKeys,
          journal)
    , consensus_(clock, adaptor_, journal)
    , j_(journal)
{
}
RCLConsensus::Adaptor::Adaptor(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : app_(app)
        , feeVote_(std::move(feeVote))
        , ledgerMaster_(ledgerMaster)
        , localTxs_(localTxs)
        , inboundTransactions_{inboundTransactions}
        , j_(journal)
        , nodeID_{validatorKeys.nodeID}
        , valPublic_{validatorKeys.publicKey}
        , valSecret_{validatorKeys.secretKey}
{
}
boost::optional<RCLCxLedger>
RCLConsensus::Adaptor::acquireLedger(LedgerHash const& hash)
{
    auto built = ledgerMaster_.getLedgerByHash(hash);
    if (!built)
    {
        if (acquiringLedger_ != hash)
        {
            JLOG(j_.warn()) << "Need consensus ledger " << hash;
            acquiringLedger_ = hash;
            app_.getJobQueue().addJob(jtADVANCE, "getConsensusLedger",
                [id = hash, &app = app_](Job&)
                {
                    app.getInboundLedgers().acquire(id, 0,
                        InboundLedger::Reason::CONSENSUS);
                });
        }
        return boost::none;
    }
    assert(!built->open() && built->isImmutable());
    assert(built->info().hash == hash);
    inboundTransactions_.newRound(built->info().seq);
    parms_.useRoundedCloseTime = built->rules().enabled(fix1528);
    return RCLCxLedger(built);
}
void
RCLConsensus::Adaptor::share(RCLCxPeerPos const& peerPos)
{
    protocol::TMProposeSet prop;
    auto const& proposal = peerPos.proposal();
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_currenttxhash(
        proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.position().size());
    auto const pk = peerPos.publicKey().slice();
    prop.set_nodepubkey(pk.data(), pk.size());
    auto const sig = peerPos.signature();
    prop.set_signature(sig.data(), sig.size());
    app_.overlay().relay(prop, peerPos.suppressionID());
}
void
RCLConsensus::Adaptor::share(RCLCxTx const& tx)
{
    if (app_.getHashRouter().shouldRelay(tx.id()))
    {
        JLOG(j_.debug()) << "Relaying disputed tx " << tx.id();
        auto const slice = tx.tx_.slice();
        protocol::TMTransaction msg;
        msg.set_rawtransaction(slice.data(), slice.size());
        msg.set_status(protocol::tsNEW);
        msg.set_receivetimestamp(
            app_.timeKeeper().now().time_since_epoch().count());
        app_.overlay().foreach (send_always(
            std::make_shared<Message>(msg, protocol::mtTRANSACTION)));
    }
    else
    {
        JLOG(j_.debug()) << "Not relaying disputed tx " << tx.id();
    }
}
void
RCLConsensus::Adaptor::propose(RCLCxPeerPos::Proposal const& proposal)
{
    JLOG(j_.trace()) << "We propose: "
                     << (proposal.isBowOut()
                             ? std::string("bowOut")
                             : ripple::to_string(proposal.position()));
    protocol::TMProposeSet prop;
    prop.set_currenttxhash(
        proposal.position().begin(), proposal.position().size());
    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.position().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_nodepubkey(valPublic_.data(), valPublic_.size());
    auto signingHash = sha512Half(
        HashPrefix::proposal,
        std::uint32_t(proposal.proposeSeq()),
        proposal.closeTime().time_since_epoch().count(),
        proposal.prevLedger(),
        proposal.position());
    auto sig = signDigest(valPublic_, valSecret_, signingHash);
    prop.set_signature(sig.data(), sig.size());
    auto const suppression = proposalUniqueId(
        proposal.position(),
        proposal.prevLedger(),
        proposal.proposeSeq(),
        proposal.closeTime(),
        valPublic_,
        sig);
    app_.getHashRouter ().addSuppression (suppression);
    app_.overlay().send(prop);
}
void
RCLConsensus::Adaptor::share(RCLTxSet const& txns)
{
    inboundTransactions_.giveSet(txns.id(), txns.map_, false);
}
boost::optional<RCLTxSet>
RCLConsensus::Adaptor::acquireTxSet(RCLTxSet::ID const& setId)
{
    if (auto txns = inboundTransactions_.getSet(setId, true))
    {
        return RCLTxSet{std::move(txns)};
    }
    return boost::none;
}
bool
RCLConsensus::Adaptor::hasOpenTransactions() const
{
    return !app_.openLedger().empty();
}
std::size_t
RCLConsensus::Adaptor::proposersValidated(LedgerHash const& h) const
{
    return app_.getValidations().numTrustedForLedger(h);
}
std::size_t
RCLConsensus::Adaptor::proposersFinished(
    RCLCxLedger const& ledger,
    LedgerHash const& h) const
{
    RCLValidations& vals = app_.getValidations();
    return vals.getNodesAfter(
        RCLValidatedLedger(ledger.ledger_, vals.adaptor().journal()), h);
}
uint256
RCLConsensus::Adaptor::getPrevLedger(
    uint256 ledgerID,
    RCLCxLedger const& ledger,
    ConsensusMode mode)
{
    RCLValidations& vals = app_.getValidations();
    uint256 netLgr = vals.getPreferred(
        RCLValidatedLedger{ledger.ledger_, vals.adaptor().journal()},
        ledgerMaster_.getValidLedgerIndex());
    if (netLgr != ledgerID)
    {
        if (mode != ConsensusMode::wrongLedger)
            app_.getOPs().consensusViewChange();
        JLOG(j_.debug())<< Json::Compact(app_.getValidations().getJsonTrie());
    }
    return netLgr;
}
auto
RCLConsensus::Adaptor::onClose(
    RCLCxLedger const& ledger,
    NetClock::time_point const& closeTime,
    ConsensusMode mode) -> Result
{
    const bool wrongLCL = mode == ConsensusMode::wrongLedger;
    const bool proposing = mode == ConsensusMode::proposing;
    notify(protocol::neCLOSING_LEDGER, ledger, !wrongLCL);
    auto const& prevLedger = ledger.ledger_;
    ledgerMaster_.applyHeldTransactions();
    ledgerMaster_.setBuildingLedger(prevLedger->info().seq + 1);
    auto initialLedger = app_.openLedger().current();
    auto initialSet = std::make_shared<SHAMap>(
        SHAMapType::TRANSACTION, app_.family(), SHAMap::version{1});
    initialSet->setUnbacked();
    for (auto const& tx : initialLedger->txs)
    {
        JLOG(j_.trace()) << "Adding open ledger TX " <<
            tx.first->getTransactionID();
        Serializer s(2048);
        tx.first->add(s);
        initialSet->addItem(
            SHAMapItem(tx.first->getTransactionID(), std::move(s)),
            true,
            false);
    }
    if ((app_.config().standalone() || (proposing && !wrongLCL)) &&
        ((prevLedger->info().seq % 256) == 0))
    {
        auto const validations =
            app_.getValidations().getTrustedForLedger (
                prevLedger->info().parentHash);
        if (validations.size() >= app_.validators ().quorum ())
        {
            feeVote_->doVoting(prevLedger, validations, initialSet);
            app_.getAmendmentTable().doVoting(
                prevLedger, validations, initialSet);
        }
    }
    initialSet = initialSet->snapShot(false);
    if (!wrongLCL)
    {
        LedgerIndex const seq = prevLedger->info().seq + 1;
        RCLCensorshipDetector<TxID, LedgerIndex>::TxIDSeqVec proposed;
        initialSet->visitLeaves(
            [&proposed, seq](std::shared_ptr<SHAMapItem const> const& item)
            {
                proposed.emplace_back(item->key(), seq);
            });
        censorshipDetector_.propose(std::move(proposed));
    }
    auto const setHash = initialSet->getHash().as_uint256();
    return Result{
        std::move(initialSet),
        RCLCxPeerPos::Proposal{
            initialLedger->info().parentHash,
            RCLCxPeerPos::Proposal::seqJoin,
            setHash,
            closeTime,
            app_.timeKeeper().closeTime(),
            nodeID_}};
}
void
RCLConsensus::Adaptor::onForceAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value && consensusJson)
{
    doAccept(
        result,
        prevLedger,
        closeResolution,
        rawCloseTimes,
        mode,
        std::move(consensusJson));
}
void
RCLConsensus::Adaptor::onAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value && consensusJson)
{
    app_.getJobQueue().addJob(
        jtACCEPT,
        "acceptLedger",
        [=, cj = std::move(consensusJson) ](auto&) mutable {
            this->doAccept(
                result,
                prevLedger,
                closeResolution,
                rawCloseTimes,
                mode,
                std::move(cj));
            this->app_.getOPs().endConsensus();
        });
}
void
RCLConsensus::Adaptor::doAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value && consensusJson)
{
    prevProposers_ = result.proposers;
    prevRoundTime_ = result.roundTime.read();
    bool closeTimeCorrect;
    const bool proposing = mode == ConsensusMode::proposing;
    const bool haveCorrectLCL = mode != ConsensusMode::wrongLedger;
    const bool consensusFail = result.state == ConsensusState::MovedOn;
    auto consensusCloseTime = result.position.closeTime();
    if (consensusCloseTime == NetClock::time_point{})
    {
        using namespace std::chrono_literals;
        consensusCloseTime = prevLedger.closeTime() + 1s;
        closeTimeCorrect = false;
    }
    else
    {
        consensusCloseTime = effCloseTime(
            consensusCloseTime, closeResolution, prevLedger.closeTime());
        closeTimeCorrect = true;
    }
    JLOG(j_.debug()) << "Report: Prop=" << (proposing ? "yes" : "no")
                     << " val=" << (validating_ ? "yes" : "no")
                     << " corLCL=" << (haveCorrectLCL ? "yes" : "no")
                     << " fail=" << (consensusFail ? "yes" : "no");
    JLOG(j_.debug()) << "Report: Prev = " << prevLedger.id() << ":"
                     << prevLedger.seq();
    std::set<TxID> failed;
    CanonicalTXSet retriableTxs{ result.txns.map_->getHash().as_uint256() };
    JLOG(j_.debug()) << "Building canonical tx set: " << retriableTxs.key();
    for (auto const& item : *result.txns.map_)
    {
        try
        {
            retriableTxs.insert(std::make_shared<STTx const>(SerialIter{item.slice()}));
            JLOG(j_.debug()) << "    Tx: " << item.key();
        }
        catch (std::exception const&)
        {
            failed.insert(item.key());
            JLOG(j_.warn()) << "    Tx: " << item.key() << " throws!";
        }
    }
    auto built = buildLCL(prevLedger, retriableTxs, consensusCloseTime,
        closeTimeCorrect, closeResolution, result.roundTime.read(), failed);
    auto const newLCLHash = built.id();
    JLOG(j_.debug()) << "Built ledger #" << built.seq() << ": " << newLCLHash;
    notify(protocol::neACCEPTED_LEDGER, built, haveCorrectLCL);
    if (haveCorrectLCL && result.state == ConsensusState::Yes)
    {
        std::vector<TxID> accepted;
        result.txns.map_->visitLeaves (
            [&accepted](std::shared_ptr<SHAMapItem const> const& item)
            {
                accepted.push_back(item->key());
            });
        for (auto const& r : retriableTxs)
            failed.insert (r.first.getTXID());
        censorshipDetector_.check(std::move(accepted),
            [curr = built.seq(), j = app_.journal("CensorshipDetector"), &failed]
            (uint256 const& id, LedgerIndex seq)
            {
                if (failed.count(id))
                    return true;
                auto const wait = curr - seq;
                if (wait && (wait % censorshipWarnInternal == 0))
                {
                    std::ostringstream ss;
                    ss << "Potential Censorship: Eligible tx " << id
                       << ", which we are tracking since ledger " << seq
                       << " has not been included as of ledger " << curr
                       << ".";
                    JLOG(j.warn()) << ss.str();
                }
                return false;
            });
    }
    if (validating_)
        validating_ = ledgerMaster_.isCompatible(
            *built.ledger_, j_.warn(), "Not validating");
    if (validating_ && !consensusFail &&
        app_.getValidations().canValidateSeq(built.seq()))
    {
        validate(built, result.txns, proposing);
        JLOG(j_.info()) << "CNF Val " << newLCLHash;
    }
    else
        JLOG(j_.info()) << "CNF buildLCL " << newLCLHash;
    ledgerMaster_.consensusBuilt(
        built.ledger_, result.txns.id(), std::move(consensusJson));
    {
        bool anyDisputes = false;
        for (auto& it : result.disputes)
        {
            if (!it.second.getOurVote())
            {
                try
                {
                    JLOG(j_.debug())
                        << "Test applying disputed transaction that did"
                        << " not get in " << it.second.tx().id();
                    SerialIter sit(it.second.tx().tx_.slice());
                    auto txn = std::make_shared<STTx const>(sit);
                    if (isPseudoTx(*txn))
                        continue;
                    retriableTxs.insert(txn);
                    anyDisputes = true;
                }
                catch (std::exception const&)
                {
                    JLOG(j_.debug())
                        << "Failed to apply transaction we voted NO on";
                }
            }
        }
        auto lock = make_lock(app_.getMasterMutex(), std::defer_lock);
        auto sl = make_lock(ledgerMaster_.peekMutex(), std::defer_lock);
        std::lock(lock, sl);
        auto const lastVal = ledgerMaster_.getValidatedLedger();
        boost::optional<Rules> rules;
        if (lastVal)
            rules.emplace(*lastVal, app_.config().features);
        else
            rules.emplace(app_.config().features);
        app_.openLedger().accept(
            app_,
            *rules,
            built.ledger_,
            localTxs_.getTxSet(),
            anyDisputes,
            retriableTxs,
            tapNONE,
            "consensus",
            [&](OpenView& view, beast::Journal j) {
                return app_.getTxQ().accept(app_, view);
            });
        app_.getOPs().reportFeeChange();
    }
    {
        ledgerMaster_.switchLCL(built.ledger_);
        assert(ledgerMaster_.getClosedLedger()->info().hash == built.id());
        assert(
            app_.openLedger().current()->info().parentHash == built.id());
    }
    if ((mode == ConsensusMode::proposing || mode == ConsensusMode::observing) && !consensusFail)
    {
        auto closeTime = rawCloseTimes.self;
        JLOG(j_.info()) << "We closed at "
                        << closeTime.time_since_epoch().count();
        using usec64_t = std::chrono::duration<std::uint64_t>;
        usec64_t closeTotal =
            std::chrono::duration_cast<usec64_t>(closeTime.time_since_epoch());
        int closeCount = 1;
        for (auto const& p : rawCloseTimes.peers)
        {
            JLOG(j_.info())
                << std::to_string(p.second) << " time votes for "
                << std::to_string(p.first.time_since_epoch().count());
            closeCount += p.second;
            closeTotal += std::chrono::duration_cast<usec64_t>(
                              p.first.time_since_epoch()) *
                p.second;
        }
        closeTotal += usec64_t(closeCount / 2);  
        closeTotal /= closeCount;
        using duration = std::chrono::duration<std::int32_t>;
        using time_point = std::chrono::time_point<NetClock, duration>;
        auto offset = time_point{closeTotal} -
            std::chrono::time_point_cast<duration>(closeTime);
        JLOG(j_.info()) << "Our close offset is estimated at " << offset.count()
                        << " (" << closeCount << ")";
        app_.timeKeeper().adjustCloseTime(offset);
    }
}
void
RCLConsensus::Adaptor::notify(
    protocol::NodeEvent ne,
    RCLCxLedger const& ledger,
    bool haveCorrectLCL)
{
    protocol::TMStatusChange s;
    if (!haveCorrectLCL)
        s.set_newevent(protocol::neLOST_SYNC);
    else
        s.set_newevent(ne);
    s.set_ledgerseq(ledger.seq());
    s.set_networktime(app_.timeKeeper().now().time_since_epoch().count());
    s.set_ledgerhashprevious(
        ledger.parentID().begin(),
        std::decay_t<decltype(ledger.parentID())>::bytes);
    s.set_ledgerhash(
        ledger.id().begin(), std::decay_t<decltype(ledger.id())>::bytes);
    std::uint32_t uMin, uMax;
    if (!ledgerMaster_.getFullValidatedRange(uMin, uMax))
    {
        uMin = 0;
        uMax = 0;
    }
    else
    {
        uMin = std::max(uMin, ledgerMaster_.getEarliestFetch());
    }
    s.set_firstseq(uMin);
    s.set_lastseq(uMax);
    app_.overlay ().foreach (send_always (
        std::make_shared <Message> (
            s, protocol::mtSTATUS_CHANGE)));
    JLOG (j_.trace()) << "send status change to peer";
}
RCLCxLedger
RCLConsensus::Adaptor::buildLCL(
    RCLCxLedger const& previousLedger,
    CanonicalTXSet& retriableTxs,
    NetClock::time_point closeTime,
    bool closeTimeCorrect,
    NetClock::duration closeResolution,
    std::chrono::milliseconds roundTime,
    std::set<TxID>& failedTxs)
{
    std::shared_ptr<Ledger> built = [&]()
    {
        if (auto const replayData = ledgerMaster_.releaseReplay())
        {
            assert(replayData->parent()->info().hash == previousLedger.id());
            return buildLedger(*replayData, tapNONE, app_, j_);
        }
        return buildLedger(previousLedger.ledger_, closeTime, closeTimeCorrect,
            closeResolution, app_, retriableTxs, failedTxs, j_);
    }();
    using namespace std::chrono_literals;
    app_.getTxQ().processClosedLedger(app_, *built, roundTime > 5s);
    if (ledgerMaster_.storeLedger(built))
        JLOG(j_.debug()) << "Consensus built ledger we already had";
    else if (app_.getInboundLedgers().find(built->info().hash))
        JLOG(j_.debug()) << "Consensus built ledger we were acquiring";
    else
        JLOG(j_.debug()) << "Consensus built new ledger";
    return RCLCxLedger{std::move(built)};
}
void
RCLConsensus::Adaptor::validate(RCLCxLedger const& ledger,
    RCLTxSet const& txns,
    bool proposing)
{
    using namespace std::chrono_literals;
    auto validationTime = app_.timeKeeper().closeTime();
    if (validationTime <= lastValidationTime_)
        validationTime = lastValidationTime_ + 1s;
    lastValidationTime_ = validationTime;
    STValidation::FeeSettings fees;
    std::vector<uint256> amendments;
    auto const& feeTrack = app_.getFeeTrack();
    std::uint32_t fee =
        std::max(feeTrack.getLocalFee(), feeTrack.getClusterFee());
    if (fee > feeTrack.getLoadBase())
        fees.loadFee = fee;
    if (((ledger.seq() + 1) % 256) == 0)
    {
        feeVote_->doValidation(ledger.ledger_, fees);
        amendments = app_.getAmendmentTable().doValidation (getEnabledAmendments(*ledger.ledger_));
    }
    auto v = std::make_shared<STValidation>(
        ledger.id(),
        ledger.seq(),
        txns.id(),
        validationTime,
        valPublic_,
        valSecret_,
        nodeID_,
        proposing ,
        fees,
        amendments);
    app_.getHashRouter().addSuppression(
        sha512Half(makeSlice(v->getSerialized())));
    handleNewValidation(app_, v, "local");
    Blob validation = v->getSerialized();
    protocol::TMValidation val;
    val.set_validation(&validation[0], validation.size());
    app_.overlay().send(val);
}
void
RCLConsensus::Adaptor::onModeChange(
    ConsensusMode before,
    ConsensusMode after)
{
    JLOG(j_.info()) << "Consensus mode change before=" << to_string(before)
                    << ", after=" << to_string(after);
    if ((before == ConsensusMode::proposing || before == ConsensusMode::observing) && before != after)
        censorshipDetector_.reset();
    mode_ = after;
}
Json::Value
RCLConsensus::getJson(bool full) const
{
    Json::Value ret;
    {
      ScopedLockType _{mutex_};
      ret = consensus_.getJson(full);
    }
    ret["validating"] = adaptor_.validating();
    return ret;
}
void
RCLConsensus::timerEntry(NetClock::time_point const& now)
{
    try
    {
        ScopedLockType _{mutex_};
        consensus_.timerEntry(now);
    }
    catch (SHAMapMissingNode const& mn)
    {
        JLOG(j_.error()) << "Missing node during consensus process " << mn;
        Rethrow();
    }
}
void
RCLConsensus::gotTxSet(NetClock::time_point const& now, RCLTxSet const& txSet)
{
    try
    {
        ScopedLockType _{mutex_};
        consensus_.gotTxSet(now, txSet);
    }
    catch (SHAMapMissingNode const& mn)
    {
        JLOG(j_.error()) << "Missing node during consensus process " << mn;
        Rethrow();
    }
}
void
RCLConsensus::simulate(
    NetClock::time_point const& now,
    boost::optional<std::chrono::milliseconds> consensusDelay)
{
    ScopedLockType _{mutex_};
    consensus_.simulate(now, consensusDelay);
}
bool
RCLConsensus::peerProposal(
    NetClock::time_point const& now,
    RCLCxPeerPos const& newProposal)
{
    ScopedLockType _{mutex_};
    return consensus_.peerProposal(now, newProposal);
}
bool
RCLConsensus::Adaptor::preStartRound(RCLCxLedger const & prevLgr)
{
    validating_ = valPublic_.size() != 0 &&
                  prevLgr.seq() >= app_.getMaxDisallowedLedger() &&
                  !app_.getOPs().isAmendmentBlocked();
    if (validating_ && !app_.config().standalone() && app_.validators().count())
    {
        auto const when = app_.validators().expires();
        if (!when || *when < app_.timeKeeper().now())
        {
            JLOG(j_.error()) << "Voluntarily bowing out of consensus process "
                                "because of an expired validator list.";
            validating_ = false;
        }
    }
    const bool synced = app_.getOPs().getOperatingMode() == NetworkOPs::omFULL;
    if (validating_)
    {
        JLOG(j_.info()) << "Entering consensus process, validating, synced="
                        << (synced ? "yes" : "no");
    }
    else
    {
        JLOG(j_.info()) << "Entering consensus process, watching, synced="
                        << (synced ? "yes" : "no");
    }
    inboundTransactions_.newRound(prevLgr.seq());
    parms_.useRoundedCloseTime = prevLgr.ledger_->rules().enabled(fix1528);
    return validating_ && synced;
}
bool
RCLConsensus::Adaptor::haveValidated() const
{
    return ledgerMaster_.haveValidated();
}
LedgerIndex
RCLConsensus::Adaptor::getValidLedgerIndex() const
{
    return ledgerMaster_.getValidLedgerIndex();
}
std::pair<std::size_t, hash_set<RCLConsensus::Adaptor::NodeKey_t>>
RCLConsensus::Adaptor::getQuorumKeys() const
{
    return app_.validators().getQuorumKeys();
}
std::size_t
RCLConsensus::Adaptor::laggards(Ledger_t::Seq const seq,
    hash_set<RCLConsensus::Adaptor::NodeKey_t>& trustedKeys) const
{
    return app_.getValidations().laggards(seq, trustedKeys);
}
bool
RCLConsensus::Adaptor::validator() const
{
    return !valPublic_.empty();
}
void
RCLConsensus::startRound(
    NetClock::time_point const& now,
    RCLCxLedger::ID const& prevLgrId,
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowUntrusted)
{
    ScopedLockType _{mutex_};
    consensus_.startRound(
        now, prevLgrId, prevLgr, nowUntrusted, adaptor_.preStartRound(prevLgr));
}
}
