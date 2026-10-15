// Ported from OpenNeato firmware/src/neato_serial.cpp (MIT)
#include "neato_serial_queue.h"

#include <cctype>

namespace neatocore {

const char *cmd_status_str(CmdStatus status) {
  switch (status) {
    case CmdStatus::SUCCESS:
      return "ok";
    case CmdStatus::TIMEOUT:
      return "timeout";
    case CmdStatus::SERIAL_ERROR:
      return "serial error";
    case CmdStatus::UNSUPPORTED:
      return "unsupported command";
    case CmdStatus::QUEUE_FULL:
      return "queue full";
  }
  return "unknown";
}

bool SerialQueue::enqueue(const std::string &command, RawCallback callback, Priority priority,
                          bool wait_for_response) {
  if (this->queue_.size() >= QUEUE_MAX_SIZE) {
    if (this->logger_)
      this->logger_(command, CmdStatus::QUEUE_FULL, 0, 0);
    if (callback)
      callback(CmdStatus::QUEUE_FULL, "");
    return false;
  }
  // Keep FIFO order within the same priority.
  auto it = this->queue_.begin();
  for (; it != this->queue_.end(); ++it) {
    if (it->priority > priority)
      break;
  }
  this->queue_.insert(it, Entry{command, static_cast<uint8_t>(priority), wait_for_response, std::move(callback)});
  return true;
}

void SerialQueue::flush_rx_() {
  while (this->transport_->read_byte() >= 0) {
  }
}

void SerialQueue::send_next_(uint32_t now) {
  Entry entry = std::move(this->queue_.front());
  this->queue_.erase(this->queue_.begin());
  this->current_command_ = std::move(entry.command);
  this->current_callback_ = std::move(entry.callback);
  this->response_.clear();

  // Drop stale bytes from a late previous response.
  this->flush_rx_();
  this->transport_->write(this->current_command_ + "\n");
  this->sent_at_ = now;
  if (!entry.wait_for_response) {
    this->complete_(CmdStatus::SUCCESS, now);
    return;
  }
  this->state_ = State::WAITING;
}

void SerialQueue::tick(uint32_t now) {
  switch (this->state_) {
    case State::IDLE:
      if (!this->queue_.empty())
        this->send_next_(now);
      break;

    case State::WAITING: {
      int c;
      while ((c = this->transport_->read_byte()) >= 0) {
        if (c == RESPONSE_TERMINATOR) {
          if (!validate_echo(this->current_command_, this->response_)) {
            // Desynced stream: the reply belongs to another command.
            this->flush_rx_();
            this->complete_(CmdStatus::SERIAL_ERROR, now);
            return;
          }
          if (this->response_.find("Unknown Cmd") != std::string::npos) {
            this->complete_(CmdStatus::UNSUPPORTED, now);
            return;
          }
          this->complete_(CmdStatus::SUCCESS, now);
          return;
        }
        this->response_ += static_cast<char>(c);
      }
      if (now - this->sent_at_ >= CMD_TIMEOUT_MS) {
        this->flush_rx_();
        this->complete_(CmdStatus::TIMEOUT, now);
      }
      break;
    }

    case State::INTER_DELAY:
      if (now - this->delay_started_at_ >= INTER_CMD_DELAY_MS) {
        this->state_ = State::IDLE;
        if (!this->queue_.empty())
          this->send_next_(now);
      }
      break;
  }
}

void SerialQueue::complete_(CmdStatus status, uint32_t now) {
  std::string cmd = std::move(this->current_command_);
  RawCallback cb = std::move(this->current_callback_);
  std::string resp;
  resp.swap(this->response_);
  this->current_command_.clear();
  this->current_callback_ = nullptr;
  this->delay_started_at_ = now;
  this->state_ = State::INTER_DELAY;

  if (this->logger_)
    this->logger_(cmd, status, now - this->sent_at_, resp.size());
  if (cb)
    cb(status, resp);
}

bool SerialQueue::validate_echo(const std::string &command, const std::string &response) {
  // The robot echoes the command on the first line; some echo only the name
  // ("Clean" for "Clean House"), so compare the first word, case-insensitive.
  std::string expected = command.substr(0, command.find(' '));
  size_t start = 0;
  while (start < response.size() && isspace(static_cast<unsigned char>(response[start])))
    start++;
  if (response.size() - start < expected.size())
    return false;
  for (size_t i = 0; i < expected.size(); i++) {
    if (tolower(static_cast<unsigned char>(response[start + i])) != tolower(static_cast<unsigned char>(expected[i])))
      return false;
  }
  return true;
}

}  // namespace neatocore
