#pragma once
#include <functional>
#include <string>

namespace lifecycle {
enum class Action { START, BOOT, STOP, REBOOT };
struct Ticket { std::string revision; bool boot = false; };
// Intent changes are short transactions, separate from the operation lock.
// A newer ticket invalidates late children even before their PID is published.
bool begin(const std::string &root, Action action, Ticket &ticket, std::string &error);
bool current(const std::string &root, const Ticket &ticket);
bool enable(const std::string &root, const Ticket &ticket, std::string &error);
// Callback publishes PID while holding the intent lock. Never run the daemon
// in this callback: STOP must be able to acquire the lock and cancel/signal it.
bool publish(const std::string &root, const Ticket &ticket,
             const std::function<bool()> &publish_pid, std::string &error);
bool set_autostart(const std::string &root, bool on, std::string &error);
}
