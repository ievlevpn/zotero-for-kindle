#include "platform/lipc.h"

namespace zk {

namespace {

class NullLipc final : public Lipc {
public:
    std::string get(const std::string&, const std::string&) override { return {}; }
    bool set(const std::string&, const std::string&, const std::string&) override { return true; }
};

}  // namespace

std::unique_ptr<Lipc> makeLipc() { return std::make_unique<NullLipc>(); }

}  // namespace zk
