#pragma once
// Records lipc property writes and serves reads from a map, for tests.

#include "platform/lipc.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace zk::test {

class FakeLipc : public Lipc {
public:
    std::string get(const std::string& publisher, const std::string& prop) override {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = props.find(publisher + " " + prop);
        return it == props.end() ? std::string{} : it->second;
    }
    bool set(const std::string& publisher, const std::string& prop, const std::string& value) override {
        std::lock_guard<std::mutex> lock(mutex);
        sets.push_back(publisher + " " + prop + " " + value);
        return true;
    }
    std::vector<std::string> setsCopy() {
        std::lock_guard<std::mutex> lock(mutex);
        return sets;
    }

    std::map<std::string, std::string> props;  // "publisher prop" -> value
    std::vector<std::string> sets;             // "publisher prop value"
    std::mutex mutex;
};

}  // namespace zk::test
