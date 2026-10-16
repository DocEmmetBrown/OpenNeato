#include "../components/openneato/neato_client.h"
#include "test_framework.h"

#include <deque>
#include <map>

using namespace neatocore;

namespace {

uint32_t g_now = 1000;
uint32_t fake_clock() { return g_now; }

// Scripted robot: answers written commands with "<cmd>\r\n<body>\x1A".
class FakeRobot : public Transport {
 public:
  int read_byte() override {
    if (this->rx.empty())
      return -1;
    int c = static_cast<unsigned char>(this->rx.front());
    this->rx.pop_front();
    return c;
  }
  void write(const std::string &data) override {
    std::string cmd = data.substr(0, data.size() - 1);  // strip '\n'
    this->written.push_back(cmd);
    if (this->silent)
      return;
    auto it = this->replies.find(cmd);
    std::string body = it != this->replies.end() ? it->second : "";
    for (char c : cmd + "\r\n" + body)
      this->rx.push_back(c);
    this->rx.push_back(0x1A);
  }
  std::deque<char> rx;
  std::vector<std::string> written;
  std::map<std::string, std::string> replies;
  bool silent = false;
};

void run(NeatoClient &client, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    g_now += 10;
    client.tick();
  }
}

void run_queue(SerialQueue &q, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    g_now += 10;
    q.tick(g_now);
  }
}

const char *VERSION_REPLY = "Model,BotVacD5Connected,905-0001,\r\nSerial Number,OPS01234-5678,0123456789AB,P,\r\n"
                            "Software,4,5,3,\r\n";

}  // namespace

TEST(queue_frames_responses_and_respects_priority) {
  FakeRobot robot;
  robot.replies["GetState"] = "Current UI State is: UIMGR_STATE_IDLE\r\n";
  SerialQueue q(&robot);
  std::vector<std::string> order;
  q.enqueue("GetCharger", [&](CmdStatus s, const std::string &) { order.push_back("charger:" + std::string(cmd_status_str(s))); });
  q.enqueue("GetState", [&](CmdStatus s, const std::string &r) {
    order.push_back("state:" + std::string(cmd_status_str(s)));
    CHECK(r.find("UIMGR_STATE_IDLE") != std::string::npos);
    CHECK(r.find('\x1A') == std::string::npos);
  });
  q.enqueue("TestMode On", [&](CmdStatus, const std::string &) { order.push_back("testmode"); }, PRIORITY_HIGH);
  run_queue(q, 500);
  CHECK_EQ(order.size(), static_cast<size_t>(3));
  if (order.size() == 3) {
    // HIGH jumps the queue, NORMAL stays FIFO.
    CHECK_STR(order[0], "testmode");
    CHECK_STR(order[1], "charger:ok");
    CHECK_STR(order[2], "state:ok");
  }
  CHECK(!q.is_busy());
}

TEST(queue_inter_command_gap) {
  FakeRobot robot;
  SerialQueue q(&robot);
  q.enqueue("GetState", nullptr);
  q.enqueue("GetErr", nullptr);
  q.tick(g_now);  // sends GetState
  q.tick(g_now);  // receives the reply
  CHECK_EQ(robot.written.size(), static_cast<size_t>(1));
  g_now += INTER_CMD_DELAY_MS - 1;
  q.tick(g_now);
  CHECK_EQ(robot.written.size(), static_cast<size_t>(1));
  g_now += 1;
  q.tick(g_now);
  CHECK_EQ(robot.written.size(), static_cast<size_t>(2));
}

TEST(queue_timeout_and_desync) {
  FakeRobot robot;
  robot.silent = true;
  SerialQueue q(&robot);
  CmdStatus got = CmdStatus::SUCCESS;
  q.enqueue("GetState", [&](CmdStatus s, const std::string &) { got = s; });
  run_queue(q, CMD_TIMEOUT_MS - 100);
  CHECK(got == CmdStatus::SUCCESS);
  run_queue(q, 200);
  CHECK(got == CmdStatus::TIMEOUT);

  // Reply echoing another command = desync.
  robot.silent = true;
  q.enqueue("GetErr", [&](CmdStatus s, const std::string &) { got = s; });
  run_queue(q, 100);
  for (char c : std::string("GetCharger\r\nFuelPercent,50\r\n\x1A"))
    robot.rx.push_back(c);
  run_queue(q, 20);
  CHECK(got == CmdStatus::SERIAL_ERROR);

  CHECK(SerialQueue::validate_echo("Clean House", "Clean\r\n"));
  CHECK(SerialQueue::validate_echo("testmode on", "TestMode On\r\n"));
  CHECK(!SerialQueue::validate_echo("GetState", "GetErr\r\n"));
}

TEST(queue_unsupported_and_full) {
  FakeRobot robot;
  robot.replies["Foo"] = "Unknown Cmd: 'Foo'\r\n";
  SerialQueue q(&robot);
  CmdStatus got = CmdStatus::SUCCESS;
  q.enqueue("Foo", [&](CmdStatus s, const std::string &) { got = s; });
  run_queue(q, 100);
  CHECK(got == CmdStatus::UNSUPPORTED);

  int full = 0;
  for (size_t i = 0; i < QUEUE_MAX_SIZE + 2; i++)
    q.enqueue("GetState", [&](CmdStatus s, const std::string &) {
      if (s == CmdStatus::QUEUE_FULL)
        full++;
    });
  CHECK_EQ(full, 2);
}

TEST(cache_coalesces_and_respects_ttl) {
  FakeRobot robot;
  robot.replies["GetCharger"] = "FuelPercent,77\r\n";
  NeatoClient client(&robot, fake_clock);
  robot.silent = true;  // keep the identify attempt from interfering
  run(client, 10);
  robot.silent = false;
  robot.written.clear();
  run(client, CMD_TIMEOUT_MS + 100);  // identify times out
  robot.written.clear();

  int calls = 0;
  int fuel = 0;
  client.get_charger([&](bool ok, const ChargerData &c) {
    calls += ok;
    fuel = c.fuel_percent;
  });
  client.get_charger([&](bool ok, const ChargerData &) { calls += ok; });
  run(client, 200);
  CHECK_EQ(calls, 2);
  CHECK_EQ(fuel, 77);
  size_t sent = 0;
  for (auto &w : robot.written)
    sent += w == "GetCharger";
  CHECK_EQ(sent, static_cast<size_t>(1));
  // Within the TTL: served from cache synchronously.
  client.get_charger([&](bool ok, const ChargerData &) { calls += ok; });
  CHECK_EQ(calls, 3);
}

TEST(client_identifies_with_backoff_and_cleans_with_setevent) {
  FakeRobot robot;
  robot.silent = true;
  NeatoClient client(&robot, fake_clock);
  int identified = 0;
  client.on_identified([&](const VersionData &v) {
    identified++;
    CHECK_STR(v.model_name, "BotVacD5Connected");
  });
  CHECK(!client.clean("house"));  // no SKey yet
  run(client, CMD_TIMEOUT_MS + 100);
  CHECK(!client.has_skey());
  size_t attempts = robot.written.size();
  run(client, 1500);  // backoff 2 s: no retry yet
  CHECK_EQ(robot.written.size(), attempts);
  robot.silent = false;
  robot.replies["GetVersion"] = VERSION_REPLY;
  run(client, 1000);
  CHECK_EQ(identified, 1);
  CHECK_STR(client.skey(), "b20e9dfade296f1210da702bf");

  // New house clean: nav mode first, then SetEvent start.
  int started = 0;
  client.on_clean_start([&]() { started++; });
  client.set_nav_mode_getter([]() { return std::string("Gentle"); });
  robot.written.clear();
  bool done = false;
  CHECK(client.clean("house", [&](bool ok) { done = ok; }));
  run(client, 300);
  CHECK(done);
  CHECK_EQ(started, 1);
  CHECK_EQ(robot.written.size(), static_cast<size_t>(2));
  if (robot.written.size() == 2) {
    CHECK_STR(robot.written[0], "SetNavigationMode Gentle");
    CHECK_STR(robot.written[1],
              "SetEvent event UIMGR_EVENT_SMARTAPP_START_HOUSE_CLEANING SKey b20e9dfade296f1210da702bf");
  }

  // Paused house clean: "house" resumes, without nav mode / clean-start hook.
  robot.replies["GetState"] = "Current UI State is: UIMGR_STATE_HOUSECLEANINGPAUSED\r\n";
  client.get_state(nullptr);
  run(client, 200);
  robot.written.clear();
  CHECK(client.clean("house"));
  run(client, 200);
  CHECK_EQ(started, 1);
  CHECK_EQ(robot.written.size(), static_cast<size_t>(1));
  if (!robot.written.empty())
    CHECK(robot.written[0].find("RESUME_CLEANING") != std::string::npos);

  robot.written.clear();
  CHECK(client.clean("dock"));
  CHECK(!client.clean("bogus"));
  run(client, 200);
  CHECK(robot.written.size() == 1 && robot.written[0].find("SEND_TO_BASE") != std::string::npos);
}

TEST(client_manual_state_patch_and_power_control) {
  FakeRobot robot;
  robot.replies["GetVersion"] = VERSION_REPLY;
  robot.replies["GetState"] = "Current UI State is: UIMGR_STATE_TESTMODE\r\n";
  NeatoClient client(&robot, fake_clock);
  run(client, 200);
  client.set_manual_clean_active(true);
  std::string ui;
  client.get_state([&](bool, const RobotState &s) { ui = s.ui_state; });
  run(client, 200);
  CHECK_STR(ui, "UIMGR_STATE_MANUALCLEANING");

  robot.written.clear();
  bool ok = false;
  client.power_control("restart", [&](bool r) { ok = r; });
  run(client, 300);
  CHECK(ok);
  CHECK(robot.written.size() == 2 && robot.written[0] == "TestMode On" &&
        robot.written[1] == "SetSystemMode PowerCycle");

  robot.written.clear();
  client.set_motor_wheels(0, 0, 0);
  client.set_motor_wheels(20000, -50, 999);
  run(client, 300);
  CHECK(robot.written.size() == 3);
  if (robot.written.size() == 3) {
    CHECK_STR(robot.written[0], "SetMotor LWheelDisable RWheelDisable");
    // The re-enable runs CRITICAL before the queued move.
    CHECK_STR(robot.written[1], "SetMotor LWheelEnable RWheelEnable");
    CHECK_STR(robot.written[2], "SetMotor LWheelDist 10000 RWheelDist -50 Speed 300");
  }
  CHECK(!client.send_raw("bad\ncmd", nullptr));
  CHECK(!client.set_user_setting("Eco Mode", "ON"));
  CHECK(client.set_user_setting("EcoMode", "ON"));
}

TEST(client_explore_sends_clean_explore) {
  FakeRobot robot;
  robot.replies["GetVersion"] = VERSION_REPLY;
  NeatoClient client(&robot, fake_clock);
  run(client, 1000);
  CHECK(client.has_skey());
  int started = 0;
  client.on_clean_start([&]() { started++; });
  client.set_nav_mode_getter([]() { return std::string("Deep"); });
  robot.written.clear();
  bool done = false;
  CHECK(client.clean("explore", [&](bool ok) { done = ok; }));
  run(client, 300);
  CHECK(done);
  CHECK_EQ(started, 1);
  // No navigation mode and no SetEvent: just the exploration run.
  CHECK_EQ(robot.written.size(), static_cast<size_t>(1));
  if (robot.written.size() == 1)
    CHECK_STR(robot.written[0], "Clean Explore");
  CHECK(!client.clean("bogus"));
}
