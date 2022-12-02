// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include <liburing.h>
#include <netinet/in.h>
#include <poll.h>

#include <boost/context/fiber.hpp>
#include <boost/intrusive/list.hpp>
#include <queue>

#include "base/init.h"
#include "base/logging.h"
#include "examples/fiber.h"

ABSL_FLAG(int16_t, port, 8081, "Echo server port");
ABSL_FLAG(uint32_t, size, 512, "Message size");

namespace ctx = boost::context;
using namespace std;

namespace example {

thread_local BaseFiberWrapper* active_fb_wrapper = nullptr;
thread_local BaseFiberWrapper* main_wrapper = nullptr;
constexpr size_t kSizeOfCtx = sizeof(BaseFiberWrapper);  // because of the virtual +8 bytes.

typedef boost::intrusive::list<
    BaseFiberWrapper,
    boost::intrusive::member_hook<BaseFiberWrapper, detail::FiberContextHook,
                                  &BaseFiberWrapper::list_hook>,
    boost::intrusive::constant_time_size<false> >
    FiberWrapperQueue;

thread_local FiberWrapperQueue ready_queue;
thread_local FiberWrapperQueue terminated_queue;

BaseFiberWrapper::BaseFiberWrapper(uint32_t cnt, std::string_view nm) : use_count_(cnt) {
  size_t len = std::min(nm.size(), sizeof(name_) - 1);
  name_[len] = 0;
  if (len) {
    memcpy(name_, nm.data(), len);
  }
}

BaseFiberWrapper::~BaseFiberWrapper() {
  DVLOG(1) << "~BaseFiberWrapper";
}

void BaseFiberWrapper::Suspend() {
  main_wrapper->Resume();
}

void BaseFiberWrapper::Terminate() {
  DCHECK(this == active_fb_wrapper);
  DCHECK(!list_hook.is_linked());

  if (this != main_wrapper) {
    terminated_queue.push_back(*this);
    main_wrapper->Resume();
  }
}

void BaseFiberWrapper::Resume() {
  BaseFiberWrapper* prev = this;

  std::swap(active_fb_wrapper, prev);

  // pass pointer to the context that resumes `this`
  std::move(c_).resume_with([prev](ctx::fiber_context&& c) {
    DCHECK(!prev->c_);

    prev->c_ = std::move(c);  // update the return address in the context we just switch from.
    return ctx::fiber_context{};
  });
}

void BaseFiberWrapper::StartMain() {
  DCHECK(!active_fb_wrapper);
  active_fb_wrapper = this;

  std::move(c_).resume();
}

void BaseFiberWrapper::SetReady() {
  DCHECK(!list_hook.is_linked());
  ready_queue.push_back(*this);
}

Fiber::~Fiber() {
  CHECK(!joinable());
}

Fiber& Fiber::operator=(Fiber&& other) noexcept {
  CHECK(!joinable());

  if (this == &other) {
    return *this;
  }

  impl_.swap(other.impl_);
  return *this;
}

void Fiber::Start() {
  impl_->SetReady();
}

void Fiber::Detach() {
  impl_.reset();
}

}  // namespace example

namespace {

struct signal_state {
  struct Item {
    std::function<void(int)> cb;
  };

  Item signal_map[_NSIG];
};

signal_state* get_signal_state() {
  static signal_state state;

  return &state;
}

void SigAction(int signal, siginfo_t*, void*) {
  signal_state* state = get_signal_state();
  DCHECK_LT(signal, _NSIG);

  auto& item = state->signal_map[signal];

  if (item.cb) {
    item.cb(signal);
  } else {
    LOG(ERROR) << "Tangling signal handler " << signal;
  }
}

void RegisterSignal(std::initializer_list<uint16_t> l, std::function<void(int)> cb) {
  auto* state = get_signal_state();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  if (cb) {
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &SigAction;

    for (uint16_t val : l) {
      CHECK(!state->signal_map[val].cb) << "Signal " << val << " was already registered";
      state->signal_map[val].cb = cb;
      CHECK_EQ(0, sigaction(val, &sa, NULL));
    }
  } else {
    sa.sa_handler = SIG_DFL;

    for (uint16_t val : l) {
      CHECK(state->signal_map[val].cb) << "Signal " << val << " was already registered";
      state->signal_map[val].cb = nullptr;
      CHECK_EQ(0, sigaction(val, &sa, NULL));
    }
  }
}

using IoResult = int;
using namespace example;

struct CqeResult {
  int32_t res;
  uint32_t flags;
};

struct ResumeablePoint {
  BaseFiberWrapper* cntx = nullptr;
  union {
    CqeResult* cqe_res;
    uint32_t next;
  };

  ResumeablePoint() : cqe_res(nullptr) {
  }
};

std::vector<ResumeablePoint> suspended_list;

uint32_t next_rp_id = UINT32_MAX;
bool loop_exit = false;

uint32_t NextRp() {
  ResumeablePoint* rp = &suspended_list[next_rp_id];
  CHECK_LT(rp->next, suspended_list.size());
  uint32_t res = next_rp_id;
  next_rp_id = rp->next;

  return res;
}

CqeResult SuspendMyself(io_uring_sqe* sqe) {
  DVLOG(1) << "SuspendMyself";

  CqeResult cqe_result;
  uint32_t myindex = NextRp();
  sqe->user_data = myindex;

  auto& rp = suspended_list[myindex];
  rp.cqe_res = &cqe_result;
  rp.cntx = active_fb_wrapper;

  DCHECK(main_wrapper);
  main_wrapper->Resume();

  return cqe_result;
}

// return 0 if succeeded.
int Recv(int fd, uint8_t* buf, size_t len, io_uring* ring) {
  DCHECK_GE(fd, 0);

  CqeResult cqe_result;
  size_t offs = 0;
  while (len > 0) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd, buf + offs, len, 0);
    cqe_result = SuspendMyself(sqe);

    if (cqe_result.res > 0) {
      CHECK_LE(uint32_t(cqe_result.res), len);
      len -= cqe_result.res;
      offs += cqe_result.res;
      continue;
    }

    if (cqe_result.res == 0) {
      return ECONNABORTED;
    }

    if (-cqe_result.res == EAGAIN) {
      continue;
    }

    return -cqe_result.res;
  }

  return 0;
}

int Send(int fd, const uint8_t* buf, size_t len, io_uring* ring) {
  CqeResult cqe_result;

  size_t offs = 0;
  while (len > 0) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd, buf + offs, len, MSG_NOSIGNAL);
    cqe_result = SuspendMyself(sqe);

    DCHECK(main_wrapper);

    if (cqe_result.res >= 0) {
      CHECK_LE(uint32_t(cqe_result.res), len);
      len -= cqe_result.res;
      offs += cqe_result.res;
      continue;
    }

    return -cqe_result.res;
  }

  return 0;
}

void HandleSocket(io_uring* ring, int fd) {
  DVLOG(1) << "Handle socket";
  uint32_t size = absl::GetFlag(FLAGS_size);
  std::unique_ptr<uint8_t[]> buf(new uint8_t[size]);

  while (true) {
    DVLOG(1) << "Trying to recv from socket " << fd;
    int res = Recv(fd, buf.get(), size, ring);

    if (res > 0) {  // error
      break;
    }

    DVLOG(1) << "After recv from socket " << fd;
    res = Send(fd, buf.get(), size, ring);
    if (res > 0) {
      break;
    }
  }

  close(fd);
}

void AcceptFiber(io_uring* ring, int listen_fd) {
  VLOG(1) << "AcceptFiber";

  CqeResult cqe_result;

  while (true) {
    int res = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (res >= 0) {
      VLOG(1) << "Accepted fd " << res;
      // sockets.emplace_back();
      // sockets.back().fd = res;

      // new fiber
      Fiber("conn", [ring, res] { HandleSocket(ring, res); }).Detach();

      DVLOG(1) << "After HandleSocket fd " << res;
    } else if (errno == EAGAIN) {
      io_uring_sqe* sqe = io_uring_get_sqe(ring);
      io_uring_prep_poll_add(sqe, listen_fd, POLLIN);

      // switch back to the main loop.
      DVLOG(1) << "Resuming to main from AcceptFiber";
      cqe_result = SuspendMyself(sqe);

      DCHECK(main_wrapper);
      DVLOG(1) << "Resuming AcceptFiber after main";

      if ((cqe_result.res & (POLLERR | POLLHUP)) != 0) {
        LOG(ERROR) << "aborted " << cqe_result.res;
        loop_exit = true;
        break;
      }
    } else {
      LOG(ERROR) << "Error " << strerror(errno);
      loop_exit = true;
      break;
    }
  }
};

void RunReadyFbs() {
  while (!ready_queue.empty()) {
    BaseFiberWrapper* cntx = &ready_queue.front();
    ready_queue.pop_front();

    cntx->Resume();
  }
}

void RunEventLoop(io_uring* ring) {
  VLOG(1) << "RunEventLoop";

  io_uring_cqe* cqe = nullptr;
  RunReadyFbs();
  while (!loop_exit) {
    int num_submitted = io_uring_submit(ring);
    CHECK_GE(num_submitted, 0);
    uint32_t head = 0;
    unsigned i = 0;
    io_uring_for_each_cqe(ring, head, cqe) {
      uint32_t index = cqe->user_data;
      CHECK_LT(index, suspended_list.size());

      auto* cqe_res = suspended_list[index].cqe_res;
      cqe_res->res = cqe->res;
      cqe_res->flags = cqe->flags;
      suspended_list[index].next = next_rp_id;
      next_rp_id = index;

      suspended_list[index].cntx->SetReady();
      ++i;
    }
    io_uring_cq_advance(ring, i);

    if (i) {
      RunReadyFbs();
      continue;
    }

    io_uring_wait_cqe_nr(ring, &cqe, 1);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  RegisterSignal({SIGINT, SIGTERM}, [](int signal) {
    LOG(INFO) << "Exiting on signal " << strsignal(signal);
    loop_exit = true;
  });

  // setup iouring
  io_uring ring;
  io_uring_params params;
  memset(&params, 0, sizeof(params));
  CHECK_EQ(0, io_uring_queue_init_params(512, &ring, &params));
  int res = io_uring_register_ring_fd(&ring);
  VLOG_IF(1, res < 0) << "io_uring_register_ring_fd failed: " << -res;

  suspended_list.resize(1024);
  for (size_t i = 0; i < suspended_list.size(); ++i) {
    suspended_list[i].next = i + 1;
  }
  next_rp_id = 0;

  // setup listening socket
  uint16_t port = absl::GetFlag(FLAGS_port);

  int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  CHECK_GE(listen_fd, 0);

  const int val = 1;
  CHECK_EQ(0, setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)));

  sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  res = bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  CHECK_EQ(0, res);
  res = listen(listen_fd, 32);
  CHECK_EQ(0, res);

  main_wrapper =
      detail::MakeCustomFiberWrapper("main", ctx::fixedsize_stack(), [&ring] { RunEventLoop(&ring); });
  DCHECK(main_wrapper->IsDefined());

  DCHECK(ready_queue.empty());
  Fiber accept_fb("accept", [&] { AcceptFiber(&ring, listen_fd); });
  accept_fb.Detach();

  DCHECK(!ready_queue.empty());

  main_wrapper->StartMain();  // blocking

  close(listen_fd);
  io_uring_queue_exit(&ring);
  terminated_queue.clear();  // TODO: there is a resource leak here.

  return 0;
}