#include "floyd/src/floyd_peer_thread.h"

#include <climits>
#include <google/protobuf/text_format.h>
#include "floyd/src/floyd_primary_thread.h"
#include "floyd/src/floyd_context.h"
#include "floyd/src/floyd_client_pool.h"
#include "floyd/src/raft/log.h"
#include "floyd/src/command.pb.h"
#include "floyd/src/logger.h"

#include "slash/include/env.h"
#include "slash/include/slash_mutex.h"

namespace floyd {

Peer::Peer(FloydPeerEnv env)
  : env_(env),
    next_index_(1),
    match_index_(0) {
}

int Peer::StartThread() {
  bg_thread_.set_thread_name("FloydPr" + env_.server.substr(env_.server.find(':')));
  return bg_thread_.StartThread();
}

Peer::~Peer() {
  LOG_INFO("Peer(%s) exit!!!", env_.server.c_str());
  //bg_thread_.set_runing(false);
}

void Peer::set_next_index(uint64_t next_index) {
  next_index_ = next_index;
}

uint64_t Peer::get_next_index() {
  return next_index_;
}

void Peer::AddRequestVoteTask() {
  bg_thread_.Schedule(DoRequestVote, this);
}

void Peer::DoRequestVote(void *arg) {
  Peer *peer = static_cast<Peer*>(arg);
  LOG_DEBUG("Peer(%s)::DoRequestVote", peer->env_.server.c_str());
  Status result = peer->RequestVote();
  if (!result.ok()) {
    LOG_ERROR("Peer(%s) failed to RequestVote caz %s.", peer->env_.server.c_str(), result.ToString().c_str());
  }
}

Status Peer::RequestVote() {
  if (env_.context->role() != Role::kCandidate) {
    return Status::OK();
  }
  
  // TODO (anan) log->getEntry() need lock
  uint64_t last_log_term = 0;
  uint64_t last_log_index = env_.context->log()->GetLastLogIndex();
  if (last_log_index != 0) {
    last_log_term = env_.context->log()->GetEntry(last_log_index).term();
  }
  uint64_t current_term = env_.context->current_term();

  command::Command req;
  req.set_type(command::Command::RaftVote);
  floyd::raft::RequestVote* rqv = req.mutable_rqv();
  rqv->set_ip(env_.context->local_ip());
  rqv->set_port(env_.context->local_port());
  rqv->set_term(current_term);
  rqv->set_last_log_term(last_log_term);
  rqv->set_last_log_index(last_log_index);

#if (LOG_LEVEL != LEVEL_NONE)
  std::string text_format;
  google::protobuf::TextFormat::PrintToString(req, &text_format);
  LOG_DEBUG("Send RequestVote to %s, message :\n%s", env_.server.c_str(), text_format.c_str());
#endif

  command::CommandRes res;
  Status result = env_.pool->SendAndRecv(env_.server, req, &res);
  
  if (!result.ok()) {
    LOG_DEBUG("RequestVote to %s failed %s", env_.server.c_str(), result.ToString().c_str());
    return result;
  }

#if (LOG_LEVEL != LEVEL_NONE)
  google::protobuf::TextFormat::PrintToString(res, &text_format);
  LOG_DEBUG("Recv RequestVote from %s, message :\n%s", env_.server.c_str(), text_format.c_str());
#endif

  uint64_t res_term = res.rsv().term();
  if (result.ok() && env_.context->role() == Role::kCandidate) {
    if (res.rsv().granted()) {
      LOG_DEBUG("Peer(%s)::RequestVote granted will Vote and check", env_.server.c_str());
      if (env_.context->VoteAndCheck(res_term)) {
        env_.primary->AddTask(kBecomeLeader);  
      }
    } else {
      LOG_DEBUG("Vote request denied by %s, res_term=%lu, current_term=%lu",
                env_.server.c_str(), res_term, current_term);
      if (res_term > current_term) {
        //TODO(anan) maybe combine these 2 steps
        env_.context->BecomeFollower(res_term);
        env_.primary->ResetElectLeaderTimer();
        //env_.primary->AddTask(TaskType::kCheckElectLeader);
        //env_.floyd->ResetLeaderElectTimer();
      }
    }
  }

  return result;
}

void Peer::BecomeLeader() {
  {
    slash::MutexLock l(&mu_);
    next_index_ = env_.log->GetLastLogIndex() + 1;
    match_index_ = 0;
  }
  LOG_DEBUG("Peer(%s)::BecomeLeader next_index=%lu", env_.server.c_str(), next_index_);

  // right now
  bg_thread_.Schedule(DoHeartBeat, this);
}

void Peer::AddAppendEntriesTask() {
  bg_thread_.Schedule(DoAppendEntries, this);
}

void Peer::DoAppendEntries(void *arg) {
  Peer* peer = static_cast<Peer*>(arg);
  LOG_DEBUG("Peer(%s) DoAppendEntries", peer->env_.server.c_str());
  Status result = peer->AppendEntries();
  if (!result.ok()) {
    LOG_ERROR("Peer(%s) failed to AppendEntries caz %s.", peer->env_.server.c_str(), result.ToString().c_str());
  }
}

void Peer::AddHeartBeatTask() {
  LOG_DEBUG("Peer(%s) AddHeartBeatTask at heartbeart_us %luus at %lums",
            env_.server.c_str(), env_.context->heartbeat_us(),
            (slash::NowMicros() + env_.context->heartbeat_us()) / 1000LL);
  bg_thread_.DelaySchedule(env_.context->heartbeat_us() / 1000LL,
                           DoHeartBeat, this);
}

void Peer::DoHeartBeat(void *arg) {
  Peer* peer = static_cast<Peer*>(arg);
  LOG_DEBUG("Peer(%s) DoHeartBeat", peer->env_.server.c_str());
  Status result = peer->AppendEntries(true);
  if (!result.ok()) {
    LOG_ERROR("Peer(%s) failed to DoHeartBeat caz %s.", peer->env_.server.c_str(), result.ToString().c_str());
  }
  peer->AddHeartBeatTask();
}

//bool Peer::HaveVote() { return have_vote_; }

uint64_t Peer::GetMatchIndex() {
  slash::MutexLock l(&mu_);
  return match_index_;
}

Status Peer::AppendEntries(bool heartbeat) {
  uint64_t last_log_index = env_.log->GetLastLogIndex();
  uint64_t prev_log_index = next_index_ - 1;
  if (prev_log_index > last_log_index) {
    return Status::InvalidArgument("prev_Log_index > last_log_index");
  }

  uint64_t prev_log_term = 0;
  if (prev_log_index != 0) {
    prev_log_term = env_.log->GetEntry(prev_log_index).term();
  }

  command::Command req;
  floyd::raft::AppendEntriesRequest* aerq = req.mutable_aerq();
  req.set_type(command::Command::RaftAppendEntries);
  aerq->set_ip(env_.context->local_ip());
  aerq->set_port(env_.context->local_port());
  aerq->set_term(env_.context->current_term());
  aerq->set_prev_log_index(prev_log_index);
  aerq->set_prev_log_term(prev_log_term);

  uint64_t num_entries = 0;
  if (!heartbeat) {
    for (uint64_t index = next_index_; index <= last_log_index; ++index) {
      Log::Entry& entry = env_.log->GetEntry(index);
      *aerq->add_entries() = entry;
      uint64_t request_size = aerq->ByteSize();
      if (request_size < env_.context->append_entries_size_once() ||
          num_entries == 0)
        ++num_entries;
      else
        aerq->mutable_entries()->RemoveLast();
    }
  }
  aerq->set_commit_index(
      std::min(env_.context->commit_index(), prev_log_index + num_entries));

#if (LOG_LEVEL != LEVEL_NONE)
  std::string text_format;
  google::protobuf::TextFormat::PrintToString(req, &text_format);
  LOG_DEBUG("AppendEntry Send to %s, message :\n%s", env_.server.c_str(), text_format.c_str());
#endif

  command::CommandRes res;
  Status result = env_.pool->SendAndRecv(env_.server, req, &res);
  
  if (!result.ok()) {
    LOG_DEBUG("AppendEntry to %s failed %s", env_.server.c_str(), result.ToString().c_str());
    return result;
  }
#if (LOG_LEVEL != LEVEL_NONE)
  google::protobuf::TextFormat::PrintToString(res, &text_format);
  LOG_DEBUG("AppendEntry Receive from %s, message :\n%s", env_.server.c_str(), text_format.c_str());
#endif

  uint64_t res_term = res.aers().term();
  if (result.ok() && res_term > env_.context->current_term()) {
    //TODO(anan) maybe combine these 2 steps
    env_.context->BecomeFollower(res_term);
    env_.primary->ResetElectLeaderTimer();
    //env_.primary->AddTask(TaskType::kCheckElectLeader);
    //env_.floyd->ResetLeaderElectTimer();
  }
  
  if (result.ok() && env_.context->role() == Role::kLeader) {
    if (res.aers().status()) {
      match_index_ = prev_log_index + num_entries;
      //TODO(anan) AddTask or direct call
      env_.primary->AdvanceCommitIndex();
      next_index_ = match_index_ + 1;
    } else {
      if (next_index_ > 1) --next_index_;
    }
  }
  return result;
}

} // namespace floyd
