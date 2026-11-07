#pragma once
// The Kindle's property bus (power, Wi-Fi, keyboard). Desktop builds get a no-op version.

#include <memory>
#include <string>

namespace zk {

class Lipc {
public:
    virtual ~Lipc() = default;
    virtual std::string get(const std::string& publisher, const std::string& prop) = 0;
    virtual bool set(const std::string& publisher, const std::string& prop, const std::string& value) = 0;
};

std::unique_ptr<Lipc> makeLipc();

}  // namespace zk
