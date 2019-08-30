#include <c10d/ProcessGroup.hpp>
#include <torch/csrc/distributed/rpc/process_group_agent.h>

#include <Python.h>

namespace torch {
namespace distributed {
namespace rpc {

namespace {

// Write the message into the given ostream
void serialize(const Message& message, std::ostream& os) {
  // We cast const void* to void* here because we need to create a tensor using
  // that memory space. If is fine as that tensor stays function-local, and will
  // not be modified during its lifetime.
  auto payload = const_cast<void*>(  // NOLINT
      static_cast<const void*>(message.payload().data()));
  auto payload_size = message.payload().size();

  // getting tensor table from the message
  std::vector<torch::Tensor> tensors = message.tensors();
  // append payload as a tensor
  tensors.push_back(torch::from_blob(payload, payload_size, {torch::kChar}));
  // append id as a tensor
  tensors.push_back(torch::tensor({message.id()}, {torch::kInt64}
  ));

  torch::save(tensors, os);
}

Message deserialize(MessageType type, std::istream& is) {
  std::vector<torch::Tensor> tensors;

  torch::load(tensors, is);

  TORCH_CHECK(tensors.size() >= 2, "Failed to deserialize a message.");
  auto idTensor = std::move(tensors.back());
  tensors.pop_back();
  auto payloadTensor = std::move(tensors.back());
  tensors.pop_back();

  int64_t id = idTensor.storage().data<int64_t>()[0];

  std::vector<char> payload(payloadTensor.numel());

  if (payloadTensor.numel() > 0) {
    std::memcpy(payload.data(),
                payloadTensor.storage().data(),
                payloadTensor.numel());
  }

  return Message(std::move(payload), std::move(tensors), type, id);
}

} // namespace

ProcessGroupAgent::ProcessGroupAgent(
    std::string workerName,
    std::unordered_map<std::string, int> nameMap,
    std::shared_ptr<c10d::ProcessGroup> pg,
    int numSendRecvThreads)
    : RpcAgent(
          WorkerId(std::move(workerName), pg->getRank()),
          processRequestBlocking
      ),
      nameMap_(std::move(nameMap)),
      pg_(std::move(pg)),
      nextId_(0),
      sendMutexes_(pg_->getSize()),
      threadPool_(numSendRecvThreads) {
  TORCH_CHECK(nameMap_.size() > 1, "ProcessGroupAgent requires world_size to "
      "be at least 2, but got ", nameMap_.size());
  auto workerRankIter = nameMap_.find(workerId_.name_);
  TORCH_CHECK(workerRankIter != nameMap_.end(), "Failed to resolve worker "
      "name ", workerId_.name_, " to a ProcessGroup rank.");
  TORCH_CHECK(pg_->getRank() == workerRankIter -> second,
      "Resolved worker rank ", workerRankIter -> second,
      " does not match ProcessGroup rank ", pg_->getRank());

  // tmp vector to sort names in rank's order
  std::vector<std::string> tmpWorkerIds(pg_->getSize());
  for (auto& entry: nameMap_) {
    tmpWorkerIds[entry.second] = entry.first;
  }

  workerIds_.reserve(pg_->getSize());
  for (int rank = 0; rank < (int)tmpWorkerIds.size(); ++rank) {
    workerIds_.emplace_back(std::move(tmpWorkerIds[rank]), rank);
  }

  PythonRpcHandler::init();
  listenerThread_ = std::thread(&ProcessGroupAgent::listenLoop, this);
}

const WorkerId& ProcessGroupAgent::getWorkerId(
    const std::string& workerName) const {
  const auto idIter = nameMap_.find(workerName);
  TORCH_CHECK(idIter != nameMap_.end(),
      "Unknown destination worker ", workerName);

  return workerIds_[idIter->second];
}

void ProcessGroupAgent::join() {
  // Every process i sends a SHUTDOWN message to process i + 1. This is
  // necessary for now because:
  // 1. There is no abort API for ProcessGroup::recvAnysource yet. We have to
  //    feed it a message or kill the thread.
  // 2. A GLOO process cannot send message to itself. (there is an ongoing
  //    effort to fix this problem).
  sync();
  int dst = (pg_->getRank() + 1) % pg_->getSize();
  enqueueSend(
      SendWork(workerIds_[dst], Message({}, {}, MessageType::SHUTDOWN)));
  threadPool_.waitWorkComplete();
  listenerThread_.join();
}

int16_t ProcessGroupAgent::getWorkerId() {
  return pg_->getRank();
}

void ProcessGroupAgent::sync() {
  // Block until all processes wants to sync. This is necessary before acquiring
  // the lock below, because other processes might not enter sync() until it
  // gets some response from this RpcAgent.
  pg_->barrier()->wait();
  // Wait until the all send works are done.
  // NB: There might be additional send works inserted while waiting.
  threadPool_.waitWorkComplete();
  // Use another barrier in case different RpcAgent handles different amounts of
  // workloads.
  pg_->barrier()->wait();
}

std::shared_ptr<FutureMessage> ProcessGroupAgent::send(
    const WorkerId& to, Message&& message) {
  TORCH_CHECK(to.id_ != (worker_id_t)pg_->getRank(),
      "ProcessGroupAgent does not support making RPC calls to self.")
  TORCH_CHECK(to.id_ < (worker_id_t)pg_->getSize(),
      "Destination rank is out of bound, got ", to.id_,
      ", but world size is ", pg_->getRank());

  auto requestId = nextId();
  auto future = std::make_shared<FutureMessage>();
  if (message.isRequest()) {
    {
      std::lock_guard<std::mutex> lock{futureMutex_};
      futures_[requestId] = future;
    }
    message.setId(requestId);
  } else {
    future->markCompleted();
  }

  // NB: cannot directly pass ``to`` to the ``SendWork``, because it might no
  // longer be alive when the ``SendWork`` is executed. For example, the
  // application could query the ``WorkerId`` using name through the
  // ``RpcAgent::getWorkerId`` API, and pass the ``WorkerId`` back here, so we
  // have C++ -> Python -> C++. For an asynchronous RPC, the ``WorkerId``
  // reference on Python side could die before ``SendWork`` uses it, and Pybind
  // will not keep the Python reference alive even if it originally comes from
  // the C++ land. Hence, we have to explicitly use the ``workerId`` in the C++
  // land.
  enqueueSend(SendWork(workerIds_[to.id_], std::move(message)));
  return future;
}

void ProcessGroupAgent::enqueueSend(SendWork work) {
  // NB: this can be changed to use a native move capture when moved to C++14
  threadPool_.run(std::bind(
    [&](const SendWork& work) {
      std::stringstream ss;
      serialize(work.message_, ss);
      std::string serializedPayload = ss.str();

      std::vector<torch::Tensor> preamble = {
        torch::tensor(
          {
            (int64_t)pg_->getRank(),
            (int64_t)serializedPayload.length(),
            (int64_t)work.message_.type()
          }, {torch::kLong})
      };

      // ProcessGroup is not thread-safe when sending with the same tag, hence
      // the lock
      std::vector<std::shared_ptr<c10d::ProcessGroup::Work>> pendingSends;
      const auto& dst = work.to_.id_;
      if (work.message_.isShutdown()) {
        pendingSends.reserve(1);
        std::lock_guard<std::mutex> guard(sendMutexes_[dst]);
        pendingSends.emplace_back(
            pg_->send(preamble, dst, dst /* channelTag */));
      } else {
        std::vector<torch::Tensor> payload = {
            torch::from_blob(
                (void *)serializedPayload.c_str(),
                serializedPayload.length(),
                {torch::kChar}
            )
        };
        pendingSends.reserve(2);
        std::lock_guard<std::mutex> guard(sendMutexes_[dst]);
        pendingSends.emplace_back(
            pg_->send(preamble, dst, dst /* channelTag */));
        pendingSends.emplace_back(
            pg_->send(payload, dst, dst /* channelTag */));
      }
      for (auto& pendingSend: pendingSends) {
        pendingSend->wait();
      }

    },
    std::move(work)
  ));
}

void ProcessGroupAgent::enqueueRecv(RecvWork work) {
  threadPool_.run(std::bind(
    [&](RecvWork& work) {

      torch::Tensor& payload = work.payload_;
      std::stringstream ss(std::string(
        (char*)payload.storage().data<signed char>(), payload.numel()));

      Message message = deserialize(work.type_, ss);

      if (message.isRequest()) {
        auto response = cb_(std::move(message));
        send(work.from_, std::move(response));
      } else if (message.isResponse()) {
        auto id = message.id();
        {
          std::lock_guard<std::mutex> lock{futureMutex_};
          futures_[id]->markCompleted(std::move(message));
          futures_.erase(id);
        }
      } else {
        // TODO: pass the error back to the caller instead of crashing here.
        AT_ERROR("unrecognized message type ", message.type());
      }
    },
    std::move(work)
  ));
}

void ProcessGroupAgent::listenLoop() {
  while (true) {
    // rank, tensor size, message type
    std::vector<torch::Tensor> preamble = {torch::empty({3}, {torch::kInt64})};
    pg_->recvAnysource(preamble, pg_->getRank())->wait();
    int64_t* preamble_items = preamble.front().storage().data<int64_t>();

    auto srcRank = preamble_items[0];
    auto size = preamble_items[1];
    MessageType type = MessageType(preamble_items[2]);

    if (type == MessageType::SHUTDOWN) {
      // FIXME: This LOG also prints warnings no InitGoogleLogging() was invoked
      // before logging, but it is not appropriate to call InitGoogleLogging()
      // here either.
      LOG(INFO) << "Shutting down ProcessGroupAgent "
                << workerId_.name_ << std::endl;
      return;
    }

    std::vector<torch::Tensor> tensors = {torch::empty({size}, {torch::kChar})};
    pg_->recv(tensors, srcRank, pg_->getRank())->wait();

    enqueueRecv(RecvWork(workerIds_[srcRank], type, std::move(tensors[0])));
  }
}

}
}
}
