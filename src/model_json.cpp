#include "model_json.hpp"

#include "time_util.hpp"

namespace mnemon {

nlohmann::json insight_to_json(const Insight& i) {
  nlohmann::json j;
  j["id"] = i.id;
  j["content"] = i.content;
  j["category"] = i.category;
  j["importance"] = i.importance;
  j["tags"] = i.tags;
  j["entities"] = i.entities;
  j["source"] = i.source;
  j["access_count"] = i.access_count;
  j["created_at"] = time_util::rfc3339_utc(i.created_at);
  j["updated_at"] = time_util::rfc3339_utc(i.updated_at);
  if (i.deleted_at) {
    j["deleted_at"] = time_util::rfc3339_utc(*i.deleted_at);
  }
  return j;
}

nlohmann::json edge_metadata_json(const std::map<std::string, std::string>& m) {
  return nlohmann::json(m);
}

// Edge metadata in DB is JSON object; only string values are loaded (matches MAGMA edge metadata model).
void parse_metadata(const std::string& s, std::map<std::string, std::string>& out) {
  out.clear();
  if (s.empty() || s == "{}") {
    return;
  }
  auto j = nlohmann::json::parse(s, nullptr, false);
  if (!j.is_object()) {
    return;
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (it.value().is_string()) {
      out[it.key()] = it.value().get<std::string>();
    }
  }
}

} // namespace mnemon
