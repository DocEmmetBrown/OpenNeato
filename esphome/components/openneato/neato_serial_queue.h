// Ported from OpenNeato firmware/src/neato_serial.{h,cpp} (MIT)
//
// Priority command queue for the Neato serial protocol: one command in flight,
// responses framed by 0x1A (Ctrl-Z), echo validation, per-command timeout and a
// fixed gap between commands. Pure C++17: the UART and the clock are injected.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace neatocore {

static const uint32_t CMD_TIMEOUT_MS = 3000;
static const uint32_t INTER_CMD_DELAY_MS = 50;
static const size_t QUEUE_MAX_SIZE = 16;
static const char RESPONSE_TERMINATOR = 0x1A;

enum class CmdStatus : uint8_t { SUCCESS, TIMEOUT, SERIAL_ERROR, UNSUPPORTED, QUEUE_FULL };
const char *cmd_status_str(CmdStatus status);

// Lower number = higher priority.
enum Priority : uint8_t {
  PRIORITY_URGENT = 0,  // follow-up that must precede everything already queued
  PRIORITY_CRITICAL = 1,
  PRIORITY_HIGH = 2,
  PRIORITY_MEDIUM = 3,
  PRIORITY_NORMAL = 4,
};

class Transport {
 public:
  virtual ~Transport() = default;
  // Next received byte, or -1 when none is pending.
  virtual int read_byte() = 0;
  virtual void write(const std::string &data) = 0;
};

using RawCallback = std::function<void(CmdStatus, const std::string &)>;
// (command, status, elapsed_ms, response_bytes)
using CommandLogger = std::function<void(const std::string &, CmdStatus, uint32_t, size_t)>;

class SerialQueue {
 public:
  explicit SerialQueue(Transport *transport) : transport_(transport) {}

  // Returns false (and calls back with QUEUE_FULL) when the queue is full.
  bool enqueue(const std::string &command, RawCallback callback, Priority priority = PRIORITY_NORMAL,
               bool wait_for_response = true);

  // Drive the state machine; call often (every main-loop iteration).
  void tick(uint32_t now);

  bool is_busy() const { return this->state_ != State::IDLE || !this->queue_.empty(); }
  bool is_waiting_response() const { return this->state_ == State::WAITING; }
  size_t depth() const { return this->queue_.size(); }
  void set_logger(CommandLogger logger) { this->logger_ = std::move(logger); }

  // Exposed for tests: does `response`'s first line echo `command`?
  static bool validate_echo(const std::string &command, const std::string &response);

 protected:
  enum class State : uint8_t { IDLE, WAITING, INTER_DELAY };
  struct Entry {
    std::string command;
    uint8_t priority;
    bool wait_for_response;
    RawCallback callback;
  };

  void send_next_(uint32_t now);
  void complete_(CmdStatus status, uint32_t now);
  void flush_rx_();

  Transport *transport_;
  std::vector<Entry> queue_;
  State state_{State::IDLE};
  std::string current_command_;
  RawCallback current_callback_;
  std::string response_;
  uint32_t sent_at_{0};
  uint32_t delay_started_at_{0};
  CommandLogger logger_;
};

}  // namespace neatocore
