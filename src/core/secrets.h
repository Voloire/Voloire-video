#pragma once
#include <string>

namespace vv {

// The stream key is a password: it lives encrypted with DPAPI (current user) under
// <settings dir>/secrets/<name>.bin and is never written to config.json or a log.
bool saveSecret(const std::string& name, const std::string& value);
std::string loadSecret(const std::string& name);  // empty when missing or unreadable
void deleteSecret(const std::string& name);

}  // namespace vv
